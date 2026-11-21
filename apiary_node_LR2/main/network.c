#include "network.h"
#include "node_config.h"
#include "screen.h"
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"

#define MQTT_READY BIT0
#define APIARY_ID "ap01"
#define GATEWAY_ID "gw01"

typedef struct {
    char ssid[33];
    char password[65];
    char host[128];
    uint16_t port;
} network_config_t;

static const char *tag = "lab2";
static network_config_t config;
static QueueHandle_t samples;
static EventGroupHandle_t state;
static esp_timer_handle_t retry_timer;
static esp_mqtt_client_handle_t client;
static unsigned retry_seconds = 1;
static char ap_ssid[16];
static char ap_password[16];
static char uri[160];
static const char status_topic[] = "apiary/" APIARY_ID "/gateway/" GATEWAY_ID "/status";
static const char telemetry_topic[] = "apiary/" APIARY_ID "/hive/" HIVE_ID "/telemetry";

const char *network_setup_ssid(void)
{
    return ap_ssid;
}

const char *network_setup_password(void)
{
    return ap_password;
}

static int64_t time_ms(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec >= 1700000000 ? (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000 : 0;
}

static bool load_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open("lab2", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t length = sizeof(config);
    esp_err_t result = nvs_get_blob(handle, "network", &config, &length);
    nvs_close(handle);
    return result == ESP_OK && length == sizeof(config) && config.ssid[0] && config.host[0] && config.port;
}

static esp_err_t save_settings(const network_config_t *settings)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("lab2", NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_blob(handle, "network", settings, sizeof(*settings));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static int hex_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = (unsigned char)tolower(ch);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static bool form_value(const char *body, const char *key, char *value, size_t capacity)
{
    size_t key_length = strlen(key);
    while (*body) {
        const char *end = strchr(body, '&');
        if (!end) {
            end = body + strlen(body);
        }
        if ((size_t)(end - body) > key_length && body[key_length] == '=' &&
            memcmp(body, key, key_length) == 0) {
            size_t index = 0;
            for (const char *p = body + key_length + 1; p < end; ++p) {
                unsigned char ch = (unsigned char)*p;
                if (ch == '%') {
                    if (end - p < 3) {
                        return false;
                    }
                    int high = hex_digit(p[1]);
                    int low = hex_digit(p[2]);
                    if (high < 0 || low < 0) {
                        return false;
                    }
                    ch = (unsigned char)(high * 16 + low);
                    p += 2;
                } else if (ch == '+') {
                    ch = ' ';
                }
                if (ch == 0 || index + 1 >= capacity) {
                    return false;
                }
                value[index++] = (char)ch;
            }
            value[index] = 0;
            return true;
        }
        body = *end ? end + 1 : end;
    }
    return false;
}

static esp_err_t show_setup(httpd_req_t *req)
{
    static const char html[] =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<title>Настройка узла</title><h1>Настройка узла</h1>"
        "<form action=/save method=post>"
        "<p><label>SSID <input name=ssid required maxlength=32></label></p>"
        "<p><label>Пароль Wi-Fi <input name=pass type=password maxlength=64></label></p>"
        "<p><label>IP или имя брокера <input name=host required maxlength=127></label></p>"
        "<p><label>Порт <input name=port type=number min=1 max=65535 value=1883 required></label></p>"
        "<button>Сохранить и перезапустить</button></form>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, html);
}

static void restart_node(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t accept_setup(httpd_req_t *req)
{
    char body[1024];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form size");
    }
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) {
            return ESP_FAIL;
        }
        received += count;
    }
    body[received] = 0;
    network_config_t settings = {0};
    char port[6];
    if (!form_value(body, "ssid", settings.ssid, sizeof(settings.ssid)) ||
        !form_value(body, "pass", settings.password, sizeof(settings.password)) ||
        !form_value(body, "host", settings.host, sizeof(settings.host)) ||
        !form_value(body, "port", port, sizeof(port)) ||
        !settings.ssid[0] || !settings.host[0] ||
        strspn(settings.host, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != strlen(settings.host) ||
        !port[0] || strspn(port, "0123456789") != strlen(port)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi or MQTT settings");
    }
    size_t password_length = strlen(settings.password);
    long number = strtol(port, NULL, 10);
    if ((password_length && password_length < 8) || number < 1 || number > 65535) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi password length or broker port");
    }
    settings.port = (uint16_t)number;
    if (save_settings(&settings) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t result = httpd_resp_sendstr(req, "Настройки сохранены. Узел перезапускается.");
    if (xTaskCreate(restart_node, "restart", 2048, NULL, 3, NULL) != pdPASS) {
        esp_restart();
    }
    return result;
}

static void start_ap(void)
{
    esp_netif_create_default_wifi_ap();
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(ap_ssid, sizeof(ap_ssid), "apiary-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(ap_password, sizeof(ap_password), "HIVE%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t wifi = {0};
    memcpy(wifi.ap.ssid, ap_ssid, strlen(ap_ssid));
    memcpy(wifi.ap.password, ap_password, strlen(ap_password));
    wifi.ap.channel = 6;
    wifi.ap.max_connection = 4;
    wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_handle_t server;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = show_setup};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = accept_setup};
    ESP_ERROR_CHECK(httpd_start(&server, &http));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));
    ESP_LOGI(tag, "AP %s password=%s http://192.168.4.1/", ap_ssid, ap_password);
}

