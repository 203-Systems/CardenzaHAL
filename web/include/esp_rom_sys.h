#pragma once
#include <stdint.h>
#include "web_compat.h"
CARDENZA_WEB_EXTERN_C_BEGIN
int esp_rom_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void esp_rom_delay_us(uint32_t us);
void esp_rom_install_channel_putc(int channel, void (*putc)(char c));
void esp_rom_install_uart_printf(void);
uint32_t esp_rom_get_cpu_ticks_per_us(void);
void esp_rom_software_reset_system(void) __attribute__((noreturn));
CARDENZA_WEB_EXTERN_C_END
