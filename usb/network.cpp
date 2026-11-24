// The USB network (usb/network.h): the board as a CDC-NCM network adapter, an esp_netif on lwIP.
//
// Data path. From the computer: TinyUSB hands each datagram of an NTB to tud_network_recv_cb on the USB task; it
// is copied into an lwIP pbuf and posted to lwIP's thread (tcpip_input never waits), and tud_network_recv_renew
// asks for the next one at once (TinyUSB guards that re-entry: one NTB is drained in one go). To the computer:
// lwIP's thread copies each frame into one of four transmit slots (waiting at most 10 ms for one, else dropping
// the frame, as a full Ethernet queue would) and wakes the USB task (usbd_defer_func), which moves the slots into
// NTBs with tud_network_can_xmit / tud_network_xmit; it also tries every pass of its loop (detail::NetworkPump),
// so a slot that found every NTB busy goes as soon as one is free. Nothing waits on the USB task.
//
// Link. The USB task follows the cable, the host's sleep and the class being presented; each change wakes the
// network's task, which brings the interface up or down (DHCP server or client) and announces <name>.local.
//
// Nothing of it is linked into a firmware built without CONFIG_CARDENZA_HAL_USB_NETWORK.
#include "usb/network.h"

#include <cstdint>
#include <cstring>

#include "usb/internal/dev.h"
#include "usb/network_logic.h"
#include "usb/usb.h"
#include "tusb.h"

#if CFG_TUD_NCM
#include <atomic>
#include <cstdio>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/esp_netif_net_stack.h>
#include <lwip/igmp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#include <dhcpserver/dhcpserver.h>

#include "device/usbd_pvt.h"
#endif

