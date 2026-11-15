#include "dht11.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dht11";

static gpio_num_t dht_pin = GPIO_NUM_NC;
static portMUX_TYPE dht_mux = portMUX_INITIALIZER_UNLOCKED;

/* Time (us) the line stays at 'level'. Returns -1 if it takes longer
 * than timeout_us. */
static int measure_level(int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();

    while (gpio_get_level(dht_pin) == level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return -1;
        }
    }

    return (int)(esp_timer_get_time() - start);
}

esp_err_t dht11_init(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,   /* open-drain: 1 = release line */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);

    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(pin, 1);
    dht_pin = pin;
    return ESP_OK;
}

esp_err_t dht11_read(dht11_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dht_pin == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[5] = {0};
    esp_err_t result = ESP_OK;

    /* Start signal: pull the line low for at least 18 ms. */
    gpio_set_level(dht_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(25));

    /* The timing below is in microseconds, so keep interrupts off. */
    portENTER_CRITICAL(&dht_mux);

    gpio_set_level(dht_pin, 1);   /* release the line */

    /* Sensor answers: line goes low (~80 us), then high (~80 us). */
    if (measure_level(1, 100) < 0 ||
        measure_level(0, 120) < 0 ||
        measure_level(1, 120) < 0) {
        result = ESP_ERR_TIMEOUT;
    }

    /* 40 data bits: ~50 us low, then high for ~27 us (0) or ~70 us (1). */
    for (int i = 0; i < 40 && result == ESP_OK; i++) {
        int low_us = measure_level(0, 100);
        int high_us = measure_level(1, 120);

        if (low_us < 0 || high_us < 0) {
            result = ESP_ERR_TIMEOUT;
            break;
        }

        data[i / 8] <<= 1;

        if (high_us > 40) {
            data[i / 8] |= 1;
        }
    }

    portEXIT_CRITICAL(&dht_mux);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "No valid response from sensor");
        return result;
    }

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);

    if (checksum != data[4]) {
        ESP_LOGW(TAG, "Checksum mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    int humidity = data[0];
    int temperature = data[2];

    /* DHT11 range: 20-90 %RH, 0-50 C. Allow a little margin. */
    if (humidity > 100 || temperature > 60) {
        ESP_LOGW(TAG, "Reading out of plausible range");
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->humidity_pct = humidity;
    out->temperature_c = temperature;
    return ESP_OK;
}