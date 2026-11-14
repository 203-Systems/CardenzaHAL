// The web target's kernel on the host: FreeRTOS semantics a firmware relies on, with a simulated
// clock (sim_clock.h) so every expected time is exact. The contexts are ucontexts here, Emscripten
// fibers in the browser; everything above them is the same code.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_timer.h"

#include "sim_clock.h"

namespace k = cardenza_web::kernel;

namespace {

std::vector<std::string> g_trace;
int g_failures = 0;

void Expect(bool condition, const char* what, int line)
{
    if (condition) return;
    std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
    ++g_failures;
}
#define EXPECT(condition) Expect((condition), #condition, __LINE__)

void Mark(const std::string& event) { g_trace.push_back(event + "@" + std::to_string(sim::g_now)); }

// ---- delays are tick-aligned and exact ----------------------------------------------------------------

uint64_t g_wokeAt = 0;

void Sleeper(void*)
{
    vTaskDelay(pdMS_TO_TICKS(10));
    g_wokeAt = sim::g_now;
    TickType_t last = xTaskGetTickCount();
    for (int index = 0; index < 3; ++index) xTaskDelayUntil(&last, pdMS_TO_TICKS(5));
    Mark("periodic-done");
    vTaskDelete(nullptr);
}

void TestDelays()
{
    sim::g_now = 1000500;  // mid-tick 1000: a 10-tick delay ends at tick 1010, as FreeRTOS counts
    g_trace.clear();
    xTaskCreate(Sleeper, "sleeper", 2048, nullptr, 5, nullptr);
    sim::RunUntil(sim::g_now + 100000);
    EXPECT(g_wokeAt == 1010000);
    EXPECT(!g_trace.empty() && g_trace.back() == "periodic-done@1025000");
}

// ---- a give wakes a higher-priority task at once ------------------------------------------------------

SemaphoreHandle_t g_semaphore;

void High(void*)
{
    for (int round = 0; round < 2; ++round)
    {
        xSemaphoreTake(g_semaphore, portMAX_DELAY);
        Mark("high");
    }
    vTaskDelete(nullptr);
}

void Low(void*)
{
    Mark("low-before");
    xSemaphoreGive(g_semaphore);
    Mark("low-after");
    xSemaphoreGive(g_semaphore);
    Mark("low-end");
    vTaskDelete(nullptr);
}

void TestPreemption()
{
    g_trace.clear();
    g_semaphore = xSemaphoreCreateBinary();
    // Both on core 0: one core, FreeRTOS's rule decides.
    xTaskCreatePinnedToCore(High, "high", 2048, nullptr, 10, nullptr, 0);
    xTaskCreatePinnedToCore(Low, "low", 2048, nullptr, 2, nullptr, 0);
    sim::RunUntil(sim::g_now + 1000);
    std::string order;
    for (const auto& event : g_trace) order += event.substr(0, event.find('@')) + " ";
    EXPECT(order == "low-before high low-after high low-end ");
    vSemaphoreDelete(g_semaphore);
}

// ---- queues: blocking, timeouts, order ----------------------------------------------------------------

QueueHandle_t g_queue;
int g_received[4];
BaseType_t g_timeoutResult = 99;
uint64_t g_timeoutAt = 0;

void Consumer(void*)
{
    for (int& value : g_received) xQueueReceive(g_queue, &value, portMAX_DELAY);
    int value = 0;
    const uint64_t before = sim::g_now;
    g_timeoutResult = xQueueReceive(g_queue, &value, pdMS_TO_TICKS(5));
    g_timeoutAt = sim::g_now - before;
    vTaskDelete(nullptr);
}

void Producer(void*)
{
    for (int value = 1; value <= 3; ++value)
    {
        xQueueSend(g_queue, &value, portMAX_DELAY);  // the queue holds two: the third waits for room
        vTaskDelay(1);
    }
    const int front = 0;
    xQueueSendToFront(g_queue, &front, portMAX_DELAY);
    vTaskDelete(nullptr);
}

void TestQueues()
{
    g_queue = xQueueCreate(2, sizeof(int));
    xTaskCreate(Producer, "producer", 2048, nullptr, 5, nullptr);
    xTaskCreate(Consumer, "consumer", 2048, nullptr, 4, nullptr);
    sim::RunUntil(sim::g_now + 50000);
    EXPECT(g_received[0] == 1 && g_received[1] == 2 && g_received[2] == 3 && g_received[3] == 0);
    EXPECT(g_timeoutResult == pdFALSE);
    EXPECT(g_timeoutAt >= 4000 && g_timeoutAt <= 6000);
    vQueueDelete(g_queue);
}

// ---- mutexes lend their holder the waiter's priority --------------------------------------------------

SemaphoreHandle_t g_mutex;
UBaseType_t g_holderPriorityWhileWaited = 0;
UBaseType_t g_holderPriorityAfter = 0;
TaskHandle_t g_holder;

void Holder(void*)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    vTaskDelay(2);  // the waiter arrives meanwhile
    g_holderPriorityWhileWaited = uxTaskPriorityGet(nullptr);
    xSemaphoreGive(g_mutex);
    g_holderPriorityAfter = uxTaskPriorityGet(nullptr);
    vTaskDelete(nullptr);
}

