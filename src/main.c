#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "mbedtls/base64.h"

#include "telegram_credentials.h"
#include "gmail_credentials.h"
#include "gmail_root_ca.h"
#include "wifi_credentials.h"
#include "dht11.h"

static const char *TAG = "plant";

#define POT_ADC_CHANNEL ADC_CHANNEL_3
#define SENSOR_INTERVAL_MS 1000

#define SOIL_DRY_BIT BIT0
#define SOIL_NOT_DRY_BIT BIT1
#define WIFI_CONNECTED_BIT BIT2
#define SOIL_DRY_THRESHOLD 3000
#define CONFIRM_READINGS 3
#define ALERT_COOLDOWN_MS 60000    /* TESTING: 60 s. Final value: 900000 (15 min)  */
#define MAX_SEND_ATTEMPTS 3
#define SEND_RETRY_DELAY_MS 10000
#define PLANT_ID "PlantMonitor-01"
#define DHT_GPIO GPIO_NUM_13
#define DHT_READ_EVERY_N_CYCLES 5   /* 5 x 1 s = one DHT11 read every 5 s */

typedef struct {
    int soil_moisture;
    int temperature_c;
    int humidity_pct;
    bool dht_valid;      /* false if the latest DHT11 read failed */
} sensor_data_t;

typedef struct {
    char body[256];
    size_t length;
} telegram_response_t;

static adc_oneshot_unit_handle_t adc_handle;
static QueueHandle_t sensor_queue;
static EventGroupHandle_t plant_event_group;

static int wifi_retry_count = 0;
static bool sntp_started = false;

/* Latest ADC value, written by monitoring_task, read by notification_task. */
static volatile int latest_soil_adc = 0;
static volatile int latest_temperature_c = 0;
static volatile int latest_humidity_pct = 0;
static volatile bool latest_dht_valid = false;

/* Collect Telegram's response body so we can verify its JSON result. */
static esp_err_t telegram_http_event_handler(esp_http_client_event_t *event)
{
    telegram_response_t *response =
        (telegram_response_t *)event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA &&
        response != NULL &&
        event->data != NULL &&
        event->data_len > 0) {

        size_t available = sizeof(response->body) - response->length - 1;
        size_t copy_length = (size_t)event->data_len;

        if (copy_length > available) {
            copy_length = available;
        }

        if (copy_length > 0) {
            memcpy(
                response->body + response->length,
                event->data,
                copy_length
            );

            response->length += copy_length;
            response->body[response->length] = '\0';
        }
    }

    return ESP_OK;
}

/* Send one URL-encoded message to the configured Telegram chat.
 * Returns true only if Telegram confirmed delivery. */
static bool send_telegram_message(const char *message)
{
    char encoded_message[384];
    char post_data[512];
    size_t j = 0;

    if (message == NULL) {
        ESP_LOGE(TAG, "Telegram message is NULL");
        return false;
    }

    /* URL-encode the message for application/x-www-form-urlencoded. */
    for (size_t i = 0; message[i] != '\0'; i++) {
        unsigned char c = (unsigned char)message[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {

            if (j + 1 >= sizeof(encoded_message)) {
                ESP_LOGE(TAG, "Telegram message is too long");
                return false;
            }

            encoded_message[j++] = (char)c;
        } else {
            if (j + 3 >= sizeof(encoded_message)) {
                ESP_LOGE(TAG, "Telegram message is too long");
                return false;
            }

            snprintf(
                &encoded_message[j],
                sizeof(encoded_message) - j,
                "%%%02X",
                c
            );

            j += 3;
        }
    }

    encoded_message[j] = '\0';

    int post_length = snprintf(
        post_data,
        sizeof(post_data),
        "chat_id=%s&text=%s",
        TELEGRAM_CHAT_ID,
        encoded_message
    );

    if (post_length < 0 || post_length >= sizeof(post_data)) {
        ESP_LOGE(TAG, "Telegram request data is too long");
        return false;
    }

    telegram_response_t response = {0};

    esp_http_client_config_t config = {
        .url = "https://api.telegram.org/bot" TELEGRAM_BOT_TOKEN "/sendMessage",
        .event_handler = telegram_http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize Telegram HTTP client");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);

    esp_http_client_set_header(
        client,
        "Content-Type",
        "application/x-www-form-urlencoded"
    );

    esp_http_client_set_post_field(
        client,
        post_data,
        post_length
    );

    ESP_LOGI(TAG, "Sending Telegram notification...");

    bool success = false;
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);

        if (status_code == 200 &&
            strstr(response.body, "\"ok\":true") != NULL) {
            ESP_LOGI(TAG, "Telegram notification sent successfully");
            success = true;
        } else {
            ESP_LOGE(
                TAG,
                "Telegram send failed. HTTP status: %d, response: %s",
                status_code,
                response.body
            );
        }
    } else {
        ESP_LOGE(
            TAG,
            "Telegram request failed: %s",
            esp_err_to_name(err)
        );
    }

    esp_http_client_cleanup(client);
    return success;
}

