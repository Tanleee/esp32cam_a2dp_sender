#include "mic_capture.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

static const char *TAG = "MIC_CAP";

#define PIN_MIC_SCK  32
#define PIN_MIC_WS   33
#define PIN_MIC_SD   34

#define MIC_SAMPLE_RATE   8000

// Toc do lay mau THUC cua I2S. INMP441 can xung SCK du nhanh (BCLK = rate x do rong slot x 2 slot).
// 8 kHz voi slot 16 bit chi cho BCLK = 256 kHz - thap hon muc toi thieu cua mic (theo datasheet,
// khoang 0.5 MHz) nen mic co the im lang (toan 0, peak_rms=0). Chay 16 kHz voi slot 32 bit
// (BCLK = 1.024 MHz, giong sketch Arduino da test OK) roi giam xuong 8 kHz bang cach lay trung binh.
#define MIC_I2S_RATE      16000
#define MIC_DECIM         (MIC_I2S_RATE / MIC_SAMPLE_RATE)   // = 2
#define MIC_FRAME_MS      32
#define MIC_FRAME_BYTES   (MIC_SAMPLE_RATE * MIC_FRAME_MS / 1000 * 2)

// ---------------- THAM SO VAD (chinh tai day neu can) ----------------
// RMS (sau khi tru DC) cua 1 khung 32ms, thang int16. Truoc day la 400 - QUA CAO:
// voi mic MEMS (INMP441...) giong noi binh thuong o ~30cm chi vao khoang 60-200,
// nen phai noi rat to sat mic moi vuot nguong -> "khong nghe thay giong noi".
#define VAD_ENERGY_THRESHOLD             100
// Can it nhat so khung LIEN TIEP vuot nguong moi coi la bat dau noi (loc tieng "tach").
#define VAD_START_FRAMES                 2
// Giu lai so khung TRUOC luc phat hien de khong mat dau am dau cau.
#define VAD_PREROLL_FRAMES               3
#define VAD_SILENCE_MS_TO_STOP           1200
#define VAD_MAX_FRAMES_WAIT_FOR_SPEECH   200      // ~6.4s
// Neu khong qua nguong VAD nhung dinh am thanh >= muc nay thi van coi la co tieng
// (truong hop noi nho / nguoi dung bam dung som) va van gui di.
#define VAD_WEAK_SPEECH_MIN_RMS          45
// Khi chua phat hien giong noi, chi giu cua so ~2s gan nhat (khong de im lang
// chiem het buffer 4s - day la loi TH2).
#define PRESPEECH_WINDOW_BYTES           (MIC_SAMPLE_RATE * 2 * 2)

// Tu dong khuech dai (chuan hoa dinh) truoc khi gui AI de am thanh nho van nghe ro.
#define AUTOGAIN_TARGET_PEAK             20000
#define AUTOGAIN_MAX                     16.0f

static i2s_chan_handle_t s_rx_chan = NULL;

// Co yeu cau dung ghi am som, set tu uart_link khi nhan duoc REC_STOP tu C3.
static volatile bool s_stop_requested = false;

esp_err_t mic_capture_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) return ret;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_I2S_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCK,
            .ws   = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_MIC_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) return ret;

    return i2s_channel_enable(s_rx_chan);
}

// Doc 1 khung (MIC_FRAME_BYTES byte int16 @ MIC_SAMPLE_RATE) tu I2S dang chay 32 bit @ MIC_I2S_RATE:
// lay 16 bit cao (INMP441 xuat 24 bit nam o bit cao) va trung binh MIC_DECIM mau lien tiep.
static esp_err_t mic_read_frame(uint8_t *frame_out, size_t *bytes_out, TickType_t timeout)
{
    static int32_t raw[(MIC_FRAME_BYTES / 2) * MIC_DECIM];
    size_t got = 0;

    esp_err_t r = i2s_channel_read(s_rx_chan, raw, sizeof(raw), &got, timeout);
    if (r != ESP_OK) {
        *bytes_out = 0;
        return r;
    }

    static bool s_logged_raw = false;
    if (!s_logged_raw && got >= 4 * sizeof(int32_t)) {
        s_logged_raw = true;
        ESP_LOGI(TAG, "raw I2S dau tien: %ld %ld %ld %ld (toan 0 = mic khong xuat du lieu)",
                 (long)raw[0], (long)raw[1], (long)raw[2], (long)raw[3]);
    }

    size_t n_in  = got / sizeof(int32_t);
    size_t n_out = n_in / MIC_DECIM;
    int16_t *out = (int16_t *)frame_out;
    for (size_t i = 0; i < n_out; i++) {
        int32_t acc = 0;
        for (int k = 0; k < MIC_DECIM; k++) {
            acc += raw[i * MIC_DECIM + k] >> 16;
        }
        out[i] = (int16_t)(acc / MIC_DECIM);
    }
    *bytes_out = n_out * sizeof(int16_t);
    return ESP_OK;
}

void mic_capture_request_stop(void)
{
    s_stop_requested = true;
}

// RMS cua phan "xoay chieu" (da tru gia tri trung binh/DC) - mic MEMS thuong co
// DC offset, neu khong tru thi RMS bi thoi phong ngay ca luc im lang.
static int32_t frame_energy(const int16_t *samples, size_t count)
{
    if (count == 0) return 0;
    int64_t sum = 0, sumsq = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t v = samples[i];
        sum   += v;
        sumsq += (int64_t)v * v;
    }
    int64_t mean = sum / (int64_t)count;
    int64_t var  = sumsq / (int64_t)count - mean * mean;
    if (var < 0) var = 0;
    return (int32_t)sqrtf((float)var);
}

