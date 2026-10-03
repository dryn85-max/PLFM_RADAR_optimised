/* NMEA/UBX byte router: see gps_rx.h. */
#include "gps_rx.h"

void gps_rx_init(gps_rx_t *rx)
{
    nmea_init(&rx->nmea);
    ubx_init(&rx->ubx);
}

void gps_rx_feed(gps_rx_t *rx, const uint8_t *data, size_t len, nmea_cb_t nmea_cb, void *nmea_ctx,
                 ubx_cb_t ubx_cb, void *ubx_ctx)
{
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i];
        if (!ubx_idle(&rx->ubx)) {
            if (ubx_wait_sync2(&rx->ubx) && c != UBX_SYNC2 && c != UBX_SYNC1) {
                ubx_abort(&rx->ubx); /* lone 0xB5 dropped; re-examine c from idle */
                continue;
            }
            ubx_feed(&rx->ubx, &c, 1, ubx_cb, ubx_ctx);
            i++;
            continue;
        }
        if (c == UBX_SYNC1 && !nmea_in_line(&rx->nmea)) {
            ubx_feed(&rx->ubx, &c, 1, ubx_cb, ubx_ctx);
            i++;
            continue;
        }
        /* NMEA run: up to (not including) the next 0xB5 that falls outside a line.
         * Line tracking mirrors nmea_feed: '$' enters a line, CR/LF leaves it. */
        bool in_line = nmea_in_line(&rx->nmea);
        size_t j = i;
        while (j < len) {
            uint8_t d = data[j];
            if (d == '$') {
                in_line = true;
            } else if (d == '\r' || d == '\n') {
                in_line = false;
            } else if (d == UBX_SYNC1 && !in_line) {
                break;
            }
            j++;
        }
        nmea_feed(&rx->nmea, data + i, j - i, nmea_cb, nmea_ctx);
        i = j;
    }
}
