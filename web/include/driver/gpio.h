#pragma once

// GPIO on the web target: every pin keeps its mode, pulls and level. GPIO0 (the Func button, active
// low) reads the page's HOME key; an output on GPIO38 (the display backlight) dims the screen. Other
// inputs read their pull (or their own output level).

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "hal/gpio_types.h"
#include "soc/soc_caps.h"

CARDENZA_WEB_EXTERN_C_BEGIN

#define GPIO_PIN_COUNT (SOC_GPIO_PIN_COUNT)
#define GPIO_IS_VALID_GPIO(gpio_num) ((gpio_num) >= 0 && (gpio_num) < GPIO_PIN_COUNT && !((gpio_num) >= 22 && (gpio_num) <= 25))
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio_num) GPIO_IS_VALID_GPIO(gpio_num)
#define GPIO_IS_VALID_DIGITAL_IO_PAD(gpio_num) GPIO_IS_VALID_GPIO(gpio_num)

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

typedef intr_handle_t gpio_isr_handle_t;
typedef void (*gpio_isr_t)(void* arg);

esp_err_t gpio_config(const gpio_config_t* pGPIOConfig);
esp_err_t gpio_reset_pin(gpio_num_t gpio_num);
esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type);
esp_err_t gpio_intr_enable(gpio_num_t gpio_num);
esp_err_t gpio_intr_disable(gpio_num_t gpio_num);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
int gpio_get_level(gpio_num_t gpio_num);
esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull);
esp_err_t gpio_wakeup_enable(gpio_num_t gpio_num, gpio_int_type_t intr_type);
esp_err_t gpio_wakeup_disable(gpio_num_t gpio_num);
esp_err_t gpio_isr_register(void (*fn)(void*), void* arg, int intr_alloc_flags, gpio_isr_handle_t* handle);
esp_err_t gpio_pullup_en(gpio_num_t gpio_num);
esp_err_t gpio_pullup_dis(gpio_num_t gpio_num);
esp_err_t gpio_pulldown_en(gpio_num_t gpio_num);
esp_err_t gpio_pulldown_dis(gpio_num_t gpio_num);
esp_err_t gpio_install_isr_service(int intr_alloc_flags);
void gpio_uninstall_isr_service(void);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void* args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num);
esp_err_t gpio_set_drive_capability(gpio_num_t gpio_num, gpio_drive_cap_t strength);
esp_err_t gpio_get_drive_capability(gpio_num_t gpio_num, gpio_drive_cap_t* strength);
esp_err_t gpio_hold_en(gpio_num_t gpio_num);
esp_err_t gpio_hold_dis(gpio_num_t gpio_num);
void gpio_deep_sleep_hold_en(void);
void gpio_deep_sleep_hold_dis(void);
void gpio_iomux_in(uint32_t gpio_num, uint32_t signal_idx);
void gpio_iomux_out(uint8_t gpio_num, int func, bool out_en_inv);
esp_err_t gpio_sleep_sel_en(gpio_num_t gpio_num) CARDENZA_WEB_UNSUPPORTED;
esp_err_t gpio_sleep_sel_dis(gpio_num_t gpio_num) CARDENZA_WEB_UNSUPPORTED;
esp_err_t gpio_sleep_set_direction(gpio_num_t gpio_num, gpio_mode_t mode) CARDENZA_WEB_UNSUPPORTED;
esp_err_t gpio_sleep_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull) CARDENZA_WEB_UNSUPPORTED;
esp_err_t gpio_dump_io_configuration(FILE* out_stream, uint64_t io_bit_mask);

CARDENZA_WEB_EXTERN_C_END
