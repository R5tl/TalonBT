// Talon — onboard WS2812 status LED (GPIO48 on this board). Mirrors Kratos's
// convention: breathing white while the SoftAP setup portal is up, blinking
// white during a WPS window, otherwise off.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TALON_LED_GPIO
#define TALON_LED_GPIO 48          // onboard WS2812 RGB LED
#endif

// Start the status-LED render task. Safe to call if the board has no WS2812 on
// the pin (the strip just drives nothing visible).
void led_status_start(void);

// Stop the LED task and blank the LED (used before an OTA flash write).
void led_status_stop(void);

#ifdef __cplusplus
}
#endif
