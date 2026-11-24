// The USB camera (usb/camera.h): the screen as a lossless stream on the WebUSB interface's bulk IN endpoint, for
// recording and streaming tutorials. Protocol: docs/screen-stream.md; pure parts: usb/camera_logic.h.
//
// Three places:
// - The firmware's Present* (display/internal/stream.h, the display's lock held): the rows it presented are
//   compared with the shadow, a copy of the screen as last captured, and the 8-pixel segments that differ are
//   copied there and marked dirty. Only while a computer reads the stream: otherwise the hook is not set.
// - The stream's task (priority 1, made when a computer starts the stream, gone when it stops): when something is
//   dirty and no frame is on its way, it writes the dirty rectangles from the shadow into the frame buffer (under
//   the lock the hook takes, so a frame is one consistent picture) and starts the transfer. Whatever the firmware
//   presents meanwhile is merged into the next frame: a slow computer gets fewer frames, never a backlog.
// - The USB task: the transfer's end (a zero-length packet after a frame of whole packets, so every frame ends a
//   transfer), the control requests, the endpoint coming and going.
#include "usb/camera.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "display/internal/snapshot.h"
#include "display/internal/stream.h"
#include "usb/camera_logic.h"
#include "usb/internal/camera.h"
#include "usb/internal/dev.h"
#include "usb/usb.h"

