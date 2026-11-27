#include "remote_link.h"
#include "uart_link.h"
#include "xfer_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

static const char *TAG = "REMOTE_LINK";

// ---------- event bits (dong tra loi cua C3) ----------
#define EV_FILE_READY  (1 << 0)
#define EV_FILE_ERR    (1 << 1)
#define EV_FILE_DONE   (1 << 2)
#define EV_ACK         (1 << 3)
#define EV_NAK         (1 << 4)
#define EV_RC_OK       (1 << 5)
#define EV_RC_ERR      (1 << 6)
#define EV_LIST_END    (1 << 7)

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t  s_rc_mutex;     // 1 lenh RC / LIST tai 1 thoi diem
static SemaphoreHandle_t  s_xfer_mutex;   // 1 file tai 1 thoi diem
static SemaphoreHandle_t  s_state_mutex;  // bao ve cac chuoi dung chung

static volatile uint32_t s_ack_value = 0;
static char s_err_text[28] = "";

// ---------- trang thai C3 (PL_STAT) ----------
static struct {
    int      state;       // 0/1/2
    uint32_t pos, dur;
    int      volume, brightness;
    int      sd_ok;
    uint32_t sd_free, sd_total;
    char     file[64];
} s_pl;
static volatile int64_t s_last_stat_us = 0;

// ---------- Bluetooth (do uart_link bao) ----------
static bool s_bt_connected = false;
static char s_bt_name[40]  = "";

// ---------- danh sach bai ----------
#define MAX_ITEMS 64
typedef struct { char name[64]; uint32_t size; } list_item_t;
static list_item_t s_items[MAX_ITEMS];
static int s_item_count = 0;

// ---------- truyen file ----------
static struct {
    volatile bool active;
    char     type;
    char     name[48];
    uint32_t total, sent;
} s_x;

static uint8_t s_frame[XFER_FRAME_MAX];

esp_err_t remote_link_init(void)
{
    s_ev          = xEventGroupCreate();
    s_rc_mutex    = xSemaphoreCreateMutex();
    s_xfer_mutex  = xSemaphoreCreateMutex();
    s_state_mutex = xSemaphoreCreateMutex();
    return (s_ev && s_rc_mutex && s_xfer_mutex && s_state_mutex) ? ESP_OK : ESP_ERR_NO_MEM;
}

void remote_link_note_bt(bool connected, const char *device_name)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_bt_connected = connected;
    strncpy(s_bt_name, device_name ? device_name : "", sizeof(s_bt_name) - 1);
    s_bt_name[sizeof(s_bt_name) - 1] = '\0';
    xSemaphoreGive(s_state_mutex);
}

// ---------------------------------------------------------------------
// Xu ly dong tu C3 (chay trong task uart_rx - phai nhanh, khong chan)
// ---------------------------------------------------------------------
static void parse_pl_stat(const char *p)
{
    int state = 0, vol = 0, br = 0, sd_ok = 0;
    unsigned pos = 0, dur = 0, fr = 0, tot = 0;
    int consumed = 0;
    if (sscanf(p, "%d|%u|%u|%d|%d|%d|%u|%u|%n", &state, &pos, &dur, &vol, &br, &sd_ok, &fr, &tot, &consumed) < 8 || consumed <= 0) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_pl.state = state; s_pl.pos = pos; s_pl.dur = dur;
    s_pl.volume = vol;  s_pl.brightness = br; s_pl.sd_ok = sd_ok;
    s_pl.sd_free = fr;  s_pl.sd_total = tot;
    strncpy(s_pl.file, p + consumed, sizeof(s_pl.file) - 1);
    s_pl.file[sizeof(s_pl.file) - 1] = '\0';
    xSemaphoreGive(s_state_mutex);
    s_last_stat_us = esp_timer_get_time();
}

