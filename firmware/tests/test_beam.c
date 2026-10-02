#define _POSIX_C_SOURCE 200809L
/* Host tests for Core/app/beam.c (integer beam tables, Decision 3). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "mock_log.h"
#include "adar1000.h"
#include "beam.h"
#include "sin_lut.h"
#include "config.h"

/* Float reference (ref_beam_table.py), n = 0..15. */
typedef struct { int el; uint8_t idx[16]; } ref_row_t;
static const ref_row_t REF[] = {
    { -90, {0, 64, 0, 64, 0, 64, 0, 64, 0, 64, 0, 64, 0, 64, 0, 64} },
    { -60, {0, 73, 17, 90, 34, 107, 51, 124, 69, 13, 86, 30, 103, 47, 120, 65} },
    { -45, {0, 83, 37, 120, 75, 30, 112, 67, 22, 105, 59, 14, 97, 52, 6, 89} },
    { -20, {0, 106, 84, 62, 40, 19, 125, 103, 81, 59, 37, 15, 121, 99, 78, 56} },
    { 0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    { 10, {0, 11, 22, 33, 44, 56, 67, 78, 89, 100, 111, 122, 5, 16, 28, 39} },
    { 30, {0, 32, 64, 96, 0, 32, 64, 96, 0, 32, 64, 96, 0, 32, 64, 96} },
    { 60, {0, 55, 111, 38, 94, 21, 77, 4, 59, 115, 42, 98, 25, 81, 8, 63} },
    { 90, {0, 64, 0, 64, 0, 64, 0, 64, 0, 64, 0, 64, 0, 64, 0, 64} },
};
#define NREF (int)(sizeof REF / sizeof REF[0])

static int spi_count(void)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) {
        if (mock_log[i].kind == MOCK_EV_SPI) n++;
    }
    return n;
}

static const mock_event_t *spi_ev(int k)
{
    int i;
    for (i = 0; i < mock_log_n; i++) {
        if (mock_log[i].kind == MOCK_EV_SPI && k-- == 0) return &mock_log[i];
    }
    return NULL;
}

static void test_plan_table_n0_3(void)
{
    /* The table from the plan (Task 6), n = 0..3, hard-coded independently. */
    static const struct { int el; uint8_t idx[4]; } T[] = {
        { -45, {0, 83, 37, 120} }, { -20, {0, 106, 84, 62} }, { 0, {0, 0, 0, 0} },
        { 10, {0, 11, 22, 33} },   { 30, {0, 32, 64, 96} },   { 60, {0, 55, 111, 38} },
        { 90, {0, 64, 0, 64} },    { -90, {0, 64, 0, 64} },
    };
    uint8_t idx[ADAR_COUNT * 4];
    int i, n;
    for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++) {
        memset(idx, 0xEE, sizeof idx);
        TT_ASSERT_EQ(0, beam_phase_indices(T[i].el, idx));
        for (n = 0; n < 4; n++) {
            TT_ASSERT_EQ(T[i].idx[n], idx[n]);
        }
    }
}

static void test_all_elements_vs_float(void)
{
    uint8_t idx[ADAR_COUNT * 4];
    int i, n;
    for (i = 0; i < NREF; i++) {
        TT_ASSERT_EQ(0, beam_phase_indices(REF[i].el, idx));
        for (n = 0; n < ADAR_COUNT * 4; n++) {
            if (idx[n] != REF[i].idx[n]) printf("  el=%d n=%d\n", REF[i].el, n);
            TT_ASSERT_EQ(REF[i].idx[n], idx[n]);
        }
    }
}

/* Run the Python reference and compare all 16 elements for the 5 spec angles. */
static void test_vs_python_reference(void)
{
    static const int ang[5] = { -45, -20, 10, 30, 60 };
    FILE *p;
    int a, n, el, v, ok = 0;
    uint8_t idx[ADAR_COUNT * 4];
    if (system("python3 --version >/dev/null 2>&1") != 0) {
        printf("  SKIP test_vs_python_reference: python3 not found\n");
        return;
    }
    p = popen("python3 ref_beam_table.py -45 -20 10 30 60", "r");
    TT_ASSERT(p != NULL);
    if (p == NULL) return;
    for (a = 0; a < 5; a++) {
        if (fscanf(p, "%d", &el) != 1) break;
        TT_ASSERT_EQ(ang[a], el);
        TT_ASSERT_EQ(0, beam_phase_indices(el, idx));
        for (n = 0; n < 16; n++) {
            if (fscanf(p, "%d", &v) != 1) { v = -1; }
            if (n < ADAR_COUNT * 4) TT_ASSERT_EQ(v, idx[n]);
            else TT_ASSERT(v >= 0 && v < 128);
        }
        ok++;
    }
    pclose(p);
    TT_ASSERT_EQ(5, ok);
}

/* sin_lut.h must equal the generator output (CI has python3). */
static void test_sin_lut_matches_generator(void)
{
    FILE *p, *f;
    int c1, c2, same = 1;
    if (system("python3 --version >/dev/null 2>&1") != 0) {
        printf("  SKIP test_sin_lut_matches_generator: python3 not found\n");
        return;
    }
    p = popen("python3 gen_sin_lut.py", "r");
    f = fopen("../Core/app/sin_lut.h", "r");
    TT_ASSERT(p != NULL && f != NULL);
    if (p == NULL || f == NULL) { if (p) pclose(p); if (f) fclose(f); return; }
    do {
        c1 = fgetc(p);
        c2 = fgetc(f);
        if (c1 != c2) { same = 0; break; }
    } while (c1 != EOF && c2 != EOF);
    pclose(p);
    fclose(f);
    TT_ASSERT(same);
}

