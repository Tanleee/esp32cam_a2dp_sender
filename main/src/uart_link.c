#include "uart_link.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "UART_LINK";

#define UART_PORT       UART_NUM_2
#define UART_TX_PIN     17
#define UART_RX_PIN     16
#define UART_BAUD       115200
#define LINE_BUF_SZ     2560

static uart_link_callbacks_t s_cbs;

// Tach "WIFI_CONNECT:<ssid>:<password>" tai dau ':' DAU TIEN sau tien to (cho
// ssid) - phan con lai (co the chua them ':') la mat khau nguyen ven. Khong
// tach password them lan nua, vi mat khau WiFi co the chua ky tu ':'.
static void handle_wifi_connect_line(const char *payload)
{
    const char *sep = strchr(payload, ':');
    if (!sep) {
        ESP_LOGW(TAG, "WIFI_CONNECT thieu dau ':' ngan cach ssid/password: %s", payload);
        return;
    }
    size_t ssid_len = (size_t)(sep - payload);
    char ssid[33];
    if (ssid_len >= sizeof(ssid)) ssid_len = sizeof(ssid) - 1;
    memcpy(ssid, payload, ssid_len);
    ssid[ssid_len] = '\0';

    const char *password = sep + 1;
    if (s_cbs.on_wifi_connect) s_cbs.on_wifi_connect(ssid, password);
}

static void uart_rx_task(void *arg)
{
    uint8_t *line = malloc(LINE_BUF_SZ);
    size_t idx = 0;

    while (1) {
        uint8_t ch;
        int n = uart_read_bytes(UART_PORT, &ch, 1, pdMS_TO_TICKS(1000));
        if (n <= 0) continue;

        if (ch == '\n') {
            line[idx] = '\0';
            idx = 0;

            if (strncmp((char *)line, "REC_START:", 10) == 0 && s_cbs.on_rec_start) {
                s_cbs.on_rec_start((char *)line + 10);
            } else if (strcmp((char *)line, "REC_STOP") == 0 && s_cbs.on_rec_stop) {
                s_cbs.on_rec_stop();
            } else if (strcmp((char *)line, "BT_CONNECT") == 0 && s_cbs.on_bt_connect) {
                s_cbs.on_bt_connect();
            } else if (strcmp((char *)line, "BT_DISCONNECT") == 0 && s_cbs.on_bt_disconnect) {
                s_cbs.on_bt_disconnect();
            } else if (strcmp((char *)line, "WIFI_SCAN_REQ") == 0 && s_cbs.on_wifi_scan_req) {
                s_cbs.on_wifi_scan_req();
            } else if (strncmp((char *)line, "WIFI_CONNECT:", 13) == 0) {
                handle_wifi_connect_line((char *)line + 13);
            } else if (line[0] != '\0') {
                ESP_LOGW(TAG, "Dong UART khong nhan dien: %s", line);
            }
        } else if (ch != '\r' && idx < LINE_BUF_SZ - 1) {
            line[idx++] = ch;
        }
    }
}

esp_err_t uart_link_init(const uart_link_callbacks_t *callbacks)
{
    if (callbacks) s_cbs = *callbacks;

    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, LINE_BUF_SZ * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 6, NULL);
    return ESP_OK;
}

void uart_link_send_ack(void)
{
    const char *msg = "REC_ACK\n";
    uart_write_bytes(UART_PORT, msg, strlen(msg));
}

void uart_link_send_result(const char *song_name)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "RESULT:%s\n", song_name);
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_error(const char *code)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "REC_ERR:%s\n", code);
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_vol_up(void)
{
    const char *msg = "VOL_UP\n";
    uart_write_bytes(UART_PORT, msg, strlen(msg));
}

void uart_link_send_vol_down(void)
{
    const char *msg = "VOL_DOWN\n";
    uart_write_bytes(UART_PORT, msg, strlen(msg));
}

void uart_link_send_time(const char *hhmm, const char *date_str)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "TIME:%s|%s\n", hhmm, date_str);
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_wifi_status(bool connected, const char *ssid)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "WIFI_STATUS:%d:%s\n", connected ? 1 : 0,
                      ssid ? ssid : "");
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_bt_status(bool connected, const char *device_name)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "BT_STATUS:%d:%s\n", connected ? 1 : 0,
                      device_name ? device_name : "");
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_wifi_list(const char *networks_csv)
{
    size_t msg_len = strlen("WIFI_LIST:") + strlen(networks_csv) + 2;
    char *msg = malloc(msg_len);
    if (!msg) {
        ESP_LOGE(TAG, "Khong du RAM de gui WIFI_LIST");
        return;
    }
    int n = snprintf(msg, msg_len, "WIFI_LIST:%s\n", networks_csv);
    uart_write_bytes(UART_PORT, msg, n);
    free(msg);
}

void uart_link_send_wifi_err(const char *code)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "WIFI_ERR:%s\n", code);
    uart_write_bytes(UART_PORT, buf, n);
}
