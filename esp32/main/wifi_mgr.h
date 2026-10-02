/* Wi-Fi manager: STA from NVS credentials, AP fallback, AP on demand, mDNS, BOOT-button actions. */
#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include "esp_err.h"
#include "esp_netif.h"
#include "wifi_scan.h"

/* Needs nvs_flash_init() done. Blocks up to 15 s while the first STA attempt runs. */
esp_err_t wifi_mgr_start(void);

/* Store STA credentials in NVS (used by the setup page); takes effect after a reboot. */
esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *pass);

/* AP netif (always created after wifi_mgr_start; its IP is valid only while the AP runs). */
esp_netif_t *wifi_mgr_ap_netif(void);

/* Blocking scan (about 2 s) for the setup page; fills at most max raw records (unsorted).
 * Briefly disrupts the AP. ESP_OK with *count possibly 0; ESP_ERR_WIFI_STATE when a STA connect
 * attempt or another scan is in progress, ESP_ERR_WIFI_MODE when not in AP+STA, or the driver's
 * error. *count is 0 on any error. */
esp_err_t wifi_mgr_scan(wsc_rec *out, size_t max, size_t *count);

/* Bring up the AP next to the STA (STA mode only) and restart its 10 min idle timer; if an AP
 * already runs, only the idle timer of an on-demand AP is restarted. Waits up to ~3 s for a scan or
 * connect attempt to end: ESP_ERR_TIMEOUT then. ESP_ERR_INVALID_STATE when Wi-Fi is not in STA/APSTA.
 * Prints the AP password line on the console. */
esp_err_t wifi_mgr_ap_on_demand(void);

/* Start the GPIO0 (BOOT) monitor task; action on release: held 2-5 s = AP on demand, 5-10 s =
 * erase STA credentials and restart, otherwise nothing. Feeds the status LED. */
esp_err_t wifi_mgr_boot_monitor_start(void);

#endif
