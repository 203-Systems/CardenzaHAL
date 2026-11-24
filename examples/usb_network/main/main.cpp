// The board as a USB network adapter (usb/network.h). Plugged in, the computer gets an address from the board and
// opens http://cardenza.local (or http://192.168.203.1): a page with the board's information and the link, live.
// Press any key: the uplink mode (the board uses the computer's shared internet, if it is turned on there: macOS
// Internet Sharing, Windows ICS) and a page fetched through it; again: back to the device mode.
// The screen: grey, off; amber, waiting for the computer; green, the device mode is ready; blue, the uplink has an
// address (a white bar: the fetch worked, red: it did not); red, the network could not start (see the log).
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cardenzaHAL.h"
#include "keyboard/led_light.h"
#include "usb/network.h"
#include "usb/usb.h"

namespace {

namespace Display = CardenzaHAL::Display;
namespace Keyboard = CardenzaHAL::Keyboard;
namespace Network = CardenzaHAL::USB::Network;

constexpr char kTag[] = "usb_network";
constexpr char kFetchUrl[] = "http://example.com/";

constexpr uint16_t Rgb(int r, int g, int b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

alignas(4) uint16_t g_frame[Display::WIDTH * Display::HEIGHT];
std::atomic<int> g_fetch{0};  // 0 none, 1 worked, -1 failed
std::atomic<bool> g_failed{false};

void Fill(int x, int y, int width, int height, uint16_t colour)
{
    for (int row = y; row < y + height && row < Display::HEIGHT; ++row)
        for (int column = x; column < x + width && column < Display::WIDTH; ++column) g_frame[row * Display::WIDTH + column] = colour;
}

std::string Ip(const uint8_t a[4])
{
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    return text;
}

// The board's information, the main fields of the development channel's INFO (docs/usb-dev.md), and the link.
std::string InfoJson()
{
    const esp_app_desc_t* app = esp_app_get_description();
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    char macText[18];
    std::snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const Network::State state = Network::GetState();
    char json[768];
    std::snprintf(json, sizeof(json),
                  "{\"board\":\"Cardenza\",\"firmware\":{\"name\":\"%.32s\",\"version\":\"%.32s\",\"idf\":\"%.32s\"},"
                  "\"chip\":{\"mac\":\"%s\"},\"uptime_ms\":%" PRIu64 ",\"internal_free\":%u,"
                  "\"network\":{\"mode\":\"%s\",\"link\":\"%s\",\"board\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\","
                  "\"dns\":\"%s\",\"computer\":\"%s\",\"hostname\":\"%s.local\"},\"fetch\":%d}",
                  app->project_name, app->version, app->idf_ver, macText, esp_timer_get_time() / 1000,
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  state.mode == Network::Mode::Device ? "device" : "uplink", Network::LinkName(state.link), Ip(state.device).c_str(),
                  Ip(state.netmask).c_str(), Ip(state.gateway).c_str(), Ip(state.dns).c_str(), Ip(state.host).c_str(),
                  state.hostname, g_fetch.load());
    return json;
}

constexpr char kPage[] = R"(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Cardenza</title><style>body{font:15px ui-monospace,monospace;margin:16px;background:#111;color:#e8e8e0}
h1{font-size:18px}pre{background:#1c1c1a;padding:12px;border-radius:6px;white-space:pre-wrap}</style></head>
<body><h1>Cardenza on USB</h1><pre id="info">...</pre><script>
async function tick(){try{const r=await fetch('/info');document.getElementById('info').textContent=JSON.stringify(await r.json(),null,2)}
catch(e){document.getElementById('info').textContent='the board went away: '+e}}tick();setInterval(tick,1000);
</script></body></html>)";

esp_err_t ServePage(httpd_req_t* request)
{
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, kPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t ServeInfo(httpd_req_t* request)
{
    const std::string json = InfoJson();
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, json.c_str(), static_cast<ssize_t>(json.size()));
}

void StartServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK)
    {
        ESP_LOGE(kTag, "the web server did not start");
        return;
    }
    static const httpd_uri_t page = {"/", HTTP_GET, ServePage, nullptr};
    static const httpd_uri_t info = {"/info", HTTP_GET, ServeInfo, nullptr};
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &info);
    ESP_LOGI(kTag, "serving http://cardenza.local and http://192.168.203.1");
}

