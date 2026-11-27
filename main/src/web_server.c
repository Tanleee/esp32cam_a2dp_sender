#include "web_server.h"
#include "remote_link.h"
#include "backlight_ctrl.h"
#include "xfer_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"

static const char *TAG = "WEB";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

#define MAX_AUDIO_BYTES   (200u * 1024u * 1024u)
#define JSON_BUF_SZ       3072

static httpd_handle_t s_server = NULL;

// ---------------------------------------------------------------------
// Tien ich
// ---------------------------------------------------------------------
static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char buf[128];
    // msg chi gom ky tu ASCII do firmware tao ra (khong co dau nhay)
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    return send_json(req, status, buf);
}

static const char *friendly_error(const char *code)
{
    if (!strcmp(code, "TRANSFER_BUSY"))  return "Another transfer is in progress";
    if (!strcmp(code, "C3_NO_RESPONSE")) return "ESP32-C3 is not responding";
    if (!strcmp(code, "NO_SD"))          return "SD card is not available";
    if (!strcmp(code, "BAD_NAME"))       return "Invalid file name";
    if (!strcmp(code, "BAD_SIZE"))       return "Invalid file size";
    if (!strcmp(code, "WRITE"))          return "SD card write failed (card full?)";
    if (!strcmp(code, "OPEN"))           return "Cannot create file on SD card";
    if (!strcmp(code, "TIMEOUT"))        return "Transfer timed out";
    if (!strcmp(code, "TRACK_PLAYING"))  return "Track is currently playing";
    if (!strcmp(code, "NOT_FOUND"))      return "Track not found";
    if (!strcmp(code, "NO_TRACKS"))      return "No tracks on SD card";
    if (!strcmp(code, "NO_RESPONSE"))    return "ESP32-C3 did not respond";
    if (!strcmp(code, "BUSY"))           return "Device busy";
    return "Device error";
}

// Ten file an toan: [A-Za-z0-9._-], khong bat dau bang '.', <= 40 ky tu.
static bool name_is_safe(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > 40 || n[0] == '.') return false;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Trang web
// ---------------------------------------------------------------------
static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_gz_start,
                           (ssize_t)(index_html_gz_end - index_html_gz_start));
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------------
// GET /api/status , GET /api/tracks
// ---------------------------------------------------------------------
static esp_err_t status_get(httpd_req_t *req)
{
    char *buf = malloc(JSON_BUF_SZ);
    if (!buf) return send_error(req, "500 Internal Server Error", "Out of memory");
    int n = remote_link_build_status_json(buf, JSON_BUF_SZ);
    esp_err_t r;
    if (n < 0) r = send_error(req, "503 Service Unavailable", "ESP32-C3 is not responding");
    else       r = send_json(req, "200 OK", buf);
    free(buf);
    return r;
}

static esp_err_t tracks_get(httpd_req_t *req)
{
    const size_t sz = 8192;   // 64 bai x ~110 byte
    char *buf = malloc(sz);
    if (!buf) return send_error(req, "500 Internal Server Error", "Out of memory");
    int n = remote_link_build_tracks_json(buf, sz);
    esp_err_t r;
    if (n < 0) r = send_error(req, "503 Service Unavailable", "ESP32-C3 did not return the track list");
    else       r = send_json(req, "200 OK", buf);
    free(buf);
    return r;
}

