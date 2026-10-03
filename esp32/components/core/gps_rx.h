/* Byte router for the GPS UART, which carries NMEA text and UBX binary
 * (plain C11, no ESP-IDF).
 *
 * Rule: when the NMEA parser is not inside a line and the UBX parser is idle, a
 * 0xB5 starts UBX framing; if the next byte is not 0x62 the 0xB5 is dropped and
 * that byte is re-examined from idle. While a UBX frame is in progress all bytes go
 * to the UBX parser (a '$' or 0xB5 inside a payload never reaches NMEA). Everything
 * else goes to nmea_feed, in contiguous runs. A 0xB5 inside an NMEA line is an
 * ordinary bad NMEA character (non-printable). Chunk boundaries do not matter. */
#ifndef GPS_RX_H
#define GPS_RX_H

#include "nmea.h"
#include "ubx.h"

typedef struct {
    nmea_t nmea;
    ubx_t ubx;
} gps_rx_t;

void gps_rx_init(gps_rx_t *rx);
void gps_rx_feed(gps_rx_t *rx, const uint8_t *data, size_t len, nmea_cb_t nmea_cb, void *nmea_ctx,
                 ubx_cb_t ubx_cb, void *ubx_ctx);

#endif
