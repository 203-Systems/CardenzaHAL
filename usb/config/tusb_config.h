#pragma once

// TinyUSB configuration for CardenzaHAL::USB (usb/usb.h). The HAL's component hands this directory to
// the TinyUSB component as an include path; nothing else may configure TinyUSB in the same firmware.

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUSB_DEBUG 0
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_MIDI 1
#define CFG_TUD_HID 1
#define CFG_TUD_AUDIO 1

#define CFG_TUD_MIDI_RX_BUFSIZE 256
#define CFG_TUD_MIDI_TX_BUFSIZE 256
#define CFG_TUD_MIDI_EP_BUFSIZE 64

#define CFG_TUD_HID_EP_BUFSIZE 64

// Audio: one UAC2 function, stereo 16-bit at 44.1 kHz both ways. The host plays through an
// asynchronous OUT endpoint with a feedback endpoint, and records from an asynchronous IN endpoint
// whose packet sizes follow the FIFO level: both run on the device's clock.
#define CARDENZA_USB_AUDIO_RATE 44100
#define CARDENZA_USB_AUDIO_CHANNELS 2
#define CARDENZA_USB_AUDIO_BYTES 2
// 45 frames a millisecond at most (44.1 on average), a frame of 4 bytes.
#define CARDENZA_USB_AUDIO_EP_SIZE ((CARDENZA_USB_AUDIO_RATE / 1000 + 1) * CARDENZA_USB_AUDIO_CHANNELS * CARDENZA_USB_AUDIO_BYTES)
// The FIFOs between the endpoints and the firmware: about 11.6 ms each, room for a 256-frame audio
// block (5.8 ms) and as much again for the jitter of both sides. RAM is what limits them: they are static.
#define CARDENZA_USB_AUDIO_FIFO_SIZE 2048

#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN CARDENZA_USB_AUDIO_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 2
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ 64
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP 0
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 1
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL 1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX CARDENZA_USB_AUDIO_CHANNELS
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX CARDENZA_USB_AUDIO_CHANNELS
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX CARDENZA_USB_AUDIO_BYTES
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX CARDENZA_USB_AUDIO_BYTES
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX CARDENZA_USB_AUDIO_EP_SIZE
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX CARDENZA_USB_AUDIO_EP_SIZE
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ CARDENZA_USB_AUDIO_FIFO_SIZE
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ CARDENZA_USB_AUDIO_FIFO_SIZE

// The length of the audio function's descriptor (usb/usb_descriptors.h builds it).
#include "usb_audio_descriptor.h"

#ifdef __cplusplus
}
#endif