// ==================== GMAIL SMTP FUNCTIONS ====================

#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465

static int smtp_write_all(esp_tls_t *tls, const char *data)
{
    size_t length = strlen(data);
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = esp_tls_conn_write(
            tls, data + sent, length - sent
        );

        if (result <= 0) {
            ESP_LOGE(TAG, "SMTP write failed");
            return -1;
        }

        sent += result;
    }

    return 0;
}

static int smtp_read_response(esp_tls_t *tls)
{
    char line[256];
    size_t position = 0;
    int response_code = -1;

    while (1) {
        char character;
        ssize_t result = esp_tls_conn_read(tls, &character, 1);

        if (result <= 0) {
            ESP_LOGE(TAG, "SMTP response read failed");
            return -1;
        }

        if (character == '\n') {
            line[position] = '\0';

            ESP_LOGI(TAG, "SMTP: %s", line);

            if (position >= 3) {
                if (response_code == -1) {
                    response_code = atoi(line);
                }

                // A space after the response code means
                // this is the final line of the response.
                if (position < 4 || line[3] == ' ') {
                    return response_code;
                }
            }

            position = 0;
        } else if (character != '\r') {
            if (position < sizeof(line) - 1) {
                line[position++] = character;
            } else {
                ESP_LOGE(TAG, "SMTP response line too long");
                return -1;
            }
        }
    }
}

static int smtp_command(
    esp_tls_t *tls,
    const char *command,
    int expected_code
)
{
    char buffer[512];

    int length = snprintf(
        buffer,
        sizeof(buffer),
        "%s\r\n",
        command
    );

    if (length < 0 || length >= sizeof(buffer)) {
        ESP_LOGE(TAG, "SMTP command too long");
        return -1;
    }

    if (smtp_write_all(tls, buffer) != 0) {
        return -1;
    }

    int response = smtp_read_response(tls);

    if (response != expected_code) {
        ESP_LOGE(
            TAG,
            "SMTP unexpected response: %d (expected %d)",
            response,
            expected_code
        );
        return -1;
    }

    return 0;
}

static int smtp_send_base64(
    esp_tls_t *tls,
    const char *input
)
{
    unsigned char encoded[512];
    size_t encoded_length = 0;

    int result = mbedtls_base64_encode(
        encoded,
        sizeof(encoded) - 1,
        &encoded_length,
        (const unsigned char *)input,
        strlen(input)
    );

    if (result != 0) {
        ESP_LOGE(TAG, "Base64 encoding failed: %d", result);
        return -1;
    }

    encoded[encoded_length] = '\0';

    if (smtp_write_all(tls, (const char *)encoded) != 0) {
        return -1;
    }

    if (smtp_write_all(tls, "\r\n") != 0) {
        return -1;
    }

    return 0;
}

