#include "uart_link.h"
#include "board_config.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "UART_LINK";

#define LINE_BUF_SZ     2560

static uart_link_callbacks_t s_cbs;

// MOI: luu lai chuoi INIT_DONE de gui lai khi ESP32-C3 hoi (HELLO). ESP32 classic
// khoi tao xong nhanh hon C3 nen INIT_DONE gui lan dau co the roi vao luc C3
// chua mo UART -> bi mat. Cho phep C3 chu dong xin lai.
static char          s_init_status[160];
static volatile bool s_init_done = false;

static void write_init_done(void)
{
    char msg[sizeof(s_init_status) + 16];
    int n = snprintf(msg, sizeof(msg), "INIT_DONE:%s\n", s_init_status);
    if (n > 0) uart_write_bytes(UART_PORT, msg, n);
}

// C3 gui "HELLO" (lap lai moi ~500ms trong luc cho INIT_DONE). Neu da init xong
// thi gui lai INIT_DONE; neu chua xong thi bo qua - INIT_DONE se duoc gui khi
// app_main hoan tat.
static void handle_hello(void)
{
    if (s_init_done) {
        ESP_LOGI(TAG, "Nhan HELLO tu C3 -> gui lai INIT_DONE");
        write_init_done();
    }
}

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

static void handle_brightness_line(const char *payload)
{
    int val = atoi(payload);
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    if (s_cbs.on_brightness_set) s_cbs.on_brightness_set((uint8_t)val);
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
            } else if ((strcmp((char *)line, "BT_ENABLE") == 0 || strcmp((char *)line, "BT_CONNECT") == 0)
                       && s_cbs.on_bt_enable) {
                // "BT_CONNECT" tran (khong kem dia chi) la lenh cua ban C3 cu -> coi nhu BT_ENABLE.
                s_cbs.on_bt_enable();
            } else if (strcmp((char *)line, "BT_DISABLE") == 0 && s_cbs.on_bt_disable) {
                s_cbs.on_bt_disable();
            } else if (strcmp((char *)line, "BT_SCAN_REQ") == 0 && s_cbs.on_bt_scan_req) {
                s_cbs.on_bt_scan_req();
            } else if (strncmp((char *)line, "BT_CONNECT:", 11) == 0 && s_cbs.on_bt_connect) {
                s_cbs.on_bt_connect((char *)line + 11);
            } else if (strcmp((char *)line, "BT_DISCONNECT") == 0 && s_cbs.on_bt_disconnect) {
                s_cbs.on_bt_disconnect();
            } else if (strncmp((char *)line, "BT_FORGET:", 10) == 0 && s_cbs.on_bt_forget) {
                s_cbs.on_bt_forget((char *)line + 10);
            } else if (strcmp((char *)line, "NET_CHECK") == 0 && s_cbs.on_net_check) {
                s_cbs.on_net_check();
            } else if (strcmp((char *)line, "WIFI_SCAN_REQ") == 0 && s_cbs.on_wifi_scan_req) {
                s_cbs.on_wifi_scan_req();
            } else if (strncmp((char *)line, "WIFI_CONNECT:", 13) == 0) {
                handle_wifi_connect_line((char *)line + 13);
            } else if (strcmp((char *)line, "WIFI_ENABLE") == 0 && s_cbs.on_wifi_enable) {
                s_cbs.on_wifi_enable(true);
            } else if (strcmp((char *)line, "WIFI_DISABLE") == 0 && s_cbs.on_wifi_enable) {
                s_cbs.on_wifi_enable(false);
            } else if (strncmp((char *)line, "WIFI_PW_QUERY:", 14) == 0 && s_cbs.on_wifi_pw_query) {
                s_cbs.on_wifi_pw_query((char *)line + 14);
            } else if (strncmp((char *)line, "WIFI_FORGET:", 12) == 0 && s_cbs.on_wifi_forget) {
                s_cbs.on_wifi_forget((char *)line + 12);
            } else if (strncmp((char *)line, "BRIGHTNESS:", 11) == 0) {
                handle_brightness_line((char *)line + 11);
            } else if (strcmp((char *)line, "HELLO") == 0) {
                handle_hello();
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

    esp_err_t ret = uart_driver_install(UART_PORT, LINE_BUF_SZ * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK) return ret;
    ret = uart_param_config(UART_PORT, &cfg);
    if (ret != ESP_OK) return ret;
    ret = uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    BaseType_t ok = xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 6, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
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

void uart_link_send_net_ok(void)
{
    const char *msg = "NET_OK\n";
    uart_write_bytes(UART_PORT, msg, strlen(msg));
}

void uart_link_send_net_err(const char *code)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "NET_ERR:%s\n", code);
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
    int n = snprintf(buf, sizeof(buf), "WIFI_STATUS:%d:%s\n", connected ? 1 : 0, ssid ? ssid : "");
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_bt_status(bool connected, const char *device_name)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "BT_STATUS:%d:%s\n", connected ? 1 : 0, device_name ? device_name : "");
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_wifi_list(const char *networks_csv)
{
    size_t msg_len = strlen("WIFI_LIST:") + strlen(networks_csv) + 2;
    char *msg = malloc(msg_len);
    if (!msg) { ESP_LOGE(TAG, "Khong du RAM de gui WIFI_LIST"); return; }
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

void uart_link_send_init_status(const char *status_csv)
{
    strncpy(s_init_status, status_csv, sizeof(s_init_status) - 1);
    s_init_status[sizeof(s_init_status) - 1] = '\0';
    s_init_done = true;   // dat SAU khi da copy xong chuoi
    write_init_done();
}

void uart_link_send_wifi_switch(bool switched_immediately)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "WIFI_SWITCH:%d\n", switched_immediately ? 1 : 0);
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_busy(const char *reason)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "BUSY:%s\n", reason);
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_wifi_pw(const char *ssid, const char *password)
{
    // "WIFI_PW:" + ssid (<=32) + ':' + password (<=64) + '\n' + NUL
    char buf[8 + 32 + 1 + 64 + 2 + 8];
    int n = snprintf(buf, sizeof(buf), "WIFI_PW:%s:%s\n", ssid, password);
    if (n < 0 || n >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "WIFI_PW qua dai, bo qua");
        return;
    }
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_wifi_pw_none(const char *ssid)
{
    char buf[16 + 32 + 4];
    int n = snprintf(buf, sizeof(buf), "WIFI_PW_NONE:%s\n", ssid);
    if (n < 0 || n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_wifi_power(bool on)
{
    const char *msg = on ? "WIFI_POWER:1\n" : "WIFI_POWER:0\n";
    uart_write_bytes(UART_PORT, msg, strlen(msg));
}
void uart_link_send_bt_power(bool on)
{
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "BT_POWER:%d\n", on ? 1 : 0);
    uart_write_bytes(UART_PORT, buf, n);
}

void uart_link_send_bt_list(const char *devices_csv)
{
    size_t msg_len = strlen("BT_LIST:") + strlen(devices_csv) + 2;
    char *msg = malloc(msg_len);
    if (!msg) { ESP_LOGE(TAG, "Khong du RAM de gui BT_LIST"); return; }
    int n = snprintf(msg, msg_len, "BT_LIST:%s\n", devices_csv);
    uart_write_bytes(UART_PORT, msg, n);
    free(msg);
}

void uart_link_send_bt_err(const char *code)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "BT_ERR:%s\n", code);
    uart_write_bytes(UART_PORT, buf, n);
}
