/* Wi-Fi behaviour (spec R6):
 *  - STA credentials stored in NVS namespace "wifi" (keys "ssid", "pass"); "ap_pass" is the
 *    generated AP password and survives a credential reset.
 *  - Credentials present: start STA and connect, wait up to 15 s.
 *      connected      -> stay STA only; if the link drops, reconnect in the background with
 *                        backoff (2 s doubling to 30 s).
 *      not connected  -> WIFI_MODE_APSTA: the AP "AERIS-MVP-XXXX" (XXXX = last two bytes of the AP
 *                        MAC) comes up so the setup page stays reachable, STA keeps retrying.
 *  - No credentials: WIFI_MODE_APSTA with the STA idle (never connects), so the setup page can
 *    scan: the driver cannot scan in WIFI_MODE_AP. Staying in APSTA (instead of switching around
 *    each scan) avoids a mode change under connected clients and needs no restore logic.
 *  - wifi_mgr_scan() is skipped while a STA connect attempt is in flight (the scan would abort
 *    it); the retry timer in turn waits while a scan runs. Both are serialised by s_scan_lock.
 *  - The AP is WPA2-PSK, channel 1, max 4 clients, password printed on the console at every boot.
 *  - Wi-Fi driver storage is RAM, so credentials exist only in our NVS keys. The STA password is
 *    never logged. */
#include "wifi_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"

#include "wifi_form.h"
#include "wifi_scan.h"
#include "sntp_sync.h"

#define NVS_NS "wifi"
#define KEY_SSID "ssid"
#define KEY_PASS "pass"
#define KEY_AP_PASS "ap_pass"

#define STA_TIMEOUT_MS 15000
#define BACKOFF_MIN_MS 2000
#define BACKOFF_MAX_MS 30000

#define BOOT_GPIO GPIO_NUM_0
#define BOOT_POLL_MS 50
#define BOOT_HOLD_MS 5000

#define GOT_IP_BIT BIT0

static const char *TAG = "wifi";

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static EventGroupHandle_t s_events;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static volatile bool s_sta_wanted;
static volatile bool s_sta_connecting; /* esp_wifi_connect() issued, no CONNECTED/DISCONNECTED yet */
static SemaphoreHandle_t s_scan_lock;  /* held during a scan and while issuing a connect */

#define SCAN_ACTIVE_MS 120 /* per channel; 13 channels ~ 1.6 s plus the channel switches */
#define RETRY_DEFER_MS 1000

esp_netif_t *wifi_mgr_ap_netif(void) { return s_ap_netif; }

static uint32_t hw_random(void) { return esp_random(); }

static esp_err_t load_str(const char *key, char *buf, size_t cap)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = cap;
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return err;
}

static esp_err_t store_strs(const char *k1, const char *v1, const char *k2, const char *v2)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, k1, v1);
    if (err == ESP_OK && k2 != NULL) err = nvs_set_str(h, k2, v2);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *pass)
{
    return store_strs(KEY_SSID, ssid, KEY_PASS, pass);
}

static void erase_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_erase_key(h, KEY_SSID); /* ESP_ERR_NVS_NOT_FOUND is fine */
    (void)nvs_erase_key(h, KEY_PASS);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void load_or_create_ap_pass(char *out /* WF_AP_PASS_LEN + 1 */)
{
    if (load_str(KEY_AP_PASS, out, WF_AP_PASS_LEN + 1) == ESP_OK &&
        strlen(out) == WF_AP_PASS_LEN) {
        return;
    }
    wf_gen_ap_pass(hw_random, out);
    if (store_strs(KEY_AP_PASS, out, NULL, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "cannot store the AP password; it changes at the next boot");
    }
}

static void retry_cb(void *arg)
{
    (void)arg;
    if (!s_sta_wanted) return;
    if (xSemaphoreTake(s_scan_lock, 0) != pdTRUE) { /* a scan is running: try again shortly */
        esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_DEFER_MS * 1000u);
        return;
    }
    s_sta_connecting = true;
    if (esp_wifi_connect() != ESP_OK) s_sta_connecting = false;
    xSemaphoreGive(s_scan_lock);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_wanted) {
            s_sta_connecting = true;
            if (esp_wifi_connect() != ESP_OK) s_sta_connecting = false;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_sta_connecting = false;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connecting = false;
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        if (s_sta_wanted) {
            esp_timer_stop(s_retry_timer); /* error if not running: ignored */
            esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000u);
            s_backoff_ms = s_backoff_ms * 2u > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2u;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_backoff_ms = BACKOFF_MIN_MS;
        xEventGroupSetBits(s_events, GOT_IP_BIT);
        printf("STA IP: " IPSTR "\n", IP2STR(&ev->ip_info.ip));
        sntp_sync_start(); /* once; later calls are no-ops */
    }
}

static void fill_ap_config(wifi_config_t *cfg, const char *ap_pass, char *ssid_out, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid_out, cap, "AERIS-MVP-%02X%02X", mac[4], mac[5]);
    memset(cfg, 0, sizeof *cfg);
    size_t n = strlen(ssid_out);
    memcpy(cfg->ap.ssid, ssid_out, n);
    cfg->ap.ssid_len = (uint8_t)n;
    memcpy(cfg->ap.password, ap_pass, strlen(ap_pass));
    cfg->ap.channel = 1;
    cfg->ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg->ap.max_connection = 4;
}