bool remote_link_handle_line(const char *line)
{
    if (!s_ev) return false;

    if (strncmp(line, "PL_STAT:", 8) == 0) { parse_pl_stat(line + 8); return true; }

    if (strcmp(line, "FILE_READY") == 0) { xEventGroupSetBits(s_ev, EV_FILE_READY); return true; }
    if (strcmp(line, "FILE_DONE") == 0)  { xEventGroupSetBits(s_ev, EV_FILE_DONE);  return true; }
    if (strncmp(line, "FILE_ERR:", 9) == 0) {
        strncpy(s_err_text, line + 9, sizeof(s_err_text) - 1);
        s_err_text[sizeof(s_err_text) - 1] = '\0';
        xEventGroupSetBits(s_ev, EV_FILE_ERR);
        return true;
    }
    if (strncmp(line, "FILE_ACK:", 9) == 0) {
        s_ack_value = (uint32_t)strtoul(line + 9, NULL, 10);
        xEventGroupSetBits(s_ev, EV_ACK);
        return true;
    }
    if (strncmp(line, "FILE_NAK:", 9) == 0) {
        s_ack_value = (uint32_t)strtoul(line + 9, NULL, 10);
        xEventGroupSetBits(s_ev, EV_NAK);
        return true;
    }

    if (strcmp(line, "RC_OK") == 0) { xEventGroupSetBits(s_ev, EV_RC_OK); return true; }
    if (strncmp(line, "RC_ERR:", 7) == 0) {
        strncpy(s_err_text, line + 7, sizeof(s_err_text) - 1);
        s_err_text[sizeof(s_err_text) - 1] = '\0';
        xEventGroupSetBits(s_ev, EV_RC_ERR);
        return true;
    }

    if (strcmp(line, "LIST_BEGIN") == 0) { s_item_count = 0; return true; }
    if (strncmp(line, "LIST_ITEM:", 10) == 0) {
        const char *bar = strchr(line + 10, '|');
        if (bar && s_item_count < MAX_ITEMS) {
            s_items[s_item_count].size = (uint32_t)strtoul(line + 10, NULL, 10);
            strncpy(s_items[s_item_count].name, bar + 1, sizeof(s_items[0].name) - 1);
            s_items[s_item_count].name[sizeof(s_items[0].name) - 1] = '\0';
            s_item_count++;
        }
        return true;
    }
    if (strcmp(line, "LIST_END") == 0) { xEventGroupSetBits(s_ev, EV_LIST_END); return true; }
    return false;
}

// ---------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------
static size_t json_escape(char *dst, size_t dst_sz, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < dst_sz; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20)         { o += (size_t)snprintf(dst + o, dst_sz - o, "\\u%04x", c); }
        else                       { dst[o++] = (char)c; }
    }
    if (o < dst_sz) dst[o] = '\0';
    return o;
}

static void get_wifi_info(int *rssi, char *ip, size_t ip_sz)
{
    *rssi = 0;
    ip[0] = '\0';
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) *rssi = ap.rssi;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        snprintf(ip, ip_sz, IPSTR, IP2STR(&info.ip));
    }
}

