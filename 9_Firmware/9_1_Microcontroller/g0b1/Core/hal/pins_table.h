/* Pure-data pin table: gpio_t -> {port index, pin number, direction, idle level}.
 * No MCU headers, so the host tests can check the pin plan. hal_gpio.c turns
 * port/pin into GPIOx/GPIO_PIN_n on the target. Pins come from pins.h. */
#ifndef PINS_TABLE_H
#define PINS_TABLE_H
#include <stdint.h>
#include "hal_gpio.h"

enum { PIN_PORT_A = 0, PIN_PORT_B = 1, PIN_PORT_C = 2 };

typedef struct {
    uint8_t port;        /* PIN_PORT_A/B/C */
    uint8_t pin;         /* 0..15 */
    uint8_t is_output;
    uint8_t idle_high;   /* initial output level */
} pin_desc_t;

/* Indexed by gpio_t, PIN_COUNT entries. */
extern const pin_desc_t k_pin_table[PIN_COUNT];

#endif