// Chuan hoa dinh ve AUTOGAIN_TARGET_PEAK (toi da x16). Tra ve he so da ap dung.
static float apply_autogain(int16_t *s, size_t n)
{
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t a = abs((int32_t)s[i]);
        if (a > peak) peak = a;
    }
    if (peak < 30) return 1.0f;   // gan nhu chi co nhieu nen, khong khuech dai

    float gain = (float)AUTOGAIN_TARGET_PEAK / (float)peak;
    if (gain > AUTOGAIN_MAX) gain = AUTOGAIN_MAX;
    if (gain < 1.2f) return 1.0f;

    int32_t g = (int32_t)(gain * 256.0f);
    for (size_t i = 0; i < n; i++) {
        int32_t v = ((int32_t)s[i] * g) >> 8;
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
    }
    return gain;
}

size_t mic_capture_record(uint8_t *out_buf, size_t max_bytes)
{
    size_t total = 0;
    bool speech_started = false;
    int silence_frames = 0;
    int frames_waited = 0;
    int loud_run = 0;
    int32_t peak_rms = 0;
    uint8_t frame[MIC_FRAME_BYTES];
    size_t dummy;

    size_t window = max_bytes < PRESPEECH_WINDOW_BYTES ? max_bytes : PRESPEECH_WINDOW_BYTES;

    // Reset co dung som tu lan ghi am truoc, tranh dinh co cu.
    s_stop_requested = false;

    // Bo 1 khung dau (con du lieu cu / tieng "bop" luc mo kenh).
    mic_read_frame(frame, &dummy, pdMS_TO_TICKS(200));

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t r = mic_read_frame(frame, &bytes_read, pdMS_TO_TICKS(500));
        if (r == ESP_OK && bytes_read > 0) {
            int32_t energy = frame_energy((int16_t *)frame, bytes_read / 2);
            if (energy > peak_rms) peak_rms = energy;

            // --- Luu khung vao buffer ---
            if (!speech_started) {
                // Chua noi: buffer chay nhu cua so truot ~2s gan nhat.
                if (total + bytes_read > window) {
                    size_t drop = total + bytes_read - window;
                    memmove(out_buf, out_buf + drop, total - drop);
                    total -= drop;
                }
            } else if (total + bytes_read > max_bytes) {
                break;   // day buffer sau khi da co giong noi -> ket thuc binh thuong
            }
            memcpy(out_buf + total, frame, bytes_read);
            total += bytes_read;

            // --- VAD ---
            if (!speech_started) {
                if (energy > VAD_ENERGY_THRESHOLD) {
                    if (++loud_run >= VAD_START_FRAMES) {
                        speech_started = true;
                        // Cat bo phan im lang dau, chi giu preroll + cac khung vua noi.
                        size_t keep = (size_t)(loud_run + VAD_PREROLL_FRAMES) * MIC_FRAME_BYTES;
                        if (keep < total) {
                            memmove(out_buf, out_buf + (total - keep), keep);
                            total = keep;
                        }
                        silence_frames = 0;
                    }
                } else {
                    loud_run = 0;
                    if (++frames_waited > VAD_MAX_FRAMES_WAIT_FOR_SPEECH) break;
                }
            } else {
                if (energy < VAD_ENERGY_THRESHOLD) {
                    silence_frames++;
                    if (silence_frames * MIC_FRAME_MS >= VAD_SILENCE_MS_TO_STOP) break;
                } else {
                    silence_frames = 0;
                }
            }
        }

        // Kiem tra REC_STOP SAU KHI da xu ly khung hien tai (truoc day kiem tra truoc
        // nen bo sot khung cuoi cung chua tieng noi).
        if (s_stop_requested) {
            if (!speech_started) {
                if (peak_rms >= VAD_WEAK_SPEECH_MIN_RMS) {
                    ESP_LOGW(TAG, "REC_STOP: chua vuot nguong VAD nhung co tin hieu (peak_rms=%d), van gui AI",
                             (int)peak_rms);
                    speech_started = true;
                } else {
                    ESP_LOGW(TAG, "REC_STOP truoc khi phat hien giong noi, huy ghi am (peak_rms=%d < %d)",
                             (int)peak_rms, VAD_WEAK_SPEECH_MIN_RMS);
                    return 0;
                }
            } else {
                ESP_LOGI(TAG, "Nhan REC_STOP, ket thuc ghi am som");
            }
            break;
        }
    }

    // Het vong lap ma van chua co giong noi (het thoi gian cho / het buffer).
    // TRUOC DAY tra ve ca buffer toan im lang -> gui AI -> sai. Gio coi la NO_SPEECH.
    if (!speech_started) {
        if (peak_rms >= VAD_WEAK_SPEECH_MIN_RMS) {
            speech_started = true;
        } else {
            ESP_LOGW(TAG, "Khong phat hien giong noi, huy ghi am (peak_rms=%d)", (int)peak_rms);
            return 0;
        }
    }

    float gain = apply_autogain((int16_t *)out_buf, total / 2);
    ESP_LOGI(TAG, "Ghi am xong: %u byte (~%.1fs), peak_rms=%d, nguong=%d, gain=x%.1f",
             (unsigned)total, total / 2.0 / MIC_SAMPLE_RATE, (int)peak_rms,
             VAD_ENERGY_THRESHOLD, gain);
    return total;
}