
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "plant";

#define POT_ADC_CHANNEL ADC_CHANNEL_3
#define SENSOR_INTERVAL_MS 1000

// Structure for sensor readings
typedef struct {
    int soil_moisture;
} sensor_data_t;

// Shared ADC handle and queue
static adc_oneshot_unit_handle_t adc_handle;
static QueueHandle_t sensor_queue;

// Sensor Task
static void sensor_task(void *pvParameters)
{
    TickType_t last_wake_time = xTaskGetTickCount();
    sensor_data_t sensor_data;

    ESP_LOGI(TAG, "Sensor task started");

    while (1) {
        ESP_ERROR_CHECK(adc_oneshot_read(
            adc_handle,
            POT_ADC_CHANNEL,
            &sensor_data.soil_moisture
        ));

        xQueueSend(sensor_queue, &sensor_data, portMAX_DELAY);

        vTaskDelayUntil(
            &last_wake_time,
            pdMS_TO_TICKS(SENSOR_INTERVAL_MS)
        );
    }
}

// Classify soil moisture using temporary ADC thresholds
static const char *get_moisture_status(int adc_value)
{
    if (adc_value > 3000) {
        return "DRY";
    } else if (adc_value >= 1500) {
        return "NORMAL";
    } else {
        return "WET";
    }
}

// Monitoring Task
static void monitoring_task(void *pvParameters)
{
    sensor_data_t received_data;

    ESP_LOGI(TAG, "Monitoring task started");

    while (1) {
        if (xQueueReceive(
            sensor_queue,
            &received_data,
            portMAX_DELAY
        ) == pdTRUE) {

        ESP_LOGI(
            TAG,
            "Soil moisture ADC: %d | Status: %s",
            received_data.soil_moisture,
            get_moisture_status(received_data.soil_moisture)
          );
        }
    }
}

void app_main(void)
{
    // Initialize ADC
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };

    ESP_ERROR_CHECK(
        adc_oneshot_new_unit(&init_config, &adc_handle)
    );

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(
            adc_handle,
            POT_ADC_CHANNEL,
            &channel_config
        )
    );

    // Create sensor queue
    sensor_queue = xQueueCreate(5, sizeof(sensor_data_t));

    if (sensor_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor queue");
        return;
    }

    // Create FreeRTOS tasks
    xTaskCreate(
        sensor_task,
        "sensor_task",
        4096,
        NULL,
        5,
        NULL
    );

    xTaskCreate(
        monitoring_task,
        "monitoring_task",
        4096,
        NULL,
        5,
        NULL
    );

    ESP_LOGI(TAG, "Smart Plant Monitoring System started");
}