static void test_lut_properties(void)
{
    int d;
    TT_ASSERT_EQ(0, SIN_Q15[0]);
    TT_ASSERT_EQ(16384, SIN_Q15[30]);
    TT_ASSERT_EQ(32767, SIN_Q15[90]);   /* 32768 clamped */
    for (d = 1; d <= 90; d++) {
        TT_ASSERT(SIN_Q15[d] > SIN_Q15[d - 1]);
    }
}

static void test_range(void)
{
    uint8_t idx[ADAR_COUNT * 4];
    mock_reset();
    TT_ASSERT_EQ(-EINVAL, beam_phase_indices(91, idx));
    TT_ASSERT_EQ(-EINVAL, beam_phase_indices(-91, idx));
    TT_ASSERT_EQ(-EINVAL, beam_phase_indices(32767, idx));
    TT_ASSERT_EQ(-EINVAL, beam_phase_indices(-32768, idx));
    TT_ASSERT_EQ(0, beam_phase_indices(90, idx));
    TT_ASSERT_EQ(0, beam_phase_indices(-90, idx));
    TT_ASSERT_EQ(-EINVAL, beam_apply(91));
    TT_ASSERT_EQ(-EINVAL, beam_apply(-91));
    TT_ASSERT_EQ(0, spi_count());     /* rejected angle -> no SPI traffic */
}

static void test_symmetry(void)
{
    /* idx(-el, n) == (128 - idx(el, n)) mod 128 except at exact half-LSB ties */
    uint8_t a[ADAR_COUNT * 4], b[ADAR_COUNT * 4];
    int el, n;
    for (el = 0; el <= 90; el++) {
        TT_ASSERT_EQ(0, beam_phase_indices(el, a));
        TT_ASSERT_EQ(0, beam_phase_indices(-el, b));
        TT_ASSERT_EQ(0, a[0]);
        TT_ASSERT_EQ(0, b[0]);
        for (n = 1; n < ADAR_COUNT * 4; n++) {
            int d = ((int)a[n] + (int)b[n]) & 127;
            TT_ASSERT(d == 0 || d == 1 || d == 127);   /* rounding of the sign flip */
        }
    }
}

static void test_apply_writes_rx_and_tx(void)
{
    uint8_t idx[ADAR_COUNT * 4];
    int g, k = 0;
    mock_reset();
    TT_ASSERT_EQ(0, beam_phase_indices(30, idx));
    mock_reset();
    TT_ASSERT_EQ(0, beam_apply(30));
    /* per channel: RX I, Q, load, then TX I, Q, load = 6 writes */
    TT_ASSERT_EQ(ADAR_COUNT * 4 * 6, spi_count());
    for (g = 0; g < ADAR_COUNT * 4; g++) {
        int dev = g / 4, ch = g % 4;
        const mock_event_t *e;
        e = spi_ev(k++);
        TT_ASSERT_EQ(((dev & 3) << 5), e->bytes[0] & 0xE0);
        TT_ASSERT_EQ(REG_CH1_RX_PHS_I + 2 * ch, e->bytes[1]);
        TT_ASSERT_EQ(VM_I[idx[g]], e->bytes[2]);
        e = spi_ev(k++);
        TT_ASSERT_EQ(REG_CH1_RX_PHS_I + 2 * ch + 1, e->bytes[1]);
        TT_ASSERT_EQ(VM_Q[idx[g]], e->bytes[2]);
        e = spi_ev(k++);
        TT_ASSERT_EQ(REG_LOAD_WORKING, e->bytes[1]);
        TT_ASSERT_EQ(LD_WRK_REGS_LDRX_OVERRIDE, e->bytes[2]);
        e = spi_ev(k++);
        TT_ASSERT_EQ(REG_CH1_TX_PHS_I + 2 * ch, e->bytes[1]);
        TT_ASSERT_EQ(VM_I[idx[g]], e->bytes[2]);
        e = spi_ev(k++);
        TT_ASSERT_EQ(REG_CH1_TX_PHS_I + 2 * ch + 1, e->bytes[1]);
        TT_ASSERT_EQ(VM_Q[idx[g]], e->bytes[2]);
        e = spi_ev(k++);
        TT_ASSERT_EQ(REG_LOAD_WORKING, e->bytes[1]);
        TT_ASSERT_EQ(LD_WRK_REGS_LDTX_OVERRIDE, e->bytes[2]);
    }
}

static void test_apply_returns_first_error_and_stops(void)
{
    mock_reset();
    mock_spi_fail_next(-EIO);
    TT_ASSERT_EQ(-EIO, beam_apply(10));
    TT_ASSERT_EQ(1, spi_count());     /* aborted at the first failing write */
}

int main(void)
{
    TT_RUN(test_plan_table_n0_3);
    TT_RUN(test_all_elements_vs_float);
    TT_RUN(test_vs_python_reference);
    TT_RUN(test_sin_lut_matches_generator);
    TT_RUN(test_lut_properties);
    TT_RUN(test_range);
    TT_RUN(test_symmetry);
    TT_RUN(test_apply_writes_rx_and_tx);
    TT_RUN(test_apply_returns_first_error_and_stops);
    return TT_RESULT();
}
