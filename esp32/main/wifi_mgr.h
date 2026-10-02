/* Wi-Fi manager: STA from NVS credentials, AP fallback, mDNS, BOOT-button credential reset. */
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

/* Start the GPIO0 (BOOT) monitor task: held >= 5 s erases STA credentials and restarts. */
esp_err_t wifi_mgr_boot_monitor_start(void);

#endif