namespace CardenzaHAL::USB {

namespace {

namespace NetLogic = NetworkLogic;

uint8_t g_deviceMac[6] = {};
uint8_t g_hostMac[6] = {};
char g_hostMacString[13] = {};

// The two MACs of the link, from the chip's Ethernet MAC (unique, never used by the Wi-Fi or Bluetooth MACs).
void LoadMacs()
{
    if (g_hostMacString[0] != '\0') return;
#if CFG_TUD_NCM
    esp_read_mac(g_deviceMac, ESP_MAC_ETH);
#endif
    NetLogic::HostMac(g_deviceMac, g_hostMac);
    NetLogic::MacString(g_hostMac, g_hostMacString);
}

}  // namespace

namespace detail {

#if CFG_TUD_NCM
std::size_t NetworkDescriptor(uint8_t* out, std::size_t capacity, uint8_t itf, uint8_t stringIndex, uint8_t macStringIndex,
                              uint8_t epNotify, uint8_t epOut, uint8_t epIn)
{
    LoadMacs();
    // 64-byte packets both ways (full speed), the notification endpoint 64 (the DWC2's smallest TX FIFO is 16
    // words), datagrams up to an Ethernet frame (CFG_TUD_NET_MTU, 1514): two fit an NTB of 3200.
    const uint8_t block[] = {TUD_CDC_NCM_DESCRIPTOR(itf, stringIndex, macStringIndex, epNotify, 64, epOut, epIn, 64, CFG_TUD_NET_MTU)};
    if (sizeof(block) > capacity) return 0;
    std::memcpy(out, block, sizeof(block));
    return sizeof(block);
}
#else
std::size_t NetworkDescriptor(uint8_t*, std::size_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) { return 0; }
#endif

const char* NetworkMacString()
{
    LoadMacs();
    return g_hostMacString;
}

}  // namespace detail

namespace Network {

#if CFG_TUD_NCM

namespace {

constexpr char kTag[] = "usb-net";
constexpr std::size_t kSlots = 4;
constexpr std::size_t kSlotBytes = 1536;
constexpr int64_t kTransmitWaitUs = 10000;
constexpr uint32_t kQuit = 1, kLinkChanged = 2, kStateChanged = 4;
constexpr int kDeviceRoutePrio = 1;     // never the default route while anything else is up
constexpr int kUplinkRoutePrio = 50;    // below Wi-Fi's station (100)
constexpr int kUplinkFirstRoutePrio = 150;

std::atomic<esp_netif_t*> g_netif{nullptr};
std::atomic<bool> g_active{false};   // started: the USB task moves frames and follows the link
std::atomic<bool> g_usbUp{false};    // the USB task's view of the link
std::atomic<TaskHandle_t> g_task{nullptr};
std::atomic<Listener> g_listener{nullptr};
SemaphoreHandle_t g_rxLock = nullptr;   // the USB task holds it while it hands a frame to the interface
SemaphoreHandle_t g_apiLock = nullptr;  // Start / Stop
SemaphoreHandle_t g_taskDone = nullptr;
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
State g_state;
Mode g_mode = Mode::Device;
bool g_preferOverWifi = false;
constexpr int32_t kEvents[3] = {IP_EVENT_AP_STAIPASSIGNED, IP_EVENT_ETH_GOT_IP, IP_EVENT_ETH_LOST_IP};
esp_event_handler_instance_t g_handlers[3] = {};
std::atomic<bool> g_drainPending{false};  // one deferred drain in TinyUSB's queue at most
std::atomic<bool> g_bounceLink{false};    // the mode changed with the class kept: the computer must ask again

// The transmit slots: lwIP's thread fills the one after the last, the USB task empties the first.
uint8_t* g_slots = nullptr;
uint16_t g_slotLength[kSlots] = {};
std::size_t g_txHead = 0, g_txCount = 0;
std::atomic<uint32_t> g_rxFrames{0}, g_txFrames{0}, g_rxDropped{0}, g_txDropped{0};

// mDNS, on lwIP's thread.
udp_pcb* g_mdns = nullptr;
int g_announcements = 0;
int64_t g_nextAnnouncementUs = 0;

void Copy4(uint8_t out[4], uint32_t networkOrder)
{
    const uint32_t value = lwip_ntohl(networkOrder);
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}


void Notify(uint32_t bits)
{
    if (TaskHandle_t task = g_task.load()) xTaskNotify(task, bits, eSetBits);
}

// ---- To the computer ------------------------------------------------------------------------------------------

// USB task: as many slots as there are free NTBs.
void DrainTransmit()
{
    if (g_slots == nullptr) return;
    for (;;)
    {
        portENTER_CRITICAL(&g_lock);
        const std::size_t count = g_txCount;
        const std::size_t head = g_txHead;
        portEXIT_CRITICAL(&g_lock);
        if (count == 0) return;
        const uint16_t length = g_slotLength[head];
        if (!tud_network_can_xmit(length)) return;  // every NTB busy: the next pass
        tud_network_xmit(g_slots + head * kSlotBytes, length);
        portENTER_CRITICAL(&g_lock);
        g_txHead = (g_txHead + 1) % kSlots;
        --g_txCount;
        portEXIT_CRITICAL(&g_lock);
        g_txFrames.fetch_add(1, std::memory_order_relaxed);
    }
}

void DeferredDrain(void*)
{
    g_drainPending.store(false);
    DrainTransmit();
}

// The USB task: tell the computer the cable went (false) or came (true), so it asks for an address again.
void DeferredLinkDown(void*) { tud_network_link_state(0, false); }
void DeferredLinkUp(void*) { tud_network_link_state(0, true); }

void DropTransmit()
{
    portENTER_CRITICAL(&g_lock);
    g_txHead = 0;
    g_txCount = 0;
    portEXIT_CRITICAL(&g_lock);
}

// lwIP's thread: one Ethernet frame (esp_netif's driver transmit; lwIP frees the buffer when it returns).
esp_err_t Transmit(void*, void* buffer, size_t length)
{
    if (!g_usbUp.load(std::memory_order_relaxed) || length == 0 || length > kSlotBytes || length > CFG_TUD_NET_MTU)
    {
        g_txDropped.fetch_add(1, std::memory_order_relaxed);
        return ESP_OK;
    }
    const int64_t until = esp_timer_get_time() + kTransmitWaitUs;
    for (;;)
    {
        portENTER_CRITICAL(&g_lock);
        const bool room = g_txCount < kSlots;
        const std::size_t slot = (g_txHead + g_txCount) % kSlots;
        portEXIT_CRITICAL(&g_lock);
        if (room)
        {
            // This thread alone fills slots, and the USB task does not read one before it is counted.
            std::memcpy(g_slots + slot * kSlotBytes, buffer, length);
            g_slotLength[slot] = static_cast<uint16_t>(length);
            portENTER_CRITICAL(&g_lock);
            ++g_txCount;
            portEXIT_CRITICAL(&g_lock);
            // Wake the USB task (it also tries every pass). One wake in its queue at most: posting into a full
            // queue would wait, and lwIP's thread must not.
            if (!g_drainPending.exchange(true)) usbd_defer_func(DeferredDrain, nullptr, false);
            return ESP_OK;
        }
        if (esp_timer_get_time() >= until || !g_usbUp.load(std::memory_order_relaxed))
        {
            g_txDropped.fetch_add(1, std::memory_order_relaxed);
            return ESP_OK;
        }
        vTaskDelay(1);
    }
}

void FreeRx(void*, void*) {}

// ---- From the computer ----------------------------------------------------------------------------------------

// The USB task (through esp_netif_receive): the datagram into an lwIP pbuf, posted to lwIP's thread.
esp_netif_recv_ret_t Input(void* handle, void* buffer, size_t length, void*)
{
    auto* netif = static_cast<struct netif*>(handle);
    if (netif == nullptr || buffer == nullptr || length == 0) return ESP_NETIF_OPTIONAL_RETURN_CODE(ESP_FAIL);
    pbuf* packet = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(length), PBUF_RAM);
    if (packet == nullptr)
    {
        g_rxDropped.fetch_add(1, std::memory_order_relaxed);
        return ESP_NETIF_OPTIONAL_RETURN_CODE(ESP_ERR_NO_MEM);
    }
    pbuf_take(packet, buffer, static_cast<u16_t>(length));
    if (netif->input(packet, netif) != ERR_OK)
    {
        pbuf_free(packet);
        g_rxDropped.fetch_add(1, std::memory_order_relaxed);
        return ESP_NETIF_OPTIONAL_RETURN_CODE(ESP_FAIL);
    }
    g_rxFrames.fetch_add(1, std::memory_order_relaxed);
    return ESP_NETIF_OPTIONAL_RETURN_CODE(ESP_OK);
}

// ---- mDNS (lwIP's thread) -------------------------------------------------------------------------------------

struct netif* LwipNetif()
{
    esp_netif_t* netif = g_netif.load();
    return netif != nullptr ? static_cast<struct netif*>(esp_netif_get_netif_impl(netif)) : nullptr;
}

void SendAnswer(struct netif* netif, const ip_addr_t* to, u16_t port, bool legacy, uint16_t id)
{
    uint8_t address[4];
    Copy4(address, ip4_addr_get_u32(netif_ip4_addr(netif)));
    if ((address[0] | address[1] | address[2] | address[3]) == 0) return;
    char name[33];
    portENTER_CRITICAL(&g_lock);
    std::memcpy(name, g_state.hostname, sizeof(name));
    portEXIT_CRITICAL(&g_lock);
    uint8_t answer[128];
    const std::size_t length = NetLogic::BuildAnswer(name, address, legacy, id, answer, sizeof(answer));
    if (length == 0) return;
    pbuf* packet = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(length), PBUF_RAM);
    if (packet == nullptr) return;
    pbuf_take(packet, answer, static_cast<u16_t>(length));
    udp_sendto_if(g_mdns, packet, to, port, netif);
    pbuf_free(packet);
}

void SendMulticastAnswer(struct netif* netif)
{
    ip_addr_t group;
    IP_ADDR4(&group, NetLogic::kMdnsGroup[0], NetLogic::kMdnsGroup[1], NetLogic::kMdnsGroup[2], NetLogic::kMdnsGroup[3]);
    SendAnswer(netif, &group, NetLogic::kMdnsPort, false, 0);
}

void MdnsReceive(void*, udp_pcb*, pbuf* packet, const ip_addr_t* from, u16_t port)
{
    struct netif* netif = LwipNetif();
    if (netif == nullptr || ip_current_input_netif() != netif)
    {
        pbuf_free(packet);
        return;  // another interface's (the firmware may answer there itself)
    }
    uint8_t bytes[512];
    const u16_t length = pbuf_copy_partial(packet, bytes, sizeof(bytes), 0);
    pbuf_free(packet);
    char name[33];
    portENTER_CRITICAL(&g_lock);
    std::memcpy(name, g_state.hostname, sizeof(name));
    portEXIT_CRITICAL(&g_lock);
    NetLogic::Query query;
    if (!NetLogic::ParseQuery(bytes, length, name, query)) return;
    // A one-shot (legacy) query from another port gets its answer back there; everyone else on the group.
    if (port != NetLogic::kMdnsPort) SendAnswer(netif, from, port, true, query.id);
    else SendMulticastAnswer(netif);
}

esp_err_t MdnsOpen(void*)
{
    struct netif* netif = LwipNetif();
    if (netif == nullptr || g_mdns != nullptr) return ESP_FAIL;
    g_mdns = udp_new_ip_type(IPADDR_TYPE_V4);
    if (g_mdns == nullptr) return ESP_ERR_NO_MEM;
    ip_set_option(g_mdns, SOF_REUSEADDR);  // beside a responder of the firmware's own on another interface
    if (udp_bind(g_mdns, IP4_ADDR_ANY, NetLogic::kMdnsPort) != ERR_OK)
    {
        udp_remove(g_mdns);
        g_mdns = nullptr;
        return ESP_FAIL;
    }
    g_mdns->ttl = 255;
    udp_set_multicast_ttl(g_mdns, 255);
    ip4_addr_t group;
    IP4_ADDR(&group, NetLogic::kMdnsGroup[0], NetLogic::kMdnsGroup[1], NetLogic::kMdnsGroup[2], NetLogic::kMdnsGroup[3]);
    igmp_joingroup_netif(netif, &group);
    udp_recv(g_mdns, MdnsReceive, nullptr);
    return ESP_OK;
}

esp_err_t MdnsClose(void*)
{
    if (g_mdns == nullptr) return ESP_OK;
    if (struct netif* netif = LwipNetif())
    {
        ip4_addr_t group;
        IP4_ADDR(&group, NetLogic::kMdnsGroup[0], NetLogic::kMdnsGroup[1], NetLogic::kMdnsGroup[2], NetLogic::kMdnsGroup[3]);
        igmp_leavegroup_netif(netif, &group);
    }
    udp_remove(g_mdns);
    g_mdns = nullptr;
    return ESP_OK;
}

esp_err_t MdnsAnnounce(void*)
{
    struct netif* netif = LwipNetif();
    if (netif != nullptr && g_mdns != nullptr && netif_is_up(netif)) SendMulticastAnswer(netif);
    return ESP_OK;
}

// ---- State and events ------------------------------------------------------------------------------------------

void Publish()
{
    State state;
    portENTER_CRITICAL(&g_lock);
    state = g_state;
    portEXIT_CRITICAL(&g_lock);
    if (Listener listener = g_listener.load()) listener(state);
}

void SetLink(Link link)
{
    portENTER_CRITICAL(&g_lock);
    g_state.link = link;
    portEXIT_CRITICAL(&g_lock);
}

void LogAddresses(const char* what)
{
    State state;
    portENTER_CRITICAL(&g_lock);
    state = g_state;
    portEXIT_CRITICAL(&g_lock);
    const esp_netif_t* defaultNetif = esp_netif_get_default_netif();
    ESP_LOGI(kTag, "%s: board %u.%u.%u.%u/%u.%u.%u.%u gateway %u.%u.%u.%u dns %u.%u.%u.%u, default route %s", what,
             state.device[0], state.device[1], state.device[2], state.device[3], state.netmask[0], state.netmask[1],
             state.netmask[2], state.netmask[3], state.gateway[0], state.gateway[1], state.gateway[2], state.gateway[3],
             state.dns[0], state.dns[1], state.dns[2], state.dns[3],
             defaultNetif == nullptr ? "none" : (defaultNetif == g_netif.load() ? "usb" : esp_netif_get_desc(const_cast<esp_netif_t*>(defaultNetif))));
}

void OnIpEvent(void*, esp_event_base_t, int32_t id, void* data)
{
    esp_netif_t* netif = g_netif.load();
    if (netif == nullptr || data == nullptr) return;
    if (id == IP_EVENT_AP_STAIPASSIGNED)
    {
        const auto* event = static_cast<const ip_event_ap_staipassigned_t*>(data);
        if (event->esp_netif != netif) return;
        portENTER_CRITICAL(&g_lock);
        Copy4(g_state.host, event->ip.addr);
        portEXIT_CRITICAL(&g_lock);
        ESP_LOGI(kTag, "the computer leased " IPSTR " (its MAC %02x:%02x:%02x:%02x:%02x:%02x); the board is http://%s.local "
                       "and 192.168.203.1", IP2STR(&event->ip), event->mac[0], event->mac[1], event->mac[2], event->mac[3],
                 event->mac[4], event->mac[5], g_state.hostname);
        Notify(kStateChanged);
        return;
    }
    const auto* event = static_cast<const ip_event_got_ip_t*>(data);
    if (event->esp_netif != netif) return;
    if (id == IP_EVENT_ETH_GOT_IP)
    {
        esp_netif_dns_info_t dns = {};
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
        portENTER_CRITICAL(&g_lock);
        Copy4(g_state.device, event->ip_info.ip.addr);
        Copy4(g_state.netmask, event->ip_info.netmask.addr);
        Copy4(g_state.gateway, event->ip_info.gw.addr);
        Copy4(g_state.dns, dns.ip.u_addr.ip4.addr);
        g_state.link = Link::Ready;
        portEXIT_CRITICAL(&g_lock);
        LogAddresses("uplink leased");
        g_announcements = 2;
        g_nextAnnouncementUs = 0;
        Notify(kStateChanged);
    }
    else if (id == IP_EVENT_ETH_LOST_IP)
    {
        portENTER_CRITICAL(&g_lock);
        std::memset(g_state.device, 0, sizeof(g_state.device));
        std::memset(g_state.gateway, 0, sizeof(g_state.gateway));
        std::memset(g_state.dns, 0, sizeof(g_state.dns));
        if (g_state.link == Link::Ready) g_state.link = Link::Up;
        portEXIT_CRITICAL(&g_lock);
        ESP_LOGW(kTag, "uplink lost its address");
        Notify(kStateChanged);
    }
}

// The network's task: the link's changes, the announcements, the listener.
void Task(void*)
{
    bool up = false;
    for (;;)
    {
        uint32_t bits = 0;
        const TickType_t wait = g_announcements > 0 ? pdMS_TO_TICKS(250) : pdMS_TO_TICKS(1000);
        xTaskNotifyWait(0, UINT32_MAX, &bits, wait);
        if (bits & kQuit) break;
        esp_netif_t* netif = g_netif.load();
        if ((bits & kLinkChanged) && netif != nullptr)
        {
            const bool now = g_usbUp.load();
            if (now != up)
            {
                up = now;
                if (up)
                {
                    if (g_bounceLink.exchange(false))
                    {
                        // Another mode on the same link: the computer drops what it leased and asks again.
                        usbd_defer_func(DeferredLinkDown, nullptr, false);
                        vTaskDelay(pdMS_TO_TICKS(600));
                        usbd_defer_func(DeferredLinkUp, nullptr, false);
                    }
                    esp_netif_action_connected(netif, nullptr, 0, nullptr);
                    if (g_mode == Mode::Device)
                    {
                        SetLink(Link::Ready);
                        LogAddresses("link up (device mode)");
                        g_announcements = 3;
                        g_nextAnnouncementUs = 0;
                    }
                    else
                    {
                        SetLink(Link::Up);
                        ESP_LOGI(kTag, "link up (uplink): asking the computer for an address (its internet sharing must be on)");
                    }
                }
                else
                {
                    esp_netif_action_disconnected(netif, nullptr, 0, nullptr);
                    portENTER_CRITICAL(&g_lock);
                    g_state.link = Link::Waiting;
                    std::memset(g_state.host, 0, sizeof(g_state.host));
                    portEXIT_CRITICAL(&g_lock);
                    g_announcements = 0;
                    ESP_LOGI(kTag, "link down (unplugged, asleep or the device changed); %lu frames in, %lu out, %lu and %lu dropped",
                             static_cast<unsigned long>(g_rxFrames.load()), static_cast<unsigned long>(g_txFrames.load()),
                             static_cast<unsigned long>(g_rxDropped.load()), static_cast<unsigned long>(g_txDropped.load()));
                }
                Publish();
            }
        }
        if (bits & kStateChanged) Publish();
        // RFC 6762 8.3: announce a few times, a second apart, once the link has its address.
        if (up && g_announcements > 0 && esp_timer_get_time() >= g_nextAnnouncementUs)
        {
            esp_netif_tcpip_exec(MdnsAnnounce, nullptr);
            --g_announcements;
            g_nextAnnouncementUs = esp_timer_get_time() + 1000000;
        }
    }
    g_task.store(nullptr);
    xSemaphoreGive(g_taskDone);
    vTaskDelete(nullptr);
}

void Ip(esp_netif_ip_info_t& info)
{
    IP4_ADDR(&info.ip, NetLogic::kDeviceAddress[0], NetLogic::kDeviceAddress[1], NetLogic::kDeviceAddress[2], NetLogic::kDeviceAddress[3]);
    IP4_ADDR(&info.netmask, NetLogic::kNetmask[0], NetLogic::kNetmask[1], NetLogic::kNetmask[2], NetLogic::kNetmask[3]);
    IP4_ADDR(&info.gw, 0, 0, 0, 0);  // no gateway: never a route out through the board
}

esp_netif_t* MakeNetif(Mode mode, const char* hostname)
{
    static esp_netif_ip_info_t deviceIp;
    Ip(deviceIp);
    esp_netif_inherent_config_t base = {};
    base.if_key = "USB_NCM";
    base.if_desc = "usb";
    if (mode == Mode::Device)
    {
        base.flags = static_cast<esp_netif_flags_t>(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
        base.ip_info = &deviceIp;
        base.route_prio = kDeviceRoutePrio;
    }
    else
    {
        base.flags = static_cast<esp_netif_flags_t>(ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_EVENT_IP_MODIFIED);
        base.get_ip_event = IP_EVENT_ETH_GOT_IP;
        base.lost_ip_event = IP_EVENT_ETH_LOST_IP;
        base.route_prio = g_preferOverWifi ? kUplinkFirstRoutePrio : kUplinkRoutePrio;
    }
    esp_netif_driver_ifconfig_t driver = {};
    driver.handle = reinterpret_cast<void*>(1);  // the link is a singleton (must not be null)
    driver.transmit = Transmit;
    driver.driver_free_rx_buffer = FreeRx;
    static const esp_netif_netstack_config_t stack = {.lwip = {.init_fn = ethernetif_init, .input_fn = Input}};
    esp_netif_config_t config = {};
    config.base = &base;
    config.driver = &driver;
    config.stack = &stack;
    esp_netif_t* netif = esp_netif_new(&config);
    if (netif == nullptr) return nullptr;
    esp_netif_set_mac(netif, g_deviceMac);
    esp_netif_set_hostname(netif, hostname);
    if (mode == Mode::Device)
    {
        // No router and no DNS server in the leases: the computer's own internet and names stay as they are.
        dhcps_offer_t none = 0;
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &none, sizeof(none));
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &none, sizeof(none));
        uint32_t lease = NetLogic::kLeaseMinutes;
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lease, sizeof(lease));
    }
    esp_netif_action_start(netif, nullptr, 0, nullptr);
    // The link is down until the USB task sees a computer.
    esp_netif_action_disconnected(netif, nullptr, 0, nullptr);
    return netif;
}