// Through the uplink: one page, its status and size in the log.
void Fetch(void*)
{
    esp_http_client_config_t config = {};
    config.url = kFetchUrl;
    config.timeout_ms = 8000;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    const int64_t started = esp_timer_get_time();
    const esp_err_t error = client != nullptr ? esp_http_client_perform(client) : ESP_FAIL;
    const int status = client != nullptr ? esp_http_client_get_status_code(client) : 0;
    const int64_t length = client != nullptr ? esp_http_client_get_content_length(client) : 0;
    if (client != nullptr) esp_http_client_cleanup(client);
    g_fetch.store(error == ESP_OK && status == 200 ? 1 : -1);
    ESP_LOGI(kTag, "uplink fetch %s: %s, status %d, %lld bytes, %lld ms", kFetchUrl, esp_err_to_name(error), status,
             static_cast<long long>(length), static_cast<long long>((esp_timer_get_time() - started) / 1000));
    vTaskDelete(nullptr);
}

// The network's task.
void Changed(const Network::State& state)
{
    ESP_LOGI(kTag, "%s mode, link %s, board %s, computer %s", state.mode == Network::Mode::Device ? "device" : "uplink",
             Network::LinkName(state.link), Ip(state.device).c_str(), Ip(state.host).c_str());
    if (state.mode == Network::Mode::Uplink && state.link == Network::Link::Ready && g_fetch.load() == 0)
        xTaskCreate(Fetch, "fetch", 6144, nullptr, 2, nullptr);
}

void Draw(const Network::State& state)
{
    uint16_t background = Rgb(40, 40, 38);
    if (g_failed.load()) background = Rgb(140, 30, 30);
    else if (state.link == Network::Link::Waiting || state.link == Network::Link::Up) background = Rgb(150, 100, 20);
    else if (state.link == Network::Link::Ready) background = state.mode == Network::Mode::Device ? Rgb(20, 110, 50) : Rgb(30, 70, 150);
    Fill(0, 0, Display::WIDTH, Display::HEIGHT, background);
    const int fetch = g_fetch.load();
    if (fetch != 0) Fill(0, Display::HEIGHT - 12, Display::WIDTH, 12, fetch > 0 ? Rgb(232, 232, 224) : Rgb(255, 52, 68));
}

void StartMode(Network::Mode mode)
{
    g_fetch.store(0);
    const bool started = Network::Start(mode);
    g_failed.store(!started);
    if (!started) ESP_LOGE(kTag, "the network did not start");
}

}  // namespace

extern "C" void app_main()
{
    if (!CardenzaHAL::Init()) ESP_LOGE(kTag, "HAL init failed");
    Keyboard::enableLED(true);
    Network::SetListener(Changed);
    StartMode(Network::Mode::Device);
    StartServer();

    bool keyBefore = false;
    Network::Mode mode = Network::Mode::Device;
    Display::FramePacer pacer;
    for (;;)
    {
        Keyboard::updateKeyList();
        const bool key = !Keyboard::keyList().empty();
        if (key && !keyBefore)
        {
            mode = mode == Network::Mode::Device ? Network::Mode::Uplink : Network::Mode::Device;
            ESP_LOGI(kTag, "switching to the %s mode", mode == Network::Mode::Device ? "device" : "uplink");
            StartMode(mode);
        }
        keyBefore = key;
        const Network::State state = Network::GetState();
        Keyboard::LedFrame leds{};
        leds.fill(Keyboard::Light::PerceivedToLevel(state.link == Network::Link::Ready ? 15 : 4));
        Keyboard::setLEDFrame(leds);
        if (pacer.DrawTick())
        {
            Draw(state);
            Display::PresentChanged(g_frame);
        }
        pacer.Wait();
    }
}
