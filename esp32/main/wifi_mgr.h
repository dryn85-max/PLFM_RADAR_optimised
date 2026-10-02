/* Wi-Fi manager: STA from NVS credentials, AP fallback, mDNS, BOOT-button credential reset. */
#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include "esp_err.h"
#include "esp_netif.h"

/* Needs nvs_flash_init() done. Blocks up to 15 s while the first STA attempt runs. */
esp_err_t wifi_mgr_start(void);

/* Store STA credentials in NVS (used by the setup page); takes effect after a reboot. */
esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *pass);

/* AP netif (always created after wifi_mgr_start; its IP is valid only while the AP runs). */
esp_netif_t *wifi_mgr_ap_netif(void);

/* Start the GPIO0 (BOOT) monitor task: held >= 5 s erases STA credentials and restarts. */
esp_err_t wifi_mgr_boot_monitor_start(void);

#endif