void TearDown()
{
    g_active.store(false);
    g_usbUp.store(false);
    if (g_task.load() != nullptr)
    {
        Notify(kQuit);
        xSemaphoreTake(g_taskDone, pdMS_TO_TICKS(2000));
    }
    if (g_netif.load() != nullptr) esp_netif_tcpip_exec(MdnsClose, nullptr);
    for (int index = 0; index < 3; ++index)
        if (g_handlers[index] != nullptr)
        {
            esp_event_handler_instance_unregister(IP_EVENT, kEvents[index], g_handlers[index]);
            g_handlers[index] = nullptr;
        }
    if (esp_netif_t* netif = g_netif.load())
    {
        esp_netif_action_disconnected(netif, nullptr, 0, nullptr);
        esp_netif_action_stop(netif, nullptr, 0, nullptr);
        // The USB task hands no frame to it any more (g_active is off and it holds g_rxLock while it does).
        xSemaphoreTake(g_rxLock, portMAX_DELAY);
        g_netif.store(nullptr);
        xSemaphoreGive(g_rxLock);
        esp_netif_destroy(netif);
    }
    DropTransmit();
    heap_caps_free(g_slots);
    g_slots = nullptr;
}

}  // namespace

bool Start(Mode mode, const Options& options)
{
    if (g_apiLock == nullptr)
    {
        g_apiLock = xSemaphoreCreateMutex();
        g_rxLock = xSemaphoreCreateMutex();
        g_taskDone = xSemaphoreCreateBinary();
    }
    if (g_apiLock == nullptr || g_rxLock == nullptr || g_taskDone == nullptr) return false;
    if (!USB::Started())
    {
        ESP_LOGE(kTag, "USB is not started (CardenzaHAL::Init or USB::Init first)");
        return false;
    }
    if (g_task.load() != nullptr && xTaskGetCurrentTaskHandle() == g_task.load())
    {
        ESP_LOGE(kTag, "Start from the listener: not allowed (it runs on the network's own task)");
        return false;
    }
    xSemaphoreTake(g_apiLock, portMAX_DELAY);
    const bool restarting = g_active.load() && USB::Classes().network;
    if (g_active.load()) TearDown();  // another mode or other options: from the start
    g_bounceLink.store(restarting);
    LoadMacs();
    g_mode = mode;
    g_preferOverWifi = options.preferOverWifi;
    State fresh;
    fresh.mode = mode;
    fresh.link = Link::Off;
    NetLogic::HostName(options.hostname, fresh.hostname);
    if (mode == Mode::Device)
    {
        std::memcpy(fresh.device, NetLogic::kDeviceAddress, 4);
        std::memcpy(fresh.netmask, NetLogic::kNetmask, 4);
    }
    portENTER_CRITICAL(&g_lock);
    g_state = fresh;
    portEXIT_CRITICAL(&g_lock);
    g_rxFrames.store(0);
    g_txFrames.store(0);
    g_rxDropped.store(0);
    g_txDropped.store(0);

    bool ok = false;
    do
    {
        esp_err_t error = esp_netif_init();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) break;
        error = esp_event_loop_create_default();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) break;
        g_slots = static_cast<uint8_t*>(heap_caps_malloc(kSlots * kSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (g_slots == nullptr) g_slots = static_cast<uint8_t*>(heap_caps_malloc(kSlots * kSlotBytes, MALLOC_CAP_8BIT));
        if (g_slots == nullptr) break;
        esp_netif_t* netif = MakeNetif(mode, fresh.hostname);
        if (netif == nullptr) break;
        g_netif.store(netif);
        for (int index = 0; index < 3; ++index)
            esp_event_handler_instance_register(IP_EVENT, kEvents[index], OnIpEvent, nullptr, &g_handlers[index]);
        if (esp_netif_tcpip_exec(MdnsOpen, nullptr) != ESP_OK) ESP_LOGW(kTag, "mDNS: port 5353 is taken, %s.local will not answer", fresh.hostname);
        TaskHandle_t task = nullptr;
        if (xTaskCreatePinnedToCore(Task, "usbnet", 3072, nullptr, 2, &task, tskNO_AFFINITY) != pdPASS) break;
        g_task.store(task);
        g_active.store(true);
        // The class, now: the computer finds the device again with it; its IN endpoints come from what gives way.
        USB::Logic::Classes keep;
        keep.network = true;
        USB::Logic::Classes wanted = USB::detail::AskedClasses();
        wanted.network = true;
        if (!USB::detail::SetClasses(wanted, keep) || !USB::Classes().network)
        {
            ESP_LOGE(kTag, "the network class cannot be presented (not built, or no room: see the usb: line above)");
            break;
        }
        portENTER_CRITICAL(&g_lock);
        g_state.presented = true;
        g_state.link = Link::Waiting;
        portEXIT_CRITICAL(&g_lock);
        ESP_LOGI(kTag, "started (%s mode, %s.local, the computer's MAC %s); waiting for the computer",
                 mode == Mode::Device ? "device" : "uplink", fresh.hostname, g_hostMacString);
        ok = true;
    } while (false);
    if (!ok)
    {
        TearDown();
        portENTER_CRITICAL(&g_lock);
        g_state.link = Link::Off;
        g_state.presented = false;
        portEXIT_CRITICAL(&g_lock);
    }
    xSemaphoreGive(g_apiLock);
    Notify(kLinkChanged);
    if (ok) Publish();
    return ok;
}