// ---------------------------------------------------------------------
// POST /api/cmd   {"cmd":"...","value":n,"name":"..."}
// ---------------------------------------------------------------------
static esp_err_t cmd_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 512) {
        return send_error(req, "400 Bad Request", "Bad request body");
    }
    char body[513];
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return send_error(req, "400 Bad Request", "Receive error");
        got += r;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "400 Bad Request", "Invalid JSON");
    const cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *jval = cJSON_GetObjectItemCaseSensitive(root, "value");
    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(jcmd) || !jcmd->valuestring) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "Missing cmd");
    }

    char cmd[24];
    strncpy(cmd, jcmd->valuestring, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    int value = cJSON_IsNumber(jval) ? (int)jval->valuedouble : 0;
    char name[64] = "";
    if (cJSON_IsString(jname) && jname->valuestring) {
        strncpy(name, jname->valuestring, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
    cJSON_Delete(root);

    char line[100], err[28] = "";
    bool ok;

    if (!strcmp(cmd, "play") || !strcmp(cmd, "pause") || !strcmp(cmd, "stop") ||
        !strcmp(cmd, "next") || !strcmp(cmd, "prev")) {
        ok = remote_link_rc(cmd, err, sizeof(err));
    } else if (!strcmp(cmd, "volume")) {
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        snprintf(line, sizeof(line), "volume:%d", value);
        ok = remote_link_rc(line, err, sizeof(err));
    } else if (!strcmp(cmd, "brightness")) {
        if (value < 10) value = 10;
        if (value > 100) value = 100;
        backlight_ctrl_handle_set((uint8_t)value);          // PWM that nam o WROVER
        snprintf(line, sizeof(line), "brightness:%d", value);
        ok = remote_link_rc(line, err, sizeof(err));         // C3 luu lai gia tri
    } else if (!strcmp(cmd, "play_track") || !strcmp(cmd, "delete_track")) {
        if (!name[0] || strpbrk(name, "/\\:|") || strstr(name, "..")) {
            return send_error(req, "400 Bad Request", "Invalid track name");
        }
        snprintf(line, sizeof(line), "%s:%s", cmd, name);
        ok = remote_link_rc(line, err, sizeof(err));
    } else {
        return send_error(req, "400 Bad Request", "Unknown command");
    }

    if (ok) return send_json(req, "200 OK", "{\"ok\":true}");
    return send_error(req, "409 Conflict", friendly_error(err));
}

// ---------------------------------------------------------------------
// POST /api/upload/audio?name=x.wav     POST /api/upload/wallpaper?name=x
// ---------------------------------------------------------------------
static esp_err_t upload_common(httpd_req_t *req, char type)
{
    size_t total = req->content_len;
    if (type == 'W' && total != WALLPAPER_BYTES) {
        return send_error(req, "400 Bad Request", "Wallpaper must be 153600 bytes (240x320 RGB565)");
    }
    if (type == 'A' && (total < 64 || total > MAX_AUDIO_BYTES)) {
        return send_error(req, "400 Bad Request", "Invalid audio size");
    }

    char query[160], name[48] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        !name_is_safe(name)) {
        return send_error(req, "400 Bad Request", "Missing or invalid name");
    }

    char err[28] = "";
    if (!remote_link_xfer_begin(type, name, (uint32_t)total, err, sizeof(err))) {
        ESP_LOGW(TAG, "xfer_begin loi: %s", err);
        const char *st = !strcmp(err, "TRANSFER_BUSY") ? "409 Conflict" : "500 Internal Server Error";
        return send_error(req, st, friendly_error(err));
    }

    uint8_t *chunk = malloc(XFER_MAX_PAYLOAD);
    if (!chunk) {
        remote_link_xfer_abort();
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }

    ESP_LOGI(TAG, "Upload %c %s (%u byte)", type, name, (unsigned)total);
    size_t remaining = total;
    bool ok = true;
    const char *fail = "Transfer to ESP32-C3 failed";

    while (remaining > 0) {
        size_t want = remaining < XFER_MAX_PAYLOAD ? remaining : XFER_MAX_PAYLOAD;
        size_t have = 0;
        int timeouts = 0;
        while (have < want) {
            int r = httpd_req_recv(req, (char *)chunk + have, want - have);
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;   // thu lai (toi da 3 lan)
            if (r <= 0) { ok = false; fail = "Connection lost during upload"; break; }
            have += (size_t)r;
        }
        if (!ok) break;
        if (!remote_link_xfer_send(chunk, want)) { ok = false; break; }
        remaining -= want;
    }
    free(chunk);

    if (!ok) {
        remote_link_xfer_abort();
        ESP_LOGW(TAG, "Upload that bai: %s", fail);
        return send_error(req, "500 Internal Server Error", fail);
    }

    if (!remote_link_xfer_finish(err, sizeof(err))) {
        ESP_LOGW(TAG, "xfer_finish loi: %s", err);
        return send_error(req, "500 Internal Server Error", friendly_error(err));
    }
    ESP_LOGI(TAG, "Upload xong: %s", name);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t upload_audio_post(httpd_req_t *req)     { return upload_common(req, 'A'); }
static esp_err_t upload_wallpaper_post(httpd_req_t *req) { return upload_common(req, 'W'); }

// ---------------------------------------------------------------------
// Khoi dong
// ---------------------------------------------------------------------
static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mdns_init that bai"); return; }
    mdns_hostname_set("musicplayer");
    mdns_instance_name_set("PocketAudio ESP32");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://musicplayer.local");
}

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 6144;
    cfg.max_uri_handlers  = 10;
    cfg.max_open_sockets  = 4;       // LWIP_MAX_SOCKETS = 10, httpd tu dung them 3
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) return ret;

    static const httpd_uri_t uris[] = {
        { .uri = "/",                     .method = HTTP_GET,  .handler = index_get },
        { .uri = "/index.html",           .method = HTTP_GET,  .handler = index_get },
        { .uri = "/favicon.ico",          .method = HTTP_GET,  .handler = favicon_get },
        { .uri = "/api/status",           .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/tracks",           .method = HTTP_GET,  .handler = tracks_get },
        { .uri = "/api/cmd",              .method = HTTP_POST, .handler = cmd_post },
        { .uri = "/api/upload/audio",     .method = HTTP_POST, .handler = upload_audio_post },
        { .uri = "/api/upload/wallpaper", .method = HTTP_POST, .handler = upload_wallpaper_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ret = httpd_register_uri_handler(s_server, &uris[i]);
        if (ret != ESP_OK) return ret;
    }

    start_mdns();
    ESP_LOGI(TAG, "Web server dang chay (cong 80)");
    return ESP_OK;
}
