// Talon — WiFi station bring-up.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Join the network from wifi_creds.h. Non-blocking; reconnects on drop.
void wifi_net_start(void);

// True once an IP is held; ip/rssi are valid only when it returns true.
bool wifi_net_up(char ip_out[16], int *rssi_out);

#ifdef __cplusplus
}
#endif
