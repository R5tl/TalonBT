// Talon — onboard WS2812 status LED (see led_status.h). Timing/behavior ported
// from Kratos's led_controller status pixel.
#include "led_status.h"
#include "wifi_net.h"
#include "esp_log.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "talon.led";

#define BREATHE_MS 2400    // AP setup: slow up/down
#define BLINK_MS   1000    // WPS: hard on/off
#define REFRESH_MS 16      // ~60 fps

static led_strip_handle_t s_strip;
static TaskHandle_t s_task;

// Stop the LED render task and blank the LED. Called before OTA: the RMT driver
// this task drives does blocking semaphore waits, which assert if they land in
// the scheduler-suspended window of a flash write.
void led_status_stop(void) {
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    if (s_strip) { led_strip_clear(s_strip); }
}

static void set_white(uint8_t level) {
    // White = equal RGB; scale by level. Cap brightness so a bare board's LED
    // isn't blinding (Kratos runs the status pixel dim too).
    uint8_t v = (uint16_t)level * 160 / 255;
    led_strip_set_pixel(s_strip, 0, v, v, v);
    led_strip_refresh(s_strip);
}

static void led_task(void *arg) {
    (void)arg;
    uint32_t frame = 0;
    talon_net_mode_t last_mode = TALON_NET_STA;
    for (;;) {
        talon_net_mode_t mode = wifi_net_mode();
        if (mode != last_mode) { frame = 0; last_mode = mode; }

        if (mode == TALON_NET_AP_SETUP) {
            uint32_t period = BREATHE_MS / REFRESH_MS;
            uint32_t half = period / 2;
            uint32_t phase = frame % period;
            uint32_t level = phase < half ? (phase * 255 / half)
                                          : ((period - phase) * 255 / half);
            if (level < 24) level = 24;             // dim floor so it's always visible
            set_white((uint8_t)level);
        } else if (mode == TALON_NET_WPS) {
            uint32_t period = BLINK_MS / REFRESH_MS;
            uint32_t phase = frame % period;
            set_white(phase < period / 2 ? 255 : 0);
        } else {
            // Normal operation: off (like Kratos). The web UI shows link state.
            led_strip_set_pixel(s_strip, 0, 0, 0, 0);
            led_strip_refresh(s_strip);
        }
        frame++;
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

void led_status_start(void) {
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = TALON_LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "no WS2812 on GPIO%d — status LED disabled", TALON_LED_GPIO);
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "led_status", 2560, NULL, tskIDLE_PRIORITY + 1, &s_task);
    ESP_LOGI(TAG, "status LED on GPIO%d", TALON_LED_GPIO);
}