static void mqtt_events(void *args, esp_event_base_t base, int32_t id, void *event)
{
    if (id == MQTT_EVENT_CONNECTED) {
        char message[72];
        snprintf(message, sizeof(message), "{\"status\":\"online\",\"ts\":%lld}", (long long)time_ms());
        esp_mqtt_client_publish(client, status_topic, message, 0, 0, 1);
        xEventGroupSetBits(state, MQTT_READY);
        ESP_LOGI(tag, "MQTT online %s", status_topic);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        xEventGroupClearBits(state, MQTT_READY);
        ESP_LOGW(tag, "MQTT disconnected");
    }
}

static void start_mqtt(void)
{
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", config.host, (unsigned)config.port);
    const esp_mqtt_client_config_t mqtt = {
        .broker.address.uri = uri,
        .credentials.client_id = GATEWAY_ID,
        .session.keepalive = 30,
        .session.last_will = {
            .topic = status_topic,
            .msg = "{\"status\":\"offline\"}",
            .qos = 0,
            .retain = true,
        },
    };
    client = esp_mqtt_client_init(&mqtt);
    ESP_ERROR_CHECK(client ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_events, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
}

static void retry_wifi(void *arg)
{
    esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGW(tag, "Wi-Fi connect: %s", esp_err_to_name(result));
        esp_timer_start_once(retry_timer, 30000000);
    }
}

static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *event)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        retry_wifi(NULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(state, MQTT_READY);
        esp_timer_stop(retry_timer);
        ESP_LOGW(tag, "Wi-Fi disconnected, retry in %u s", retry_seconds);
        esp_timer_start_once(retry_timer, (uint64_t)retry_seconds * 1000000);
        retry_seconds = retry_seconds < 15 ? retry_seconds * 2 : 30;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got = event;
        ESP_LOGI(tag, "STA IP " IPSTR, IP2STR(&got->ip_info.ip));
        esp_timer_stop(retry_timer);
        retry_seconds = 1;
        if (!client) {
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
            start_mqtt();
        }
    }
}

static void publish_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(state, MQTT_READY, pdFALSE, pdTRUE, portMAX_DELAY);
        measurement_t sample;
        if (xQueuePeek(samples, &sample, 0) == pdTRUE) {
            char weight[32] = "null";
            char battery[12] = "null";
            if (isfinite(sample.grams)) {
                snprintf(weight, sizeof(weight), "%.0f", (double)sample.grams);
            }
            if (sample.battery_ok) {
                snprintf(battery, sizeof(battery), "%d", sample.charge);
            }
            double rms = isfinite(sample.dbfs) ? fmin(1.0, pow(10.0, sample.dbfs / 20.0)) : -1.0;
            char message[256];
            int length = snprintf(message, sizeof(message),
                "{\"node_addr\":%u,\"ts\":%lld,\"weight_g\":%s,\"rms\":%.8g,"
                "\"band_100_200_ratio\":0,\"swarm_flag\":false,\"battery_pct\":%s}",
                (unsigned)NODE_ADDRESS, (long long)time_ms(), weight, rms, battery);
            if (length > 0 && (size_t)length < sizeof(message) &&
                esp_mqtt_client_publish(client, telemetry_topic, message, length, 0, 1) >= 0) {
                ESP_LOGI(tag, "%s %s", telemetry_topic, message);
            }
            vTaskDelay(pdMS_TO_TICKS(30000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void network_start(QueueHandle_t measurements)
{
    samples = measurements;
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    if (!load_settings()) {
        start_ap();
        return;
    }
    state = xEventGroupCreate();
    ESP_ERROR_CHECK(state ? ESP_OK : ESP_ERR_NO_MEM);
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_events, NULL));
    const esp_timer_create_args_t timer = {.callback = retry_wifi, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &retry_timer));
    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, config.ssid, strlen(config.ssid));
    memcpy(wifi.sta.password, config.password, strlen(config.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(xTaskCreate(publish_task, "mqtt_pub", 4096, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