static bool send_gmail_notification(const char *message)
{
    ESP_LOGI(TAG, "Connecting to Gmail SMTP...");

    esp_tls_t *tls = esp_tls_init();

    if (tls == NULL) {
        ESP_LOGE(TAG, "Could not initialize ESP-TLS");
        return false;
    }

     esp_tls_cfg_t config = {
        .cacert_buf = (const unsigned char *)GMAIL_ROOT_CA_PEM,
        .cacert_bytes = sizeof(GMAIL_ROOT_CA_PEM),
        .timeout_ms = 15000
    };

    int connected = esp_tls_conn_new_sync(
        SMTP_HOST,
        strlen(SMTP_HOST),
        SMTP_PORT,
        &config,
        tls
    );

    if (connected != 1) {
        ESP_LOGE(TAG, "Gmail TLS connection failed");
        esp_tls_conn_destroy(tls);
        return false;
    }

    bool success = false;

    // Gmail greeting
    if (smtp_read_response(tls) != 220) {
        ESP_LOGE(TAG, "Gmail greeting failed");
        goto cleanup;
    }

    // Identify the ESP32
    if (smtp_command(tls, "EHLO esp32.local", 250) != 0) {
        goto cleanup;
    }

    // Begin authentication
    if (smtp_command(tls, "AUTH LOGIN", 334) != 0) {
        goto cleanup;
    }

    // Send Gmail address
    if (smtp_send_base64(tls, GMAIL_SENDER_EMAIL) != 0) {
        goto cleanup;
    }

    if (smtp_read_response(tls) != 334) {
        ESP_LOGE(TAG, "SMTP username authentication failed");
        goto cleanup;
    }

    // Send Gmail App Password
    if (smtp_send_base64(tls, GMAIL_APP_PASSWORD) != 0) {
        goto cleanup;
    }

    if (smtp_read_response(tls) != 235) {
        ESP_LOGE(TAG, "SMTP password authentication failed");
        goto cleanup;
    }

    // Sender address
    char command[256];

    snprintf(
        command,
        sizeof(command),
        "MAIL FROM:<%s>",
        GMAIL_SENDER_EMAIL
    );

    if (smtp_command(tls, command, 250) != 0) {
        goto cleanup;
    }

    // Recipient address
    snprintf(
        command,
        sizeof(command),
        "RCPT TO:<%s>",
        GMAIL_RECIPIENT_EMAIL
    );

    if (smtp_command(tls, command, 250) != 0) {
        goto cleanup;
    }

    // Begin email content
    if (smtp_command(tls, "DATA", 354) != 0) {
        goto cleanup;
    }

    char email[1200];

    int email_length = snprintf(
        email,
        sizeof(email),
        "From: Smart Plant Monitor <%s>\r\n"
        "To: <%s>\r\n"
        "Subject: Smart Plant Monitoring Alert\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "\r\n"
        "%s\r\n"
        "\r\n.\r\n",
        GMAIL_SENDER_EMAIL,
        GMAIL_RECIPIENT_EMAIL,
        message
    );

    if (email_length < 0 || email_length >= sizeof(email)) {
        ESP_LOGE(TAG, "Email message is too long");
        goto cleanup;
    }

    if (smtp_write_all(tls, email) != 0) {
        goto cleanup;
    }

    if (smtp_read_response(tls) != 250) {
        ESP_LOGE(TAG, "Gmail rejected the email");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Gmail notification sent successfully");
    success = true;

    // End SMTP session
    smtp_command(tls, "QUIT", 221);

cleanup:
    esp_tls_conn_destroy(tls);
    return success;
}

/* Called by SNTP each time the clock is synchronized. */
static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized with NTP server");
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(TAG, "Wi-Fi started. Connecting...");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        xEventGroupClearBits(
            plant_event_group,
            WIFI_CONNECTED_BIT
        );

        wifi_retry_count++;

        ESP_LOGW(
            TAG,
            "Wi-Fi disconnected. Reconnecting (attempt %d)...",
            wifi_retry_count
        );

        esp_wifi_connect();

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "Connected! IP address: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        wifi_retry_count = 0;

        xEventGroupSetBits(
            plant_event_group,
            WIFI_CONNECTED_BIT
        );

        /* Start time synchronization once, after the first IP address. */
        if (!sntp_started) {
            esp_sntp_config_t sntp_config =
                ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_config.sync_cb = time_sync_cb;

            if (esp_netif_sntp_init(&sntp_config) == ESP_OK) {
                sntp_started = true;
                ESP_LOGI(TAG, "SNTP started");
            } else {
                ESP_LOGW(TAG, "SNTP start failed");
            }
        }
    }
}