void Stop()
{
    if (g_apiLock == nullptr) return;
    if (g_task.load() != nullptr && xTaskGetCurrentTaskHandle() == g_task.load())
    {
        ESP_LOGE(kTag, "Stop from the listener: not allowed (it runs on the network's own task)");
        return;
    }
    xSemaphoreTake(g_apiLock, portMAX_DELAY);
    if (g_active.load() || g_netif.load() != nullptr)
    {
        TearDown();
        USB::Logic::Classes wanted = USB::detail::AskedClasses();
        if (wanted.network)
        {
            wanted.network = false;
            USB::detail::SetClasses(wanted, USB::Logic::Classes{});
        }
        portENTER_CRITICAL(&g_lock);
        g_state.link = Link::Off;
        g_state.presented = false;
        portEXIT_CRITICAL(&g_lock);
        ESP_LOGI(kTag, "stopped");
        Publish();
    }
    xSemaphoreGive(g_apiLock);
}

bool Started() { return g_active.load(); }

State GetState()
{
    State state;
    portENTER_CRITICAL(&g_lock);
    state = g_state;
    portEXIT_CRITICAL(&g_lock);
    return state;
}

esp_netif_obj* Netif() { return g_netif.load(); }

void SetListener(Listener listener) { g_listener.store(listener); }

