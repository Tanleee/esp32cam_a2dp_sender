#include "wifi_mgr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";
static EventGroupHandle_t s_eg;
static bool s_initialized = false;
#define WIFI_CONNECTED_BIT BIT0

static wifi_mgr_status_cb_t s_status_cb = NULL;
static char s_current_ssid[33] = "";

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);
        if (s_status_cb) s_status_cb(false, "");
        esp_wifi_connect();   // tu dong thu lai
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_eg, WIFI_CONNECTED_BIT);
        if (s_status_cb) s_status_cb(true, s_current_ssid);
    }
}

esp_err_t wifi_mgr_init_and_connect(const char *ssid, const char *password)
{
    if (!s_initialized) {
        s_eg = xEventGroupCreate();
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt, NULL));

        strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
        s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';

        wifi_config_t wifi_cfg = { 0 };
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_initialized = true;
    } else if (wifi_mgr_is_connected()) {
        return ESP_OK;
    } else {
        esp_wifi_connect();
    }

    EventBits_t bits = xEventGroupWaitBits(s_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Da ket noi WiFi");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Khong ket noi WiFi trong 15s");
    return ESP_ERR_TIMEOUT;
}

bool wifi_mgr_is_connected(void)
{
    return s_eg && (xEventGroupGetBits(s_eg) & WIFI_CONNECTED_BIT);
}

void wifi_mgr_set_status_callback(wifi_mgr_status_cb_t cb)
{
    s_status_cb = cb;
}

int wifi_mgr_scan(wifi_ap_record_t *out_records, int max_count)
{
    if (!s_initialized || max_count <= 0) return 0;

    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    // block = true: scan dong bo, tra ve khi quet xong (vai giay). Scan trong
    // luc van dang ket noi 1 AP khac duoc ESP-IDF ho tro (foreground scan),
    // co the gay gian doan truyen du lieu rat ngan.
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Quet WiFi that bai: %s", esp_err_to_name(ret));
        return 0;
    }

    uint16_t count = (uint16_t)max_count;
    ret = esp_wifi_scan_get_ap_records(&count, out_records);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Lay danh sach mang WiFi that bai: %s", esp_err_to_name(ret));
        return 0;
    }
    return (int)count;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *password)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
    s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    }

    xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_connect();
    if (ret != ESP_OK) return ret;

    EventBits_t bits = xEventGroupWaitBits(s_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Da ket noi mang WiFi moi: %s", ssid);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Ket noi mang WiFi moi that bai/het thoi gian: %s", ssid);
    return ESP_ERR_TIMEOUT;
}
