#pragma once

// The UAC2 function of CardenzaHAL::USB: three interfaces (control, the host's playback, the host's
// recording) on one fixed 44.1 kHz clock. Playback: USB streaming -> feature unit (the host's mute and
// volume) -> speaker. Recording: microphone -> USB streaming. No interrupt endpoint: the ESP32-S3 has
// no IN endpoint left for it. TinyUSB's descriptor macros (device/usbd.h) are expanded where this is
// used, so this header needs nothing else.

#define CARDENZA_USB_AUDIO_CLOCK 0x04
#define CARDENZA_USB_AUDIO_SPK_IN_TERM 0x01
#define CARDENZA_USB_AUDIO_SPK_FEATURE 0x02
#define CARDENZA_USB_AUDIO_SPK_OUT_TERM 0x03
#define CARDENZA_USB_AUDIO_MIC_IN_TERM 0x11
#define CARDENZA_USB_AUDIO_MIC_OUT_TERM 0x13

#define CARDENZA_USB_AUDIO_ENTITIES_LEN                                                                        \
    (TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2) + \
     TUD_AUDIO20_DESC_OUTPUT_TERM_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN)

#define CARDENZA_USB_AUDIO_STREAM_ALT1_LEN                                                         \
    (TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN + \
     TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

#define CARDENZA_USB_AUDIO_DESC_LEN                                                                      \
    (TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN + TUD_AUDIO20_DESC_CS_AC_LEN +               \
     CARDENZA_USB_AUDIO_ENTITIES_LEN +                                                                   \
     /* playback: alternate 0, alternate 1 with its data and feedback endpoints */                       \
     TUD_AUDIO20_DESC_STD_AS_LEN + CARDENZA_USB_AUDIO_STREAM_ALT1_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP_LEN + \
     /* recording: alternate 0, alternate 1 */                                                           \
     TUD_AUDIO20_DESC_STD_AS_LEN + CARDENZA_USB_AUDIO_STREAM_ALT1_LEN)

// _itf: the control interface; playback is _itf + 1, recording _itf + 2.
#define CARDENZA_USB_AUDIO_DESCRIPTOR(_itf, _stridx, _epout, _epfb, _epin)                                       \
    TUD_AUDIO20_DESC_IAD(_itf, 0x03, 0x00),                                                                       \
    TUD_AUDIO20_DESC_STD_AC(_itf, 0x00, _stridx),                                                                 \
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_IO_BOX, CARDENZA_USB_AUDIO_ENTITIES_LEN,                         \
                           AUDIO20_CS_AS_INTERFACE_CTRL_LATENCY_POS),                                            \
    TUD_AUDIO20_DESC_CLK_SRC(CARDENZA_USB_AUDIO_CLOCK, AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK,                      \
                             (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), 0x00, 0x00),              \
    TUD_AUDIO20_DESC_INPUT_TERM(CARDENZA_USB_AUDIO_SPK_IN_TERM, AUDIO_TERM_TYPE_USB_STREAMING, 0x00,             \
                                CARDENZA_USB_AUDIO_CLOCK, 0x02, AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00,     \
                                0x0000, 0x00),                                                                   \
    TUD_AUDIO20_DESC_FEATURE_UNIT(CARDENZA_USB_AUDIO_SPK_FEATURE, CARDENZA_USB_AUDIO_SPK_IN_TERM, 0x00,          \
                                  AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS |                        \
                                      AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS,                   \
                                  0x00000000, 0x00000000),                                                       \
    TUD_AUDIO20_DESC_OUTPUT_TERM(CARDENZA_USB_AUDIO_SPK_OUT_TERM, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, 0x00,     \
                                 CARDENZA_USB_AUDIO_SPK_FEATURE, CARDENZA_USB_AUDIO_CLOCK, 0x0000, 0x00),        \
    TUD_AUDIO20_DESC_INPUT_TERM(CARDENZA_USB_AUDIO_MIC_IN_TERM, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0x00,            \
                                CARDENZA_USB_AUDIO_CLOCK, 0x02, AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00,     \
                                0x0000, 0x00),                                                                   \
    TUD_AUDIO20_DESC_OUTPUT_TERM(CARDENZA_USB_AUDIO_MIC_OUT_TERM, AUDIO_TERM_TYPE_USB_STREAMING, 0x00,           \
                                 CARDENZA_USB_AUDIO_MIC_IN_TERM, CARDENZA_USB_AUDIO_CLOCK, 0x0000, 0x00),        \
    /* Playback, alternate 0: no bandwidth */                                                                    \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 1), 0x00, 0x00, 0x00),                                        \
    /* Playback, alternate 1: data out, feedback in */                                                           \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 1), 0x01, 0x02, 0x00),                                        \
    TUD_AUDIO20_DESC_CS_AS_INT(CARDENZA_USB_AUDIO_SPK_IN_TERM, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,         \
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, 0x02, AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00), \
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(CARDENZA_USB_AUDIO_BYTES, 16),                                                 \
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(_epout,                                                                        \
                                   (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS | \
                                             (uint8_t)TUSB_ISO_EP_ATT_DATA),                                     \
                                   CARDENZA_USB_AUDIO_EP_SIZE, 0x01),                                            \
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,           \
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),                   \
    TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP(_epfb, 4, 1),                                                               \
    /* Recording, alternate 0: no bandwidth */                                                                   \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 2), 0x00, 0x00, 0x00),                                        \
    /* Recording, alternate 1: data in */                                                                        \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 2), 0x01, 0x01, 0x00),                                        \
    TUD_AUDIO20_DESC_CS_AS_INT(CARDENZA_USB_AUDIO_MIC_OUT_TERM, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,        \
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, 0x02, AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00), \
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(CARDENZA_USB_AUDIO_BYTES, 16),                                                 \
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(_epin,                                                                         \
                                   (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS | \
                                             (uint8_t)TUSB_ISO_EP_ATT_DATA),                                     \
                                   CARDENZA_USB_AUDIO_EP_SIZE, 0x01),                                            \
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,           \
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000)
