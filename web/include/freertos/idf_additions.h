#pragma once
// ESP-IDF's additions to FreeRTOS (pinned creation, the WithCaps calls) are declared with the
// kernel's own headers on the web target.
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