static void fill_sta_config(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    memset(cfg, 0, sizeof *cfg);
    memcpy(cfg->sta.ssid, ssid, strnlen(ssid, sizeof cfg->sta.ssid));
    memcpy(cfg->sta.password, pass, strnlen(pass, sizeof cfg->sta.password));
    cfg->sta.threshold.authmode = pass[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    cfg->sta.pmf_cfg.capable = true;
    cfg->sta.pmf_cfg.required = false;
}

esp_err_t wifi_mgr_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_events = xEventGroupCreate();
    s_scan_lock = xSemaphoreCreateMutex();
    if (s_events == NULL || s_scan_lock == NULL) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "sta_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    char ap_pass[WF_AP_PASS_LEN + 1];
    load_or_create_ap_pass(ap_pass);
    char ap_ssid[33];
    wifi_config_t ap_cfg;
    fill_ap_config(&ap_cfg, ap_pass, ap_ssid, sizeof ap_ssid);

    char ssid[WF_SSID_MAX + 1] = "", pass[WF_PASS_MAX + 1] = "";
    bool have_creds = load_str(KEY_SSID, ssid, sizeof ssid) == ESP_OK && ssid[0] != '\0' &&
                      load_str(KEY_PASS, pass, sizeof pass) == ESP_OK;

    bool sta_ok = false;
    if (have_creds) {
        wifi_config_t sta_cfg;
        fill_sta_config(&sta_cfg, ssid, pass);
        s_sta_wanted = true;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start()); /* STA_START event triggers esp_wifi_connect() */
        EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(STA_TIMEOUT_MS));
        sta_ok = (bits & GOT_IP_BIT) != 0;
        if (!sta_ok) {
            ESP_LOGW(TAG, "STA did not connect in %d s, starting the AP (STA keeps retrying)",
                     STA_TIMEOUT_MS / 1000);
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        }
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); /* STA stays idle; needed for scans */
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    memset(pass, 0, sizeof pass);

    esp_netif_ip_info_t ap_ip = {0};
    esp_netif_get_ip_info(s_ap_netif, &ap_ip);
    esp_netif_ip_info_t sta_ip = {0};
    if (sta_ok) esp_netif_get_ip_info(s_sta_netif, &sta_ip);
    printf("AP password: %s  AP SSID: %s  AP IP: " IPSTR " (%s)  STA IP: " IPSTR "\n", ap_pass, ap_ssid,
           IP2STR(&ap_ip.ip), sta_ok ? "AP off" : "AP on", IP2STR(&sta_ip.ip));
    memset(ap_pass, 0, sizeof ap_pass);
    memset(&ap_cfg, 0, sizeof ap_cfg);

    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set("aeris-mvp");
        mdns_instance_name_set("AERIS-10 Lite MVP");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_scan(wsc_rec *out, size_t max, size_t *count)
{
    *count = 0;
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_APSTA) return ESP_ERR_WIFI_MODE;
    if (xSemaphoreTake(s_scan_lock, 0) != pdTRUE) return ESP_ERR_WIFI_STATE; /* scan already running */
    esp_err_t err = ESP_ERR_WIFI_STATE;
    wifi_ap_record_t *aps = NULL;
    uint16_t num = 0, want = 0;
    size_t n = 0;
    if (s_sta_connecting) goto done; /* do not abort a connect attempt */

    const wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = SCAN_ACTIVE_MS, .max = SCAN_ACTIVE_MS},
    };
    err = esp_wifi_scan_start(&cfg, true); /* blocking; bounded by 13 channels * SCAN_ACTIVE_MS */
    if (err != ESP_OK) goto done;
    err = esp_wifi_scan_get_ap_num(&num);
    if (err != ESP_OK) goto done;
    if (num == 0) goto done;
    want = num < WSC_RAW_MAX ? num : WSC_RAW_MAX;
    aps = malloc(sizeof *aps * want);
    if (aps == NULL) {
        esp_wifi_clear_ap_list();
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    err = esp_wifi_scan_get_ap_records(&want, aps); /* frees the driver's list, even beyond want */
    if (err != ESP_OK) goto done;
    n = want < max ? want : max;
    for (size_t i = 0; i < n; i++) {
        size_t l = strnlen((const char *)aps[i].ssid, sizeof aps[i].ssid);
        memset(&out[i], 0, sizeof out[i]);
        memcpy(out[i].ssid, aps[i].ssid, l);
        out[i].ssid_len = (uint8_t)l;
        out[i].rssi = aps[i].rssi;
        out[i].channel = aps[i].primary;
        out[i].secure = aps[i].authmode != WIFI_AUTH_OPEN;
    }
    *count = n;
done:
    free(aps);
    xSemaphoreGive(s_scan_lock);
    return err;
}

static void boot_monitor_task(void *arg)
{
    (void)arg;
    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
        if (gpio_get_level(BOOT_GPIO) == 0) {
            held_ms += BOOT_POLL_MS;
            if (held_ms >= BOOT_HOLD_MS) {
                erase_credentials();
                ESP_LOGW(TAG, "Wi-Fi credentials erased");
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
    }
}

esp_err_t wifi_mgr_boot_monitor_start(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    return xTaskCreate(boot_monitor_task, "boot_btn", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK
                                                                                      : ESP_ERR_NO_MEM;
}
