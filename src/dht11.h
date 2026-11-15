#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

typedef struct {
    int temperature_c;   /* whole degrees Celsius (DHT11 resolution) */
    int humidity_pct;    /* whole percent relative humidity */
} dht11_reading_t;

/* Prepare the data pin. Call once before dht11_read(). */
esp_err_t dht11_init(gpio_num_t pin);

/* Read the sensor once. Do not call more often than once per second.
 * Returns:
 *   ESP_OK                    valid reading stored in *out
 *   ESP_ERR_TIMEOUT           sensor did not answer (wiring/power problem)
 *   ESP_ERR_INVALID_CRC       data received but checksum failed
 *   ESP_ERR_INVALID_RESPONSE  checksum OK but value outside plausible range
 *   ESP_ERR_INVALID_STATE     dht11_init() was not called
 *   ESP_ERR_INVALID_ARG       out is NULL
 */
esp_err_t dht11_read(dht11_reading_t *out);