const char* LinkName(Link link)
{
    switch (link)
    {
        case Link::Off: return "off";
        case Link::Waiting: return "waiting for the computer";
        case Link::Up: return "up";
        case Link::Ready: return "ready";
    }
    return "?";
}

#else  // the build has no USB network (CONFIG_CARDENZA_HAL_USB_NETWORK)

bool Start(Mode, const Options&) { return false; }
void Stop() {}
bool Started() { return false; }
State GetState() { return State{}; }
esp_netif_obj* Netif() { return nullptr; }
void SetListener(Listener) {}

const char* LinkName(Link link)
{
    switch (link)
    {
        case Link::Off: return "off";
        case Link::Waiting: return "waiting for the computer";
        case Link::Up: return "up";
        case Link::Ready: return "ready";
    }
    return "?";
}

#endif

}  // namespace Network

#if CFG_TUD_NCM
namespace detail {

// The USB task, every pass: the link's state and the frames waiting to go.
void NetworkPump()
{
    using namespace Network;
    const bool up = g_active.load(std::memory_order_relaxed) && tud_mounted() && !tud_suspended() && USB::Classes().network;
    if (up != g_usbUp.load(std::memory_order_relaxed))
    {
        g_usbUp.store(up);
        if (!up) DropTransmit();
        Notify(kLinkChanged);
    }
    if (up) DrainTransmit();
}

}  // namespace detail
#endif

}  // namespace CardenzaHAL::USB

