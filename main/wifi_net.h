// Talon — WiFi manager: STA with NVS-stored credentials, SoftAP provisioning
// with captive portal, WPS push-button join, and mDNS (talon.local).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TALON_HOSTNAME  "talon"          // mDNS + DHCP name -> http://talon.local/
#define TALON_AP_SSID   "Talon-Setup"    // open provisioning AP

typedef enum {
    TALON_NET_STA,        // joined (or joining) the stored network
    TALON_NET_AP_SETUP,   // SoftAP provisioning portal up
    TALON_NET_WPS,        // WPS push-button negotiation running
} talon_net_mode_t;

typedef enum {
    TALON_WPS_IDLE,
    TALON_WPS_CONNECTING,
    TALON_WPS_CONNECTED,
    TALON_WPS_FAILED,
} talon_wps_state_t;

// Bring the radio up: STA when credentials are stored (NVS, seeded once from
// wifi_creds.h if present), otherwise straight into SoftAP provisioning.
void wifi_net_start(void);

// True once an IP is held on the STA interface; ip/rssi valid only then.
bool wifi_net_up(char ip_out[16], int *rssi_out);

talon_net_mode_t  wifi_net_mode(void);
talon_wps_state_t wifi_net_wps_state(void);
int  wifi_net_wps_remaining(void);              // seconds left, 0 when not running
bool wifi_net_has_creds(void);
const char *wifi_net_ssid(void);                // stored SSID ("" when none)

// Save credentials and (after a short delay, so the HTTP reply gets out first)
// drop the setup AP and join that network.
void wifi_net_save_creds(const char *ssid, const char *pass);

// Erase stored credentials and enter SoftAP provisioning.
void wifi_net_forget(void);

// Enter SoftAP provisioning (keeps stored credentials).
void wifi_net_enter_setup(void);

// Start WPS push-button negotiation (tears down the setup AP if active).
void wifi_net_start_wps(void);

// Blocking scan; writes a JSON array [{"ssid":...,"rssi":...,"open":0|1},...].
// Returns the number of networks written (0 on failure/empty).
int wifi_net_scan_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
