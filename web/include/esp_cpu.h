#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
CARDENZA_WEB_EXTERN_C_BEGIN
typedef uint32_t esp_cpu_cycle_count_t;
typedef int esp_cpu_intr_type_t;
// The core the running task is on (one of two virtual cores) and a cycle count at 240 MHz.
int esp_cpu_get_core_id(void);
esp_cpu_cycle_count_t esp_cpu_get_cycle_count(void);
void esp_cpu_set_cycle_count(esp_cpu_cycle_count_t cycle_count);
void* esp_cpu_get_sp(void);
void esp_cpu_wait_for_intr(void);
bool esp_cpu_dbgr_is_attached(void);
void esp_cpu_dbgr_break(void);
static inline intptr_t esp_cpu_get_call_addr(intptr_t return_address) { return return_address; }
#define esp_cpu_process_stack_pc(pc) ((uint32_t)(pc))
CARDENZA_WEB_EXTERN_C_END