void Waiter(void*)
{
    vTaskDelay(1);
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    Mark("waiter-got-it");
    xSemaphoreGive(g_mutex);
    vTaskDelete(nullptr);
}

void TestInheritance()
{
    g_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(Holder, "holder", 2048, nullptr, 3, &g_holder, 0);
    xTaskCreatePinnedToCore(Waiter, "waiter", 2048, nullptr, 12, nullptr, 0);
    sim::RunUntil(sim::g_now + 20000);
    EXPECT(g_holderPriorityWhileWaited == 12);
    EXPECT(g_holderPriorityAfter == 3);
    EXPECT(xSemaphoreGive(g_mutex) == pdFAIL);  // not the holder: refused
    vSemaphoreDelete(g_mutex);
}

// ---- notifications and event groups -------------------------------------------------------------------

TaskHandle_t g_notified;
uint32_t g_taken[3];
EventGroupHandle_t g_events;
EventBits_t g_bitsA = 0, g_bitsB = 0;

void Notified(void*)
{
    for (uint32_t& value : g_taken) value = ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
    vTaskDelete(nullptr);
}

void WaitAll(void*)
{
    g_bitsA = xEventGroupWaitBits(g_events, 0x3, pdTRUE, pdTRUE, portMAX_DELAY);
    vTaskDelete(nullptr);
}

void WaitAny(void*)
{
    g_bitsB = xEventGroupWaitBits(g_events, 0x2, pdFALSE, pdFALSE, portMAX_DELAY);
    vTaskDelete(nullptr);
}

void Setter(void*)
{
    xTaskNotifyGive(g_notified);
    xTaskNotifyGive(g_notified);
    vTaskDelay(1);
    xTaskNotifyGive(g_notified);
    xEventGroupSetBits(g_events, 0x1);
    vTaskDelay(1);
    xEventGroupSetBits(g_events, 0x2);  // both waiters satisfied by the same bits; then 0x3 is cleared
    vTaskDelete(nullptr);
}

void TestNotificationsAndEvents()
{
    g_events = xEventGroupCreate();
    // On one core, the setter above the notified task: both gives land before the first take.
    xTaskCreatePinnedToCore(Notified, "notified", 2048, nullptr, 4, &g_notified, 0);
    xTaskCreate(WaitAll, "all", 2048, nullptr, 6, nullptr);
    xTaskCreate(WaitAny, "any", 2048, nullptr, 6, nullptr);
    xTaskCreatePinnedToCore(Setter, "setter", 2048, nullptr, 5, nullptr, 0);
    sim::RunUntil(sim::g_now + 10000);
    // Counting semantics: the first take sees both gives, takes one (2), then one is left (1)...
    EXPECT(g_taken[0] == 2 && g_taken[1] == 1 && g_taken[2] == 1);
    EXPECT((g_bitsA & 0x3) == 0x3);
    EXPECT((g_bitsB & 0x2) == 0x2);
    EXPECT(xEventGroupGetBits(g_events) == 0);
    vEventGroupDelete(g_events);
}