static void wifi_init_sta(void)
{
    /* Initialize NVS. */
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    /* Initialize network interface. */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Create default event loop. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create Wi-Fi station interface. */
    esp_netif_create_default_wifi_sta();

    /* Initialize Wi-Fi. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register Wi-Fi event handler. */
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL
    ));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL
    ));

    /* Configure Wi-Fi credentials. */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config
    ));

    /* Start Wi-Fi. */
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialization completed.");
}

/* Sensor Task */
static void sensor_task(void *pvParameters)
{
    TickType_t last_wake_time = xTaskGetTickCount();
    sensor_data_t sensor_data = {0};
    int cycle = 0;

    ESP_LOGI(TAG, "Sensor task started");

    while (1) {
        ESP_ERROR_CHECK(adc_oneshot_read(
            adc_handle,
            POT_ADC_CHANNEL,
            &sensor_data.soil_moisture
        ));

        /* The DHT11 is slow, so read it only every few cycles. */
        if (cycle % DHT_READ_EVERY_N_CYCLES == 0) {
            dht11_reading_t dht;
            esp_err_t err = dht11_read(&dht);

            if (err == ESP_OK) {
                sensor_data.temperature_c = dht.temperature_c;
                sensor_data.humidity_pct = dht.humidity_pct;
                sensor_data.dht_valid = true;
            } else {
                sensor_data.dht_valid = false;
                ESP_LOGW(
                    TAG,
                    "DHT11 read failed: %s",
                    esp_err_to_name(err)
                );
            }
        }

        cycle++;

        xQueueSend(sensor_queue, &sensor_data, portMAX_DELAY);

        vTaskDelayUntil(
            &last_wake_time,
            pdMS_TO_TICKS(SENSOR_INTERVAL_MS)
        );
    }
}

/* Classify soil moisture using temporary ADC thresholds. */
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

/* Monitoring Task */
static void monitoring_task(void *pvParameters)
{
    sensor_data_t received_data;

    bool confirmed_dry = false;   /* last confirmed state */
    int opposite_count = 0;       /* readings in a row that disagree with it */

    ESP_LOGI(TAG, "Monitoring task started");

    while (1) {
        if (xQueueReceive(
            sensor_queue,
            &received_data,
            portMAX_DELAY
        ) == pdTRUE) {

            const char *status =
                get_moisture_status(received_data.soil_moisture);

            if (received_data.dht_valid) {
                ESP_LOGI(
                    TAG,
                    "Soil moisture ADC: %d | Status: %s | "
                    "Temp: %d C | Humidity: %d %%",
                    received_data.soil_moisture,
                    status,
                    received_data.temperature_c,
                    received_data.humidity_pct
                );
            } else {
                ESP_LOGI(
                    TAG,
                    "Soil moisture ADC: %d | Status: %s | "
                    "DHT11: no valid reading",
                    received_data.soil_moisture,
                    status
                );
            }

            latest_soil_adc = received_data.soil_moisture;
            latest_temperature_c = received_data.temperature_c;
            latest_humidity_pct = received_data.humidity_pct;
            latest_dht_valid = received_data.dht_valid;

            bool reading_dry =
                received_data.soil_moisture > SOIL_DRY_THRESHOLD;

            if (reading_dry == confirmed_dry) {
                /* Reading agrees with the confirmed state. */
                opposite_count = 0;
            } else {
                opposite_count++;

                if (opposite_count >= CONFIRM_READINGS) {
                    confirmed_dry = reading_dry;
                    opposite_count = 0;

                    ESP_LOGI(
                        TAG,
                        "Confirmed state change: %s",
                        confirmed_dry ? "DRY" : "NOT DRY"
                    );

                    if (confirmed_dry) {
                        xEventGroupClearBits(plant_event_group, SOIL_NOT_DRY_BIT);
                        xEventGroupSetBits(plant_event_group, SOIL_DRY_BIT);
                    } else {
                        xEventGroupClearBits(plant_event_group, SOIL_DRY_BIT);
                        xEventGroupSetBits(plant_event_group, SOIL_NOT_DRY_BIT);
                    }
                }
            }
        }
    }
}

