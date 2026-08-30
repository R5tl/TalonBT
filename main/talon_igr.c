// Talon — Cerbios In-Game Reset (IGR) combos (see talon_igr.h).
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "talon_igr.h"
#include "talon.h"
#include "esp_log.h"

static const char *TAG = "talon.igr";

// Cerbios button-code nibble (0..F) -> Talon control name. Order matches the
// legend Cerbios writes into its config.ini.
static const char *const NIBBLE[16] = {
    "a", "b", "x", "y", "black", "white", "lt", "rt",
    "up", "down", "left", "right", "start", "back", "ls", "rs",
};

// Cerbios default combos (CerbiosToolInternal Config.cs). Hold-then-release.
static const struct { const char *name, *combo; } IGR[] = {
    { "dash",     "67CD" },   // soft reset to dashboard (keeps mounted ISO/CCI)
    { "game",     "467C" },   // reload the running game/app
    { "full",     "467D" },   // full kernel reset (unmounts, back to dash)
    { "cycle",    "4678" },   // full power cycle / reboot
    { "shutdown", "678D" },   // shut the console down
    { "screen",   "4679" },   // screenshot (needs EnableScreenshots in Cerbios)
};

static int nibble_of(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// The press/hold/release runs on its own short-lived task so the httpd worker
// (esp_http_server serves requests on ONE task) returns immediately and the web
// UI / gamepad forwarding never freezes during the 800 ms hold.
static void igr_task(void *arg) {
    const char *combo = (const char *)arg;
    for (const char *p = combo; *p; p++) {
        int n = nibble_of(*p);
        if (n >= 0) talon_set_control(NIBBLE[n], 1);
    }
    ESP_LOGI(TAG, "IGR combo %s held", combo);
    vTaskDelay(pdMS_TO_TICKS(800));      // let the Cerbios input scan catch it
    talon_reset_controls();
    vTaskDelete(NULL);
}

bool talon_igr_trigger(const char *action) {
    const char *combo = NULL;
    for (size_t i = 0; i < sizeof(IGR) / sizeof(IGR[0]); i++)
        if (!strcmp(action, IGR[i].name)) { combo = IGR[i].combo; break; }
    if (!combo) return false;
    // combo points into the static IGR table, so it stays valid for the task.
    return xTaskCreate(igr_task, "igr", 2560, (void *)combo, tskIDLE_PRIORITY + 3, NULL) == pdPASS;
}
