#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/task.h"
#ifndef CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT
#define CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT 3072
#endif
#ifndef CONFIG_PTHREAD_TASK_PRIO_DEFAULT
#define CONFIG_PTHREAD_TASK_PRIO_DEFAULT 5
#endif
#ifndef CONFIG_PTHREAD_TASK_NAME_DEFAULT
#define CONFIG_PTHREAD_TASK_NAME_DEFAULT "pthread"
#endif
CARDENZA_WEB_EXTERN_C_BEGIN
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 768
#endif
typedef struct {
    size_t stack_size;
    size_t prio;
    bool inherit_cfg;
    const char* thread_name;
    int pin_to_core;
    uint32_t stack_alloc_caps;
} esp_pthread_cfg_t;
esp_pthread_cfg_t esp_pthread_get_default_config(void);
esp_err_t esp_pthread_set_cfg(const esp_pthread_cfg_t* cfg);
esp_err_t esp_pthread_get_cfg(esp_pthread_cfg_t* p);
esp_err_t esp_pthread_init(void);
CARDENZA_WEB_EXTERN_C_END
