#include "ai_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "AI_CLIENT";

#define GEMINI_API_KEY   "AQ.Ab8RN6IMhnp_aglwa28YlsA7HtTQVQRoenm2kVowXHFahHrw6g"
#define GEMINI_MODEL     "gemini-3.5-flash"
#define GEMINI_URL       "https://generativelanguage.googleapis.com/v1beta/models/" \
                          GEMINI_MODEL ":generateContent?key=" GEMINI_API_KEY

#define WAV_HEADER_SIZE  44

static void put_u32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put_u16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

static void build_wav_header(uint8_t *hdr, size_t data_len, int sample_rate)
{
    uint32_t byte_rate = sample_rate * 2;
    memcpy(hdr, "RIFF", 4);          put_u32(hdr + 4, 36 + (uint32_t)data_len);
    memcpy(hdr + 8, "WAVE", 4);      memcpy(hdr + 12, "fmt ", 4);
    put_u32(hdr + 16, 16);           put_u16(hdr + 20, 1);
    put_u16(hdr + 22, 1);            put_u32(hdr + 24, sample_rate);
    put_u32(hdr + 28, byte_rate);    put_u16(hdr + 32, 2);
    put_u16(hdr + 34, 16);           memcpy(hdr + 36, "data", 4);
    put_u32(hdr + 40, (uint32_t)data_len);
}

static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 2 < out_sz; i++) {
        if (in[i] == '"' || in[i] == '\\') out[j++] = '\\';
        out[j++] = in[i];
    }
    out[j] = '\0';
}

esp_err_t ai_client_recommend_song(const uint8_t *pcm_data, size_t pcm_len,
                                    int pcm_sample_rate,
                                    const char *playlist_csv,
                                    char *out_song, size_t out_song_sz)
{
    size_t wav_total_len = WAV_HEADER_SIZE + pcm_len;
    uint8_t *wav_buf = malloc(wav_total_len);
    if (!wav_buf) {
        ESP_LOGE(TAG, "Khong du RAM cho WAV buffer (%u byte)", (unsigned)wav_total_len);
        return ESP_ERR_NO_MEM;
    }
    build_wav_header(wav_buf, pcm_len, pcm_sample_rate);
    memcpy(wav_buf + WAV_HEADER_SIZE, pcm_data, pcm_len);

    char playlist_esc[2048];
    json_escape(playlist_csv, playlist_esc, sizeof(playlist_esc));

    // Gio day cho phep Gemini goi y TOI DA 4 bai hat (thay vi dung 1 bai nhu truoc),
    // liet ke cach nhau boi dau phay de app_controller/C3 tach ra thanh danh sach.
    char prompt_text[2560];
    int prompt_len = snprintf(prompt_text, sizeof(prompt_text),
        "Nghe doan ghi am giong noi nay, xac dinh cam xuc cua nguoi noi, "
        "roi chon TOI DA 4 bai hat phu hop nhat trong danh sach sau: [%s]. "
        "Tra loi dung cac ten bai hat, dung nguyen van nhu trong danh sach, "
        "cach nhau boi dau phay, khong danh so, khong giai thich them, "
        "khong them khoang trang thua quanh dau phay neu duoc.",
        playlist_esc);
    if (prompt_len < 0) { free(wav_buf); return ESP_FAIL; }

    size_t b64_len = ((wav_total_len + 2) / 3) * 4;

    char json_prefix[2800];
    int prefix_len = snprintf(json_prefix, sizeof(json_prefix),
        "{\"contents\":[{\"parts\":["
        "{\"text\":\"%s\"},"
        "{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"", prompt_text);
    const char *json_suffix = "\"}}]}]}";
    size_t suffix_len = strlen(json_suffix);
    size_t content_length = prefix_len + b64_len + suffix_len;

    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    esp_http_client_config_t http_cfg = {
        .url = GEMINI_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, content_length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mo HTTP that bai: %s", esp_err_to_name(err));
        free(wav_buf);
        esp_http_client_cleanup(client);
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        return err;
    }

    esp_http_client_write(client, json_prefix, prefix_len);

    const size_t CHUNK = 3000;
    uint8_t b64_tmp[((CHUNK + 2) / 3) * 4 + 4];
    size_t off = 0;
    while (off < wav_total_len) {
        size_t n = wav_total_len - off;
        if (n > CHUNK) n = CHUNK;
        size_t out_len = 0;
        mbedtls_base64_encode(b64_tmp, sizeof(b64_tmp), &out_len, wav_buf + off, n);
        esp_http_client_write(client, (char *)b64_tmp, out_len);
        off += n;
    }
    esp_http_client_write(client, json_suffix, suffix_len);
    free(wav_buf);

    esp_http_client_fetch_headers(client);
    int code = esp_http_client_get_status_code(client);

    char *resp_buf = malloc(4096);
    int total_read = 0;
    if (resp_buf) {
        int r;
        while ((r = esp_http_client_read(client, resp_buf + total_read, 4095 - total_read)) > 0) {
            total_read += r;
            if (total_read >= 4095) break;
        }
        resp_buf[total_read] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

    if (code != 200 || !resp_buf) {
        ESP_LOGE(TAG, "Gemini loi, HTTP %d: %s", code, resp_buf ? resp_buf : "(khong doc duoc)");
        free(resp_buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp_buf);
    free(resp_buf);
    if (!root) return ESP_FAIL;

    esp_err_t ret = ESP_FAIL;
    cJSON *cand0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "candidates"), 0);
    cJSON *content = cand0 ? cJSON_GetObjectItem(cand0, "content") : NULL;
    cJSON *part0 = content ? cJSON_GetArrayItem(cJSON_GetObjectItem(content, "parts"), 0) : NULL;
    cJSON *text = part0 ? cJSON_GetObjectItem(part0, "text") : NULL;

    if (cJSON_IsString(text)) {
        strncpy(out_song, text->valuestring, out_song_sz - 1);
        out_song[out_song_sz - 1] = '\0';
        size_t len = strlen(out_song);
        while (len > 0 && (out_song[len-1]=='\n' || out_song[len-1]=='\r' || out_song[len-1]==' ')) {
            out_song[--len] = '\0';
        }
        ret = ESP_OK;
    }

    cJSON_Delete(root);
    return ret;
}