// ---- timers: FreeRTOS software timers and esp_timer ---------------------------------------------------

int g_softwareTicks = 0;
int g_espTicks = 0;
uint64_t g_oneShotAt = 0;

void SoftwareTick(TimerHandle_t) { ++g_softwareTicks; }
void EspTick(void*) { ++g_espTicks; }
void OneShot(void*) { g_oneShotAt = sim::g_now; }

void TestTimers()
{
    const uint64_t start = sim::g_now;
    TimerHandle_t timer = xTimerCreate("tick", pdMS_TO_TICKS(10), pdTRUE, nullptr, SoftwareTick);
    xTimerStart(timer, 0);
    esp_timer_handle_t periodic = nullptr, once = nullptr;
    esp_timer_create_args_t args = {};
    args.callback = EspTick;
    args.name = "periodic";
    esp_timer_create(&args, &periodic);
    args.callback = OneShot;
    args.name = "once";
    esp_timer_create(&args, &once);
    esp_timer_start_periodic(periodic, 2500);
    esp_timer_start_once(once, 7777);
    sim::RunUntil(start + 100000 + 500);
    EXPECT(g_softwareTicks == 10);
    EXPECT(g_espTicks == 40);
    EXPECT(g_oneShotAt == start + 7777);
    xTimerStop(timer, 0);
    xTimerDelete(timer, 0);
    esp_timer_stop(periodic);
    esp_timer_delete(periodic);
    esp_timer_delete(once);
}

// ---- two cores: a busy task on one does not starve the other ------------------------------------------

volatile bool g_flag = false;
uint64_t g_spinEnded = 0;

void Spinner(void*)
{
    // Waits for a flag another core's task sets, calling into the kernel as it polls (a preemption point).
    while (!g_flag) (void)xTaskGetTickCount();
    g_spinEnded = sim::g_now;
    vTaskDelete(nullptr);
}

void FlagSetter(void*)
{
    vTaskDelay(3);
    g_flag = true;
    vTaskDelete(nullptr);
}

void TestCores()
{
    sim::g_ticking = true;  // busy loops take time
    const uint64_t start = sim::g_now;
    xTaskCreatePinnedToCore(Spinner, "spinner", 2048, nullptr, 20, nullptr, 0);
    xTaskCreatePinnedToCore(FlagSetter, "setter", 2048, nullptr, 1, nullptr, 1);
    sim::RunUntil(start + 50000);
    sim::g_ticking = false;
    EXPECT(g_flag);
    EXPECT(g_spinEnded != 0 && g_spinEnded - start < 10000);
}

// ---- message buffers ----------------------------------------------------------------------------------

MessageBufferHandle_t g_messages;
char g_message[32];

void MessageReader(void*)
{
    const size_t length = xMessageBufferReceive(g_messages, g_message, sizeof(g_message), portMAX_DELAY);
    g_message[length] = '\0';
    vTaskDelete(nullptr);
}

void MessageWriter(void*)
{
    vTaskDelay(2);
    xMessageBufferSend(g_messages, "cardenza", 8, portMAX_DELAY);
    vTaskDelete(nullptr);
}

void TestMessages()
{
    g_messages = xMessageBufferCreate(64);
    xTaskCreate(MessageReader, "reader", 2048, nullptr, 5, nullptr);
    xTaskCreate(MessageWriter, "writer", 2048, nullptr, 5, nullptr);
    sim::RunUntil(sim::g_now + 10000);
    EXPECT(std::strcmp(g_message, "cardenza") == 0);
    vMessageBufferDelete(g_messages);
}

}  // namespace

int main()
{
    sim::Start();
    TestDelays();
    TestPreemption();
    TestQueues();
    TestInheritance();
    TestNotificationsAndEvents();
    TestTimers();
    TestCores();
    TestMessages();
    if (g_failures == 0) std::printf("kernel tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
