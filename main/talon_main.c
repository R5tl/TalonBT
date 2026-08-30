// Talon — ESP32-S3 WiFi-enabled original Xbox "Duke" controller.
//
// The native USB-OTG port plugs into the Xbox controller port and enumerates as
// a real Duke (XID device, 045E:0202). WiFi joins the LAN and serves a web UI /
// HTTP API that drives the input report — a controller you press from a browser.
//
// Console/flash is on UART0 (COM3). USB comes up before WiFi: the sibling
// Falcon project showed radio bring-up can disturb Xbox USB enumeration.
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "sdkconfig.h"
#include "xid_descriptors.h"
#include "talon.h"
#include "wifi_net.h"

static const char *TAG = "talon";

void webui_start(void);

// Physical recovery: hold the BOOT button (GPIO0) ~3 s to erase the stored
// WiFi credentials and drop back into the "Talon-Setup" provisioning AP —
// the way out of a wrong-password lockout without reflashing.
static void boot_button_task(void *arg) {
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            if (++held == 30) {
                ESP_LOGW(TAG, "BOOT held 3s — forgetting WiFi, entering setup mode");
                wifi_net_forget();
            }
        } else {
            held = 0;
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "boot: reset_reason=%d", (int)esp_reset_reason());
    ESP_LOGI(TAG, "Talon: Xbox Duke controller (XID) emulator starting");

    // esp_tinyusb builds its tud_descriptor_*_cb from the pointers passed here;
    // the XID class driver registers itself via usbd_app_driver_get_cb.
    const tusb_desc_device_t *dev; const uint8_t *cfg; const char **strs; int nstr;
    xid_get_descriptors(&dev, &cfg, &strs, &nstr);
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = dev,
        .string_descriptor        = strs,
        .string_descriptor_count  = nstr,
        .configuration_descriptor = cfg,
        .external_phy             = false,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB device installed — waiting for the Xbox to enumerate");

    // Give enumeration a quiet radio: wait for mount (or 8 s if the Xbox is off /
    // not polling yet), then bring WiFi + the web UI up.
    for (int i = 0; i < 80 && !tud_mounted(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "mounted=%d — starting WiFi", tud_mounted() ? 1 : 0);
    wifi_net_start();
    webui_start();
    xTaskCreate(boot_button_task, "bootbtn", 2560, NULL, tskIDLE_PRIORITY + 2, NULL);

    // Periodic UART status so a serial monitor sees the whole story even if
    // single event lines are missed — and so a reboot is obvious (t resets).
    for (uint32_t t = 0;; t++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        char ip[16] = "-"; int rssi = 0;
        wifi_net_up(ip, &rssi);
        uint16_t rl, rr; talon_get_rumble(&rl, &rr);
        static const char *modes[] = { "sta", "setup", "wps" };
        ESP_LOGI(TAG, "HB t=%lus mnt=%d net=%s ip=%s rssi=%d rst=%lu open=%lu xid=%lu "
                      "in_ok=%lu in_err=%lu rumble=%lu(%u/%u) free=%u",
                 (unsigned long)(t * 5), tud_mounted() ? 1 : 0,
                 modes[wifi_net_mode()], ip, rssi,
                 (unsigned long)g_xid_reset, (unsigned long)g_xid_open,
                 (unsigned long)g_xid_ctrl_xid, (unsigned long)g_xid_in_ok,
                 (unsigned long)g_xid_in_err, (unsigned long)g_xid_out_pkts,
                 rl, rr, (unsigned)esp_get_free_heap_size());
    }
}