/* Current local time (UTC+8) for messages, or a notice if not synced. */
static void format_timestamp(char *buffer, size_t size)
{
    time_t now = time(NULL);

    if (now < 1700000000) {   /* clock still near 1970: not synced yet */
        snprintf(buffer, size, "time not synced");
        return;
    }

    struct tm local_time;
    localtime_r(&now, &local_time);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &local_time);
}

/* Describe the latest DHT11 reading for use in messages. */
static void format_environment(char *buffer, size_t size)
{
    if (latest_dht_valid) {
        snprintf(
            buffer,
            size,
            "%d C, %d %% humidity",
            latest_temperature_c,
            latest_humidity_pct
        );
    } else {
        snprintf(buffer, size, "unavailable (sensor error)");
    }
}

/* Send one message to Telegram and Gmail, retrying if both fail.
 * must_stay_set: event bits that must still be set after each retry delay,
 * otherwise the retry is cancelled. Returns true if at least one channel
 * delivered the message. */
static bool deliver_message(const char *message, EventBits_t must_stay_set)
{
    for (int attempt = 1; attempt <= MAX_SEND_ATTEMPTS; attempt++) {

        bool telegram_ok = send_telegram_message(message);
        bool gmail_ok = send_gmail_notification(message);

        if (telegram_ok || gmail_ok) {
            ESP_LOGI(
                TAG,
                "Message delivered (Telegram: %s, Gmail: %s)",
                telegram_ok ? "OK" : "FAILED",
                gmail_ok ? "OK" : "FAILED"
            );
            return true;
        }

        if (attempt < MAX_SEND_ATTEMPTS) {
            ESP_LOGW(
                TAG,
                "All channels failed (attempt %d of %d). Retrying in %d s...",
                attempt,
                MAX_SEND_ATTEMPTS,
                SEND_RETRY_DELAY_MS / 1000
            );

            vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_DELAY_MS));

            EventBits_t bits = xEventGroupGetBits(plant_event_group);

            if ((bits & must_stay_set) != must_stay_set) {
                ESP_LOGW(TAG, "Retry cancelled: condition changed or Wi-Fi lost.");
                return false;
            }
        }
    }

    return false;
}

