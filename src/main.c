#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "plant";

#define POT_ADC_CHANNEL ADC_CHANNEL_3  // GPIO 4 on ESP32-S3

void app_main(void)
{
    adc_oneshot_unit_handle_t adc_handle;

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(
        adc_handle, POT_ADC_CHANNEL, &channel_config));

    ESP_LOGI(TAG, "Potentiometer test started");

    while (1) {
        int adc_value = 0;

        ESP_ERROR_CHECK(adc_oneshot_read(
            adc_handle, POT_ADC_CHANNEL, &adc_value));

        ESP_LOGI(TAG, "Potentiometer ADC value: %d", adc_value);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}