namespace CardenzaHAL::USB {

namespace {

using namespace ScreenStream;

constexpr char kTag[] = "camera";
#if CARDENZA_USB_BUILD_CAMERA
constexpr uint32_t kStallUs = 2000000;       // a frame not read for this long: the computer stopped reading
constexpr int64_t kFirstFrameWaitUs = 200000; // no Present this long after the start: take the last framebuffer
constexpr TickType_t kHookLockWait = pdMS_TO_TICKS(2);
constexpr std::size_t kMaxRects = 96;
constexpr uint32_t kStackBytes = 3584;
constexpr std::size_t kShadowBytes = static_cast<std::size_t>(kWidth) * kHeight * sizeof(uint16_t);

// Set by the USB task (control requests, the endpoint), read by the stream's task.
std::atomic<bool> g_endpointOpen{false};
std::atomic<bool> g_wanted{false};        // a computer started the stream
std::atomic<bool> g_allowRle{false};
std::atomic<bool> g_keyRequested{false};
std::atomic<bool> g_inFlight{false};      // a frame (or its zero-length end) is on its way
std::atomic<bool> g_zlpPending{false};
std::atomic<bool> g_stalled{false};
std::atomic<int64_t> g_sentAtUs{0};
portMUX_TYPE g_taskMux = portMUX_INITIALIZER_UNLOCKED;
bool g_taskRunning = false;               // under g_taskMux
std::atomic<TaskHandle_t> g_task{nullptr};

// The capture: the shadow, what is dirty, the frame being sent. PSRAM, made by the stream's task, under g_lock
// (the hook and the task).
SemaphoreHandle_t g_lock = nullptr;
uint16_t* g_shadow = nullptr;
uint8_t* g_frame = nullptr;
Dirty* g_dirty = nullptr;
bool g_captureAll = true;   // the next Present is compared whole
bool g_haveFrame = false;   // the shadow holds a frame
bool g_keyPending = true;   // the next frame is a keyframe
uint16_t g_presentsSince = 0;
uint32_t g_presentMs = 0;
Camera::Stats g_stats;      // under g_lock (counters the USB task adds: atomics below)
std::atomic<uint32_t> g_bytesSent{0}, g_errors{0}, g_sequence{0};

// The control requests' answers (EP0 reads them after the request returns).
uint8_t g_info[kInfoBytes];
uint8_t g_statsOut[kStatsBytes];
uint8_t g_zlp[4] __attribute__((aligned(4)));

void Notify()
{
    if (TaskHandle_t task = g_task.load()) xTaskNotifyGive(task);
}

// The firmware's Present: what it changed, into the shadow.
void OnPresent(const uint16_t* framebuffer, const Display::Region* regions, std::size_t count)
{
    if (framebuffer == nullptr || g_shadow == nullptr) return;
    const int64_t start = esp_timer_get_time();
    if (xSemaphoreTake(g_lock, kHookLockWait) != pdTRUE)
    {
        // The stream's task is writing a frame: this Present is not compared, so the next one is, whole.
        g_captureAll = true;
        return;
    }
    if (g_shadow == nullptr)
    {
        xSemaphoreGive(g_lock);
        return;
    }
    bool any = false;
    if (g_captureAll)
    {
        any = Capture(framebuffer, g_shadow, 0, kHeight, 0, kSegments, *g_dirty);
        g_captureAll = false;
        g_haveFrame = true;
    }
    else if (g_haveFrame)
    {
        // The segments each band of eight rows was presented in.
        int first[kBands], last[kBands];
        std::fill(first, first + kBands, kSegments);
        std::fill(last, last + kBands, 0);
        for (std::size_t index = 0; index < count; ++index)
        {
            const Display::Region& region = regions[index];
            const int top = std::max(region.y, 0), bottom = std::min(region.y + region.height, kHeight);
            if (top >= bottom || region.width <= 0) continue;
            int segmentFirst = 0, segmentLast = 0;
            SegmentSpan(region, segmentFirst, segmentLast);
            for (int band = top / kBandRows; band <= (bottom - 1) / kBandRows; ++band)
            {
                first[band] = std::min(first[band], segmentFirst);
                last[band] = std::max(last[band], segmentLast);
            }
        }
        for (int band = 0; band < kBands; ++band)
            if (first[band] < last[band])
                any = Capture(framebuffer, g_shadow, band * kBandRows, std::min((band + 1) * kBandRows, kHeight), first[band],
                              last[band], *g_dirty) ||
                      any;
    }
    if (g_presentsSince < 0xFFFF) ++g_presentsSince;
    g_presentMs = static_cast<uint32_t>(start / 1000);
    ++g_stats.presents;
    const uint32_t took = static_cast<uint32_t>(esp_timer_get_time() - start);
    g_stats.captureUs = took;
    g_stats.maxCaptureUs = std::max(g_stats.maxCaptureUs, took);
    xSemaphoreGive(g_lock);
    if (any) Notify();
}

bool Allocate()
{
    g_shadow = static_cast<uint16_t*>(heap_caps_malloc(kShadowBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_frame = static_cast<uint8_t*>(heap_caps_malloc(kMaxFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    void* dirty = heap_caps_malloc(sizeof(Dirty), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_dirty = dirty != nullptr ? new (dirty) Dirty() : nullptr;
    if (g_shadow != nullptr && g_frame != nullptr && g_dirty != nullptr) return true;
    ESP_LOGE(kTag, "no PSRAM for the screen stream (%u KB)", unsigned((kShadowBytes + kMaxFrameBytes) / 1024));
    return false;
}

void Free()
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    heap_caps_free(g_shadow);
    heap_caps_free(g_frame);
    heap_caps_free(g_dirty);
    g_shadow = nullptr;
    g_frame = nullptr;
    g_dirty = nullptr;
    xSemaphoreGive(g_lock);
}

// The next frame from the shadow into g_frame. 0: nothing to send.
std::size_t WriteNext()
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const int64_t start = esp_timer_get_time();
    if (!g_haveFrame || (!g_keyPending && !g_dirty->Any()))
    {
        xSemaphoreGive(g_lock);
        return 0;
    }
    static Rect rects[kMaxRects];
    const Rect whole{0, 0, kWidth, kHeight};
    std::size_t count = 0;
    bool key = g_keyPending || g_dirty->Everything();
    if (!key)
    {
        count = g_dirty->Rects(rects, kMaxRects);
        key = count > kMaxRects || RawFrameBytes(rects, count) >= kKeyframeBytes;
    }
    FrameInfo info;
    info.sequence = g_sequence.load();
    info.timeMs = g_presentMs;
    info.presents = g_presentsSince;
    info.allowRle = g_allowRle.load();
    info.keyframe = key;
    std::size_t bytes = key ? 0 : WriteFrame(g_shadow, rects, count, info, g_frame, kMaxFrameBytes);
    if (bytes == 0)
    {
        info.keyframe = key = true;
        count = 1;
        bytes = WriteFrame(g_shadow, &whole, 1, info, g_frame, kMaxFrameBytes);
    }
    g_dirty->Clear();
    g_keyPending = false;
    ++g_stats.frames;
    if (key) ++g_stats.keyframes;
    g_stats.rects += static_cast<uint32_t>(key ? 1 : count);
    if (g_presentsSince > 1) g_stats.coalesced += g_presentsSince - 1u;
    g_presentsSince = 0;
    g_stats.lastFrameBytes = static_cast<uint32_t>(bytes);
    const uint32_t took = static_cast<uint32_t>(esp_timer_get_time() - start);
    g_stats.encodeUs = took;
    g_stats.maxEncodeUs = std::max(g_stats.maxEncodeUs, took);
    xSemaphoreGive(g_lock);
    return bytes;
}

// While a computer reads the stream.
void Stream()
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_captureAll = true;
    g_haveFrame = false;
    g_keyPending = true;
    g_presentsSince = 0;
    g_dirty->Clear();
    xSemaphoreGive(g_lock);
    g_stalled.store(false);
    Display::detail::SetPresentHook(OnPresent);
    const int64_t started = esp_timer_get_time();
    ESP_LOGI(kTag, "a computer reads the screen stream");
    while (g_wanted.load() && g_endpointOpen.load())
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        if (!g_wanted.load() || !g_endpointOpen.load()) break;
        const int64_t now = esp_timer_get_time();
        if (g_inFlight.load())
        {
            // The computer stopped reading (a page closed without stopping, a program paused): capture nothing
            // until it reads again, then begin with a keyframe.
            if (!g_stalled.load() && now - g_sentAtUs.load() > kStallUs)
            {
                g_stalled.store(true);
                Display::detail::SetPresentHook(nullptr);
                ESP_LOGW(kTag, "the computer stopped reading the screen stream: paused");
            }
            continue;
        }
        if (g_stalled.exchange(false))
        {
            xSemaphoreTake(g_lock, portMAX_DELAY);
            g_captureAll = true;
            g_keyPending = true;
            xSemaphoreGive(g_lock);
            Display::detail::SetPresentHook(OnPresent);
        }
        if (g_keyRequested.exchange(false))
        {
            xSemaphoreTake(g_lock, portMAX_DELAY);
            g_keyPending = true;
            xSemaphoreGive(g_lock);
        }
        // A firmware that presents nothing now (a still screen): the frame it last presented.
        if (!g_haveFrame && now - started > kFirstFrameWaitUs)
            if (const uint16_t* last = Display::detail::LastFramebuffer())
            {
                xSemaphoreTake(g_lock, portMAX_DELAY);
                if (!g_haveFrame)
                {
                    Capture(last, g_shadow, 0, kHeight, 0, kSegments, *g_dirty);
                    g_haveFrame = true;
                    g_captureAll = true;  // and the next Present is compared whole (this copy may have been mid-draw)
                }
                xSemaphoreGive(g_lock);
            }
        const std::size_t bytes = WriteNext();
        if (bytes == 0) continue;
        g_zlpPending.store(bytes % 64 == 0);
        g_sentAtUs.store(esp_timer_get_time());
        g_inFlight.store(true);
        if (detail::CameraTransfer(g_frame, static_cast<uint16_t>(bytes)))
            g_sequence.fetch_add(1);
        else
        {
            // It did not go: the next frame paints everything.
            g_inFlight.store(false);
            g_zlpPending.store(false);
            g_errors.fetch_add(1);
            xSemaphoreTake(g_lock, portMAX_DELAY);
            g_keyPending = true;
            xSemaphoreGive(g_lock);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    Display::detail::SetPresentHook(nullptr);
    ESP_LOGI(kTag, "screen stream stopped");
}

void Task(void*)
{
    for (;;)
    {
        if (g_wanted.load() && g_endpointOpen.load())
        {
            if (Allocate())
                Stream();
            else
            {
                g_errors.fetch_add(1);
                g_wanted.store(false);
            }
        }
        // The frame buffer stays until the transfer reading it ends (or the endpoint goes, which ends it).
        while (g_inFlight.load() && g_endpointOpen.load()) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        g_inFlight.store(false);
        Free();
        bool again = false;
        portENTER_CRITICAL(&g_taskMux);
        if (g_wanted.load() && g_endpointOpen.load())
            again = true;
        else
        {
            g_taskRunning = false;
            g_task.store(nullptr);
        }
        portEXIT_CRITICAL(&g_taskMux);
        if (!again) break;
    }
    vTaskDelete(nullptr);
}

// A computer started the stream: the task, if it is not there.
bool Start()
{
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < kMaxFrameBytes ||
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kShadowBytes + kMaxFrameBytes + sizeof(Dirty) + 1024)
    {
        g_errors.fetch_add(1);
        ESP_LOGE(kTag, "the screen stream needs %u KB of PSRAM", unsigned((kShadowBytes + kMaxFrameBytes) / 1024));
        return false;
    }
    if (g_lock == nullptr) g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) return false;
    bool create = false;
    portENTER_CRITICAL(&g_taskMux);
    g_wanted.store(true);
    if (!g_taskRunning)
    {
        g_taskRunning = true;
        create = true;
    }
    portEXIT_CRITICAL(&g_taskMux);
    if (!create)
    {
        g_keyRequested.store(true);  // a new reader begins with a keyframe
        Notify();
        return true;
    }
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(Task, "usbcam", kStackBytes, nullptr, 1, &task, tskNO_AFFINITY) != pdPASS)
    {
        portENTER_CRITICAL(&g_taskMux);
        g_taskRunning = false;
        g_wanted.store(false);
        portEXIT_CRITICAL(&g_taskMux);
        g_errors.fetch_add(1);
        return false;
    }
    g_task.store(task);
    return true;
}

Camera::Stats Snapshot()
{
    Camera::Stats stats;
    if (g_lock != nullptr && xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        stats = g_stats;
        xSemaphoreGive(g_lock);
    }
    stats.enabled = USB::Classes().camera;
    stats.open = g_endpointOpen.load();
    stats.streaming = g_wanted.load() && stats.open;
    stats.stalled = g_stalled.load();
    stats.bytes = g_bytesSent.load();
    stats.errors = g_errors.load();
    stats.sequence = g_sequence.load();
    return stats;
}
#endif  // CARDENZA_USB_BUILD_CAMERA

}  // namespace

namespace detail {

#if CARDENZA_USB_BUILD_CAMERA
void CameraOpened(bool open)
{
    g_endpointOpen.store(open);
    if (!open)
    {
        // A reset or the cable: the transfer is gone with the endpoint, and the computer starts again.
        g_inFlight.store(false);
        g_zlpPending.store(false);
        g_wanted.store(false);
        Notify();
    }
}

void CameraSent(bool ok, uint32_t bytes)
{
    g_bytesSent.fetch_add(bytes);
    if (!ok) g_errors.fetch_add(1);
    // A frame of whole packets ends with a zero-length one, so a reader's transfer ends with the frame.
    if (ok && g_zlpPending.exchange(false) && CameraTransfer(g_zlp, 0)) return;
    g_inFlight.store(false);
    Notify();
}

bool CameraControl(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request)
{
    if (stage != CONTROL_STAGE_SETUP) return true;
    const bool in = request->bmRequestType_bit.direction == TUSB_DIR_IN;
    switch (request->bRequest)
    {
        case kRequestStart:
            if (in || !g_endpointOpen.load()) return false;
            g_allowRle.store(request->wValue & kStartAllowRle);
            if (!Start()) return false;  // stalled: the computer sees the request fail
            return tud_control_status(rhport, request);
        case kRequestStop:
            if (in) return false;
            g_wanted.store(false);
            Notify();
            return tud_control_status(rhport, request);
        case kRequestKeyframe:
            if (in) return false;
            g_keyRequested.store(true);
            Notify();
            return tud_control_status(rhport, request);
        case kRequestInfo:
            if (!in) return false;
            WriteInfo(g_info, g_wanted.load(), g_stalled.load(), g_sequence.load());
            return tud_control_xfer(rhport, request, g_info, std::min<uint16_t>(request->wLength, kInfoBytes));
        case kRequestStats:
        {
            if (!in) return false;
            // From the USB task: the counters without waiting for the capture's lock.
            Camera::Stats stats;
            if (g_lock != nullptr && xSemaphoreTake(g_lock, 0) == pdTRUE)
            {
                stats = g_stats;
                xSemaphoreGive(g_lock);
            }
            stats.enabled = true;
            stats.open = g_endpointOpen.load();
            stats.streaming = g_wanted.load() && stats.open;
            stats.stalled = g_stalled.load();
            stats.bytes = g_bytesSent.load();
            stats.errors = g_errors.load();
            stats.sequence = g_sequence.load();
            WriteStats(g_statsOut, stats);
            return tud_control_xfer(rhport, request, g_statsOut, std::min<uint16_t>(request->wLength, kStatsBytes));
        }
        default: return false;
    }
}
#else
void CameraOpened(bool) {}
void CameraSent(bool, uint32_t) {}
bool CameraControl(uint8_t, uint8_t, tusb_control_request_t const*) { return false; }
#endif

}  // namespace detail

namespace Camera {

bool Enable(bool on)
{
#if CARDENZA_USB_BUILD_CAMERA
    if (!Started()) return false;
    Logic::Classes asked = detail::AskedClasses();
    if (asked.camera == on && Classes().camera == on) return true;
    asked.camera = on;
    Logic::Classes keep;
    keep.camera = on;
    return detail::SetClasses(asked, keep) && Classes().camera == on;
#else
    return !on;
#endif
}

bool Enabled() { return Classes().camera; }

bool Streaming()
{
#if CARDENZA_USB_BUILD_CAMERA
    return g_wanted.load() && g_endpointOpen.load();
#else
    return false;
#endif
}

Stats GetStats()
{
#if CARDENZA_USB_BUILD_CAMERA
    return Snapshot();
#else
    return Stats{};
#endif
}

}  // namespace Camera

}  // namespace CardenzaHAL::USB