/* Notification Task */
static void notification_task(void *pvParameters)
{
    TickType_t last_alert_tick = 0;
    bool alert_sent_before = false;
    bool delivered_this_episode = false;
    char message[220];
    char environment[48];
    char timestamp[32];

    ESP_LOGI(TAG, "Notification task started");

    while (1) {
        /* Wait until the soil is confirmed DRY. */
        xEventGroupWaitBits(
            plant_event_group,
            SOIL_DRY_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY
        );

        delivered_this_episode = false;

        ESP_LOGW(TAG, "ALERT: Soil is DRY! Water your plant.");

        /* Wait until Wi-Fi is connected before sending alerts. */
        EventBits_t wifi_bits = xEventGroupWaitBits(
            plant_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY
        );

        if ((wifi_bits & WIFI_CONNECTED_BIT) != 0) {

            TickType_t now = xTaskGetTickCount();

            if (alert_sent_before &&
                (now - last_alert_tick) < pdMS_TO_TICKS(ALERT_COOLDOWN_MS)) {

                ESP_LOGW(
                    TAG,
                    "Alert suppressed: cool-down active (%lu s left)",
                    (unsigned long)((pdMS_TO_TICKS(ALERT_COOLDOWN_MS) -
                                     (now - last_alert_tick)) /
                                    configTICK_RATE_HZ)
                );

            } else {
                format_environment(environment, sizeof(environment));
                format_timestamp(timestamp, sizeof(timestamp));

                snprintf(
                    message,
                    sizeof(message),
                    "[%s] ALERT: Soil is DRY (ADC %d). Air: %s. Time: %s (UTC+8). "
                    "Reason: %d consecutive readings above %d. "
                    "Water your plant.",
                    PLANT_ID,
                    latest_soil_adc,
                    environment,
                    timestamp,
                    CONFIRM_READINGS,
                    SOIL_DRY_THRESHOLD
                );

                if (deliver_message(
                        message,
                        SOIL_DRY_BIT | WIFI_CONNECTED_BIT)) {
                    last_alert_tick = xTaskGetTickCount();
                    alert_sent_before = true;
                    delivered_this_episode = true;
                } else {
                    ESP_LOGE(
                        TAG,
                        "Alert NOT delivered. Cool-down not started."
                    );
                }
            }

        } else {
            ESP_LOGW(
                TAG,
                "Notifications not sent: Wi-Fi is unavailable."
            );
        }

        /* Wait until the soil is confirmed no longer DRY. */
        xEventGroupWaitBits(
            plant_event_group,
            SOIL_NOT_DRY_BIT,
            pdTRUE,
            pdTRUE,
            portMAX_DELAY
        );

        ESP_LOGI(TAG, "Soil condition is no longer DRY.");

        /* Recovery notice: only if this dry episode's alert was delivered. */
        if (delivered_this_episode) {
            if ((xEventGroupGetBits(plant_event_group) &
                 WIFI_CONNECTED_BIT) != 0) {

                format_environment(environment, sizeof(environment));
                format_timestamp(timestamp, sizeof(timestamp));

                snprintf(
                    message,
                    sizeof(message),
                    "[%s] RECOVERY: Soil is no longer DRY (ADC %d). Air: %s. Time: %s (UTC+8). "
                    "Reason: %d consecutive readings at or below %d.",
                    PLANT_ID,
                    latest_soil_adc,
                    environment,
                    timestamp,
                    CONFIRM_READINGS,
                    SOIL_DRY_THRESHOLD
                );

                if (!deliver_message(message, WIFI_CONNECTED_BIT)) {
                    ESP_LOGE(TAG, "Recovery notice NOT delivered.");
                }
            } else {
                ESP_LOGW(
                    TAG,
                    "Recovery notice not sent: Wi-Fi is unavailable."
                );
            }
        }
    }
}

/* Main Application */
void app_main(void)
{
    /* Initialize ADC. */
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

    /* Philippines time (UTC+8). */
    setenv("TZ", "PST-8", 1);
    tzset();

    /* Initialize DHT11 data pin. */
    ESP_ERROR_CHECK(dht11_init(DHT_GPIO));

    /* Create sensor queue. */
    sensor_queue = xQueueCreate(5, sizeof(sensor_data_t));

    if (sensor_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor queue");
        return;
    }

    /* Create Event Group. */
    plant_event_group = xEventGroupCreate();

    if (plant_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Event Group");
        return;
    }

    /* Initialize Wi-Fi only after the Event Group exists. */
    wifi_init_sta();

    /* Create FreeRTOS tasks. */
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

    xTaskCreate(
        notification_task,
        "notification_task",
        6144,
        NULL,
        5,
        NULL
    );

    ESP_LOGI(TAG, "Smart Plant Monitoring System started");
}