#if CFG_TUD_NCM
// ---- TinyUSB's network callbacks (C linkage) ------------------------------------------------------------------
extern "C" {

// Used by TinyUSB's ECM/RNDIS only; NCM names the computer's MAC in its descriptor (NetworkMacString).
uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x03, 0x00, 0x00, 0x01};

void tud_network_init_cb(void) { CardenzaHAL::USB::Network::DropTransmit(); }

// The USB task: one datagram from the computer. Always taken (copied, or dropped when nobody has the link), and
// the next one asked for at once.
bool tud_network_recv_cb(const uint8_t* source, uint16_t size)
{
    using namespace CardenzaHAL::USB::Network;
    bool handed = false;
    if (g_usbUp.load(std::memory_order_relaxed) && xSemaphoreTake(g_rxLock, 0) == pdTRUE)
    {
        if (esp_netif_t* netif = g_netif.load())
        {
            esp_netif_receive(netif, const_cast<uint8_t*>(source), size, nullptr);
            handed = true;
        }
        xSemaphoreGive(g_rxLock);
    }
    if (!handed) g_rxDropped.fetch_add(1, std::memory_order_relaxed);
    tud_network_recv_renew();
    return true;
}

// The USB task, inside tud_network_xmit: a slot's frame into the NTB.
uint16_t tud_network_xmit_cb(uint8_t* destination, void* reference, uint16_t length)
{
    std::memcpy(destination, reference, length);
    return length;
}

}  // extern "C"
#endif
