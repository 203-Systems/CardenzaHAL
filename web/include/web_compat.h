#pragma once

// Shared by the web target's ESP-IDF headers.
//
// CARDENZA_WEB_UNSUPPORTED marks a function the device has but the web target does not provide:
// calling it is a build error that says so ("does not follow the HAL" for a firmware: it reaches
// past what the HAL and its native handles offer). The build tool (tools/cardenza_web.py) lists them.
#define CARDENZA_HAL_WEB 1

#if defined(__clang__)
#define CARDENZA_WEB_UNSUPPORTED __attribute__((unavailable("not provided by the CardenzaHAL web target")))
#else
#define CARDENZA_WEB_UNSUPPORTED
#endif

// CARDENZA_WEB_HAL_ONLY marks what CardenzaHAL's own sources call to set up hardware it owns (the
// display's panel and panel IO): the HAL builds with CARDENZA_WEB_HAL_SOURCE, a firmware uses the
// handles the HAL gives instead.
#if defined(CARDENZA_WEB_HAL_SOURCE) || !defined(__clang__)
#define CARDENZA_WEB_HAL_ONLY
#else
#define CARDENZA_WEB_HAL_ONLY __attribute__((unavailable("set up by CardenzaHAL: use the handle the HAL gives (Display::GetPanel / GetPanelIO)")))
#endif

#ifdef __cplusplus
#define CARDENZA_WEB_EXTERN_C_BEGIN extern "C" {
#define CARDENZA_WEB_EXTERN_C_END }
#else
#define CARDENZA_WEB_EXTERN_C_BEGIN
#define CARDENZA_WEB_EXTERN_C_END
#endif
