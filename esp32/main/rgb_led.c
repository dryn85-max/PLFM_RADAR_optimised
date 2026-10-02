/* WS2812 driver on the RMT TX peripheral: one bytes encoder, 24 bits per write, GRB order,
 * MSB first. RMT resolution 10 MHz (tick = 0.1 us).
 * Timing VERIFY: the WS2812 variant on the board is not documented (the DevKitC-1 document
 * only says "Addressable RGB LED"); the usual WS2812B values are used: bit 0 = 0.3 us high +
 * 0.9 us low, bit 1 = 0.9 us high + 0.3 us low, tolerance about +-0.15 us.
 * Reset (latch): the line must stay low for >= 50 us after the last bit. No reset symbol is
 * appended: the RMT leaves the line low after a transmission (eot_level 0) and the status LED
 * task writes at most every 20 ms, far above 50 us. */
#include "rgb_led.h"

#include <stdbool.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#define RMT_RES_HZ 10000000u
#define T_TICKS(ns) ((ns) * (RMT_RES_HZ / 1000000u) / 1000u) /* ns -> RMT ticks */
#define WRITE_TIMEOUT_MS 50

static const char *TAG = "rgb_led";

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static uint8_t s_grb[3]; /* payload must stay valid until the transmission is done */
static bool s_warned;

esp_err_t rgb_led_init(void)
{
    if (s_chan != NULL) return ESP_OK;
    const rmt_tx_channel_config_t cfg = {
        .gpio_num = RGB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    rmt_channel_handle_t chan = NULL;
    esp_err_t err = rmt_new_tx_channel(&cfg, &chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }
    const rmt_bytes_encoder_config_t ecfg = {
        .bit0 = {.level0 = 1, .duration0 = T_TICKS(300), .level1 = 0, .duration1 = T_TICKS(900)},
        .bit1 = {.level0 = 1, .duration0 = T_TICKS(900), .level1 = 0, .duration1 = T_TICKS(300)},
        .flags.msb_first = 1,
    };
    rmt_encoder_handle_t enc = NULL;
    err = rmt_new_bytes_encoder(&ecfg, &enc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_bytes_encoder failed: %s", esp_err_to_name(err));
        rmt_del_channel(chan);
        return err;
    }
    err = rmt_enable(chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rmt_del_encoder(enc);
        rmt_del_channel(chan);
        return err;
    }
    s_chan = chan;
    s_enc = enc;
    return ESP_OK;
}

static uint8_t scale(uint8_t v) { return (uint8_t)(((unsigned)v * RGB_LED_BRIGHTNESS_PCT) / 100u); }

esp_err_t rgb_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_chan == NULL) return ESP_ERR_INVALID_STATE;
    s_grb[0] = scale(g);
    s_grb[1] = scale(r);
    s_grb[2] = scale(b);
    const rmt_transmit_config_t tx = {.loop_count = 0};
    esp_err_t err = rmt_transmit(s_chan, s_enc, s_grb, sizeof s_grb, &tx);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(s_chan, WRITE_TIMEOUT_MS);
    if (err != ESP_OK && !s_warned) { /* once: the caller may retry every tick */
        s_warned = true;
        ESP_LOGW(TAG, "LED write failed: %s", esp_err_to_name(err));
    }
    return err;
}
