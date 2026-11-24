// The USB drive (usb/drive.h): the SD card lent to the computer as mass storage. usb/drive_logic.h decides; a
// worker task (created by Share, gone once the card is back) carries it out: the file system lets the card go
// (storage/internal/lend.h), the card is started alone, the drive is added to the USB device (detail::SetClasses),
// and the other way back. The MSC callbacks run on the USB task and read and write the card directly, a block of
// CFG_TUD_MSC_EP_BUFSIZE at a time, so nothing of a request outlives a USB reset.
#include "usb/drive.h"

#include <atomic>
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "usb/internal/dev.h"
#include "usb/usb.h"
#include "tusb.h"

#if CFG_TUD_MSC
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

#include "storage/internal/lend.h"
#include "storage/internal/sd_slot.h"
#include "system/internal/hooks.h"
#endif

namespace CardenzaHAL::USB {

namespace {

constexpr char kTag[] = "usb-drive";

// What the worker publishes, for GetStatus and the waits in Share and Stop.
portMUX_TYPE g_statusLock = portMUX_INITIALIZER_UNLOCKED;
Drive::Status g_status;
uint32_t g_statusChanges = 0;
std::atomic<Drive::Listener> g_listener{nullptr};

Drive::Status ReadStatus(uint32_t* changes = nullptr)
{
    portENTER_CRITICAL_SAFE(&g_statusLock);
    Drive::Status status = g_status;
    if (changes != nullptr) *changes = g_statusChanges;
    portEXIT_CRITICAL_SAFE(&g_statusLock);
    return status;
}

#if CFG_TUD_MSC
enum class Request : uint8_t { Share, Stop, Eject, HostGone };

constexpr uint32_t kTickMs = 250;
constexpr uint32_t kIdleExitMs = 2000;   // the worker goes this long after the card is back
constexpr uint32_t kIoLockMs = 2000;     // the USB task waits this long for the card while it is started or stopped
constexpr uint32_t kStackBytes = 6144;   // the file system's mount, SetClasses' log lines

DriveLogic::Machine g_machine;           // the worker's
QueueHandle_t g_requests = nullptr;      // made on the first Share, kept
std::atomic<bool> g_running{false};

// What the USB task reads and writes.
std::atomic<bool> g_mediumReady{false};  // the computer may read and write the card
std::atomic<bool> g_hostCommand{false};  // the computer sent a command since the worker last looked
std::atomic<uint32_t> g_sectorsRead{0}, g_sectorsWritten{0}, g_ioErrors{0};
SemaphoreHandle_t g_ioLock = nullptr;    // the card: the USB task's reads and writes, the worker's start and stop
sdmmc_host_t g_host;
sdmmc_card_t g_card;
std::atomic<bool> g_cardUp{false};

void Publish()
{
    Drive::Status status;
    status.state = g_machine.GetState();
    status.reason = g_machine.GetReason();
    status.hostSeen = g_machine.HostSeen();
    status.bytesRead = uint64_t(g_sectorsRead.load()) * 512u;
    status.bytesWritten = uint64_t(g_sectorsWritten.load()) * 512u;
    status.ioErrors = g_ioErrors.load();
    portENTER_CRITICAL_SAFE(&g_statusLock);
    const bool changed = status.state != g_status.state || status.reason != g_status.reason;
    g_status = status;
    if (changed) ++g_statusChanges;
    portEXIT_CRITICAL_SAFE(&g_statusLock);
    if (!changed) return;
    ESP_LOGI(kTag, "%s%s%s", Drive::StateName(status.state), status.reason == Drive::Reason::None ? "" : ": ",
             status.reason == Drive::Reason::None ? "" : Drive::ReasonName(status.reason));
    if (Drive::Listener listener = g_listener.load()) listener(status);
}

// The card alone (no file system), for the computer. ESP-IDF's SD driver resets every pin of the slot when it lets
// it go, GPIO0 (HOME) among them: the side key gets its input back each time (as storage/storage.cpp does).
bool StartCard()
{
    g_host = SDMMC_HOST_DEFAULT();
    esp_err_t status = sdmmc_host_init();
    if (status == ESP_OK)
    {
        const sdmmc_slot_config_t slot = Storage::detail::SlotConfig();
        status = sdmmc_host_init_slot(g_host.slot, &slot);
        if (status == ESP_OK) status = sdmmc_card_init(&g_host, &g_card);
        if (status != ESP_OK) sdmmc_host_deinit();
    }
    System::detail::RestoreHomeButton();
    if (status != ESP_OK)
    {
        ESP_LOGW(kTag, "no card: %s", esp_err_to_name(status));
        return false;
    }
    g_sectorsRead.store(0);
    g_sectorsWritten.store(0);
    g_ioErrors.store(0);
    g_cardUp.store(true);
    ESP_LOGI(kTag, "card: %u sectors of %u bytes", unsigned(g_card.csd.capacity), unsigned(g_card.csd.sector_size));
    return true;
}

void StopCard()
{
    // After the USB task's read or write under way.
    xSemaphoreTake(g_ioLock, portMAX_DELAY);
    if (g_cardUp.exchange(false))
    {
        sdmmc_host_deinit();
        System::detail::RestoreHomeButton();
    }
    xSemaphoreGive(g_ioLock);
}

// Adds or removes the drive. Adding makes room (Logic::Fit) and keeps the drive; true when it is there.
bool PresentDrive(bool on)
{
    Logic::Classes wanted = detail::AskedClasses();
    if (wanted.drive == on && Classes().drive == on) return true;
    wanted.drive = on;
    Logic::Classes keep;
    keep.drive = on;
    const bool applied = detail::SetClasses(wanted, keep);
    return applied && Classes().drive == on;
}

using DriveLogic::Action;

Action Perform(Action action)
{
    switch (action)
    {
        case Action::None: return Action::None;
        case Action::Lend:
        {
            // Starting: the firmware's listener closes its files now, before the file system lets the card go.
            Publish();
            if (!Storage::detail::Lend())
            {
                ESP_LOGE(kTag, "the file system did not let the card go");
                return g_machine.Lent(false, DriveLogic::Reason::CardError);
            }
            if (!StartCard()) return g_machine.Lent(false, DriveLogic::Reason::NoCard);
            return g_machine.Lent(true);
        }
        case Action::Present:
        {
            const bool presented = PresentDrive(true);
            if (!presented) ESP_LOGE(kTag, "the drive could not be added to the USB device");
            g_mediumReady.store(presented);
            const Action next = g_machine.Presented(presented);
            Publish();
            return next;
        }
        case Action::Return:
        {
            // The computer hears "no medium" from now on; then the disk goes from its USB device, then the card
            // from the drive, then the file system mounts it again.
            g_mediumReady.store(false);
            Publish();
            if (!PresentDrive(false)) ESP_LOGW(kTag, "the drive could not be removed from the USB device");
            StopCard();
            const bool mounted = Storage::detail::GiveBack();
            if (!mounted) ESP_LOGW(kTag, "the file system could not mount the card again");
            const Action next = g_machine.Returned(mounted);
            Publish();
            return next;
        }
    }
    return Action::None;
}

void Run(Action action)
{
    while (action != Action::None) action = Perform(action);
}

void Worker(void*)
{
    TickType_t last = xTaskGetTickCount();
    uint32_t idleMs = 0;
    for (;;)
    {
        Request request;
        const bool got = xQueueReceive(g_requests, &request, pdMS_TO_TICKS(kTickMs)) == pdTRUE;
        const TickType_t now = xTaskGetTickCount();
        const uint32_t elapsed = pdTICKS_TO_MS(now - last);
        last = now;
        if (g_hostCommand.exchange(false)) g_machine.HostCommand();
        Action action = Action::None;
        if (got)
        {
            switch (request)
            {
                case Request::Share: action = g_machine.Share(true, Started(), Mounted()); break;
                case Request::Stop: action = g_machine.Stop(); break;
                case Request::Eject: action = g_machine.HostEject(); break;
                case Request::HostGone: action = g_machine.HostGone(); break;
            }
            Publish();  // a refusal (NotStarted, NoHost) is a change too
        }
        if (action == Action::None) action = g_machine.Tick(elapsed, tud_mounted() && !tud_suspended());
        Run(action);
        Publish();  // the counters

        const DriveLogic::State state = g_machine.GetState();
        const bool rest = state == DriveLogic::State::Off || state == DriveLogic::State::Failed;
        idleMs = rest && !got ? idleMs + elapsed : 0;
        if (rest && idleMs >= kIdleExitMs)
        {
            g_running.store(false);
            // A request that came just now: this worker keeps it, unless another started already.
            if (uxQueueMessagesWaiting(g_requests) == 0 || g_running.exchange(true)) break;
            idleMs = 0;
        }
    }
    vTaskDelete(nullptr);
}

bool Post(Request request)
{
    if (g_requests == nullptr) g_requests = xQueueCreate(8, sizeof(Request));
    if (g_ioLock == nullptr) g_ioLock = xSemaphoreCreateMutex();
    if (g_requests == nullptr || g_ioLock == nullptr) return false;
    if (xQueueSend(g_requests, &request, 0) != pdTRUE) return false;
    if (!g_running.exchange(true) &&
        xTaskCreatePinnedToCore(Worker, "usb_drive", kStackBytes, nullptr, 2, nullptr, tskNO_AFFINITY) != pdPASS)
    {
        g_running.store(false);
        xQueueReset(g_requests);
        ESP_LOGE(kTag, "no memory for the drive's task");
        return false;
    }
    return true;
}

// From the USB task (never waits).
void PostFromUsbTask(Request request)
{
    if (g_requests != nullptr && g_running.load()) xQueueSend(g_requests, &request, 0);
}

// Waits for the state the request leads to: a change after `changes`, then not Starting or Returning.
Drive::Status Await(uint32_t changes, uint32_t waitMs)
{
    for (uint32_t waited = 0;; waited += 10)
    {
        uint32_t now = 0;
        const Drive::Status status = ReadStatus(&now);
        const bool moving = status.state == Drive::State::Starting || status.state == Drive::State::Returning;
        if ((now != changes && !moving) || waited >= waitMs) return status;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif  // CFG_TUD_MSC

}  // namespace

namespace Drive {

bool Share(uint32_t waitMs)
{
#if CFG_TUD_MSC
    uint32_t changes = 0;
    if (ReadStatus(&changes).state == State::Shared) return true;
    if (!Post(Request::Share)) return false;
    if (waitMs == 0) return true;
    return Await(changes, waitMs).state == State::Shared;
#else
    (void)waitMs;
    portENTER_CRITICAL_SAFE(&g_statusLock);
    g_status.state = State::Failed;
    g_status.reason = Reason::NotBuilt;
    ++g_statusChanges;
    portEXIT_CRITICAL_SAFE(&g_statusLock);
    return false;
#endif
}

bool Stop(uint32_t waitMs)
{
#if CFG_TUD_MSC
    uint32_t changes = 0;
    const State state = ReadStatus(&changes).state;
    if (state == State::Off || state == State::Failed) return state == State::Off;
    if (!Post(Request::Stop)) return false;
    if (waitMs == 0) return true;
    return Await(changes, waitMs).state == State::Off;
#else
    (void)waitMs;
    return true;
#endif
}

Status GetStatus() { return ReadStatus(); }

void SetListener(Listener listener) { g_listener.store(listener); }

}  // namespace Drive

namespace detail {

#if CFG_TUD_MSC
std::size_t DriveDescriptor(uint8_t* out, std::size_t capacity, uint8_t itf, uint8_t stringIndex, uint8_t epOut, uint8_t epIn)
{
    const uint8_t block[] = {TUD_MSC_DESCRIPTOR(itf, stringIndex, epOut, epIn, 64)};
    if (sizeof(block) > capacity) return 0;
    std::memcpy(out, block, sizeof(block));
    return sizeof(block);
}

void DriveHostGone() { PostFromUsbTask(Request::HostGone); }
#else
std::size_t DriveDescriptor(uint8_t*, std::size_t, uint8_t, uint8_t, uint8_t, uint8_t) { return 0; }
void DriveHostGone() {}
#endif

}  // namespace detail

}  // namespace CardenzaHAL::USB

#if CFG_TUD_MSC
// The MSC callbacks (TinyUSB, on the USB task): the card as one LUN of 512-byte sectors, removable.
using namespace CardenzaHAL::USB;

namespace {

// SCSI sense codes (key, additional code, qualifier).
void MediumNotPresent(uint8_t lun) { tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); }

// The card for one read or write: false (sense set) when it is not the computer's or not there.
bool TakeCard(uint8_t lun)
{
    g_hostCommand.store(true);
    if (!g_mediumReady.load() || g_ioLock == nullptr)
    {
        MediumNotPresent(lun);
        return false;
    }
    if (xSemaphoreTake(g_ioLock, pdMS_TO_TICKS(kIoLockMs)) != pdTRUE)
    {
        MediumNotPresent(lun);
        return false;
    }
    if (!g_cardUp.load() || !g_mediumReady.load())
    {
        xSemaphoreGive(g_ioLock);
        MediumNotPresent(lun);
        return false;
    }
    return true;
}

}  // namespace

extern "C" {

void tud_msc_inquiry_cb(uint8_t, uint8_t vendor[8], uint8_t product[16], uint8_t revision[4])
{
    g_hostCommand.store(true);
    std::memcpy(vendor, "203     ", 8);
    std::memcpy(product, "Cardenza SD     ", 16);
    std::memcpy(revision, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    g_hostCommand.store(true);
    if (g_mediumReady.load() && g_cardUp.load()) return true;
    MediumNotPresent(lun);
    return false;
}

void tud_msc_capacity_cb(uint8_t, uint32_t* blockCount, uint16_t* blockSize)
{
    const bool ready = g_mediumReady.load() && g_cardUp.load();
    *blockCount = ready ? static_cast<uint32_t>(g_card.csd.capacity) : 0;
    *blockSize = 512;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bytes)
{
    if (!TakeCard(lun)) return -1;
    int32_t result = -1;
    if (!DriveLogic::BlockRangeOk(lba, bytes, offset, static_cast<uint32_t>(g_card.csd.capacity)))
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);  // logical block address out of range
    else if (sdmmc_read_sectors(&g_card, buffer, lba, bytes / 512) == ESP_OK)
    {
        g_sectorsRead.fetch_add(bytes / 512);
        result = static_cast<int32_t>(bytes);
    }
    else
    {
        g_ioErrors.fetch_add(1);
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);  // unrecovered read error
    }
    xSemaphoreGive(g_ioLock);
    return result;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bytes)
{
    if (!TakeCard(lun)) return -1;
    int32_t result = -1;
    if (!DriveLogic::BlockRangeOk(lba, bytes, offset, static_cast<uint32_t>(g_card.csd.capacity)))
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
    // Written to the card before the computer hears it is: nothing waits in a cache here.
    else if (sdmmc_write_sectors(&g_card, buffer, lba, bytes / 512) == ESP_OK)
    {
        g_sectorsWritten.fetch_add(bytes / 512);
        result = static_cast<int32_t>(bytes);
    }
    else
    {
        g_ioErrors.fetch_add(1);
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);  // write error
    }
    xSemaphoreGive(g_ioLock);
    return result;
}

// A microSD card has no write-protect switch the host could read: always writable.
bool tud_msc_is_writable_cb(uint8_t) { return true; }

bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool loadEject)
{
    g_hostCommand.store(true);
    if (!loadEject) return true;  // power conditions: nothing to do
    if (start) return g_mediumReady.load();  // "load": only while shared
    // Eject: no medium from now on, and the card goes back to the firmware.
    g_mediumReady.store(false);
    PostFromUsbTask(Request::Eject);
    return true;
}

bool tud_msc_prevent_allow_medium_removal_cb(uint8_t, uint8_t, uint8_t)
{
    // The computer may ask; the card still goes back when the firmware stops sharing (Stop) or the cable goes.
    g_hostCommand.store(true);
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const command[16], void*, uint16_t)
{
    g_hostCommand.store(true);
    switch (command[0])
    {
        case 0x35:  // SYNCHRONIZE CACHE (10): nothing is cached
        case 0x91:  // SYNCHRONIZE CACHE (16)
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);  // invalid command operation code
            return -1;
    }
}

}  // extern "C"
#endif  // CFG_TUD_MSC