int remote_link_build_status_json(char *buf, size_t buf_size)
{
    if (esp_timer_get_time() - s_last_stat_us > 6LL * 1000 * 1000 || s_last_stat_us == 0) {
        return -1;   // C3 khong bao trang thai -> coi nhu mat ket noi voi thiet bi
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int state = s_pl.state;
    uint32_t pos = s_pl.pos, dur = s_pl.dur, fr = s_pl.sd_free, tot = s_pl.sd_total;
    int vol = s_pl.volume, br = s_pl.brightness, sd_ok = s_pl.sd_ok;
    char file[64], btn[40];
    strncpy(file, s_pl.file, sizeof(file)); file[sizeof(file) - 1] = '\0';
    strncpy(btn, s_bt_name, sizeof(btn));   btn[sizeof(btn) - 1] = '\0';
    bool btc = s_bt_connected;
    xSemaphoreGive(s_state_mutex);

    char file_esc[140], bt_esc[90], xname_esc[110], ip[20];
    json_escape(file_esc, sizeof(file_esc), file);
    json_escape(bt_esc, sizeof(bt_esc), btn);
    int rssi;
    get_wifi_info(&rssi, ip, sizeof(ip));

    bool xa = s_x.active;
    char xtype = s_x.type;
    uint32_t xtot = s_x.total, xsent = s_x.sent;
    char xname[48];
    strncpy(xname, s_x.name, sizeof(xname)); xname[sizeof(xname) - 1] = '\0';
    json_escape(xname_esc, sizeof(xname_esc), xa ? xname : "");
    int pct = (xa && xtot) ? (int)((uint64_t)xsent * 100 / xtot) : 0;

    const char *st = (state == 1) ? "playing" : (state == 2) ? "paused" : "stopped";
    int n = snprintf(buf, buf_size,
        "{\"player\":{\"state\":\"%s\",\"track\":\"%s\",\"position_s\":%u,\"duration_s\":%u,\"volume\":%d},"
        "\"display\":{\"brightness\":%d},"
        "\"sd\":{\"ok\":%s,\"free_mb\":%u,\"total_mb\":%u},"
        "\"wifi\":{\"rssi\":%d,\"ip\":\"%s\"},"
        "\"bt\":{\"connected\":%s,\"device\":\"%s\"},"
        "\"transfer\":{\"active\":%s,\"type\":\"%s\",\"name\":\"%s\",\"percent\":%d}}",
        st, file_esc, (unsigned)pos, (unsigned)dur, vol,
        br,
        sd_ok ? "true" : "false", (unsigned)fr, (unsigned)tot,
        rssi, ip,
        btc ? "true" : "false", bt_esc,
        xa ? "true" : "false", xa ? (xtype == 'A' ? "audio" : "wallpaper") : "", xname_esc, pct);
    return (n > 0 && (size_t)n < buf_size) ? n : -1;
}

int remote_link_build_tracks_json(char *buf, size_t buf_size)
{
    if (xSemaphoreTake(s_rc_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) return -1;
    xEventGroupClearBits(s_ev, EV_LIST_END);
    uart_link_send_line("LIST_REQ");
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_LIST_END, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
    if (!(b & EV_LIST_END)) { xSemaphoreGive(s_rc_mutex); return -1; }

    size_t o = (size_t)snprintf(buf, buf_size, "{\"tracks\":[");
    char esc[140];
    for (int i = 0; i < s_item_count && o + 200 < buf_size; i++) {
        json_escape(esc, sizeof(esc), s_items[i].name);
        o += (size_t)snprintf(buf + o, buf_size - o, "%s{\"name\":\"%s\",\"size\":%u}",
                              i ? "," : "", esc, (unsigned)s_items[i].size);
    }
    o += (size_t)snprintf(buf + o, buf_size - o, "]}");
    xSemaphoreGive(s_rc_mutex);
    return (int)o;
}

bool remote_link_rc(const char *cmd_line, char *err_out, size_t err_sz)
{
    if (xSemaphoreTake(s_rc_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        snprintf(err_out, err_sz, "BUSY");
        return false;
    }
    xEventGroupClearBits(s_ev, EV_RC_OK | EV_RC_ERR);
    char line[100];
    snprintf(line, sizeof(line), "RC:%s", cmd_line);
    uart_link_send_line(line);
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_RC_OK | EV_RC_ERR, pdTRUE, pdFALSE, pdMS_TO_TICKS(2500));
    bool ok = (b & EV_RC_OK) != 0;
    if (!ok) snprintf(err_out, err_sz, "%s", (b & EV_RC_ERR) ? s_err_text : "NO_RESPONSE");
    xSemaphoreGive(s_rc_mutex);
    return ok;
}

// ---------------------------------------------------------------------
// Truyen file
// ---------------------------------------------------------------------
bool remote_link_xfer_begin(char type, const char *name, uint32_t size, char *err_out, size_t err_sz)
{
    if (xSemaphoreTake(s_xfer_mutex, 0) != pdTRUE) {
        snprintf(err_out, err_sz, "TRANSFER_BUSY");
        return false;
    }
    xEventGroupClearBits(s_ev, EV_FILE_READY | EV_FILE_ERR | EV_FILE_DONE | EV_ACK | EV_NAK);

    s_x.type = type;
    strncpy(s_x.name, name, sizeof(s_x.name) - 1);
    s_x.name[sizeof(s_x.name) - 1] = '\0';
    s_x.total = size;
    s_x.sent  = 0;
    s_x.active = true;

    char line[100];
    snprintf(line, sizeof(line), "FILE_BEGIN:%c:%s:%u", type, name, (unsigned)size);
    uart_link_send_line(line);

    EventBits_t b = xEventGroupWaitBits(s_ev, EV_FILE_READY | EV_FILE_ERR, pdTRUE, pdFALSE, pdMS_TO_TICKS(4000));
    if (!(b & EV_FILE_READY)) {
        snprintf(err_out, err_sz, "%s", (b & EV_FILE_ERR) ? s_err_text : "C3_NO_RESPONSE");
        s_x.active = false;
        xSemaphoreGive(s_xfer_mutex);
        return false;
    }
    return true;
}

bool remote_link_xfer_send(const uint8_t *data, size_t len)
{
    if (!s_x.active || len == 0 || len > XFER_MAX_PAYLOAD) return false;

    uint32_t off = s_x.sent;
    uint32_t end = off + (uint32_t)len;

    s_frame[0] = XFER_MAGIC0;
    s_frame[1] = XFER_MAGIC1;
    s_frame[2] = (uint8_t)(off);
    s_frame[3] = (uint8_t)(off >> 8);
    s_frame[4] = (uint8_t)(off >> 16);
    s_frame[5] = (uint8_t)(off >> 24);
    s_frame[6] = (uint8_t)(len);
    s_frame[7] = (uint8_t)(len >> 8);
    memcpy(s_frame + XFER_HDR_LEN, data, len);
    uint16_t crc = xfer_crc16(0xFFFF, s_frame + 2, 6);
    crc = xfer_crc16(crc, data, len);
    s_frame[XFER_HDR_LEN + len]     = (uint8_t)(crc);
    s_frame[XFER_HDR_LEN + len + 1] = (uint8_t)(crc >> 8);
    size_t frame_len = XFER_HDR_LEN + len + 2;

    for (int attempt = 0; attempt < 6; attempt++) {
        xEventGroupClearBits(s_ev, EV_ACK | EV_NAK | EV_FILE_ERR);
        uart_link_write(s_frame, frame_len);

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        for (;;) {
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(deadline - now) <= 0) break;   // timeout -> gui lai
            EventBits_t b = xEventGroupWaitBits(s_ev, EV_ACK | EV_NAK | EV_FILE_ERR, pdTRUE, pdFALSE, deadline - now);
            if (b & EV_FILE_ERR) {
                ESP_LOGW(TAG, "C3 bao loi: %s", s_err_text);
                return false;
            }
            if ((b & EV_ACK) && s_ack_value >= end) { s_x.sent = end; return true; }
            if ((b & EV_NAK)) {
                if (s_ack_value >= end) { s_x.sent = end; return true; }   // C3 da nhan roi
                break;                                                     // gui lai ngay
            }
            // ACK cu (offset nho hon) -> tiep tuc cho
        }
        ESP_LOGW(TAG, "Gui lai khung offset=%u (lan %d)", (unsigned)off, attempt + 1);
    }
    return false;
}

bool remote_link_xfer_finish(char *err_out, size_t err_sz)
{
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_FILE_DONE | EV_FILE_ERR, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    bool ok = (b & EV_FILE_DONE) != 0;
    if (!ok) snprintf(err_out, err_sz, "%s", (b & EV_FILE_ERR) ? s_err_text : "FINISH_TIMEOUT");
    s_x.active = false;
    xSemaphoreGive(s_xfer_mutex);
    return ok;
}

void remote_link_xfer_abort(void)
{
    if (!s_x.active) return;
    uart_link_send_line("FILE_ABORT");
    s_x.active = false;
    xSemaphoreGive(s_xfer_mutex);
}
