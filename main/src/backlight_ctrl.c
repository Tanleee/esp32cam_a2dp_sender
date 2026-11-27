#include "backlight_ctrl.h"
#include "board_config.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "BACKLIGHT";

#define LEDC_TIMER     LEDC_TIMER_0
#define LEDC_MODE      LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL   LEDC_CHANNEL_0
#define LEDC_DUTY_RES  LEDC_TIMER_10_BIT      // 0-1023
#define LEDC_FREQ_HZ   15000

// MOI: trang thai den nen. s_percent la do sang nguoi dung dat (tu C3 hoac web), con
// s_screen_off la co tat man hinh bang nut. Duty thuc te = 0 neu dang tat, nguoc lai
// theo s_percent. Duoc goi tu nhieu task (uart_rx, web server, nut nhan) nen co mutex.
static uint8_t           s_percent    = 100;
static bool              s_screen_off = false;
static SemaphoreHandle_t s_mutex      = NULL;

static void lock(void)   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

// Goi khi DANG giu mutex.
static void apply_locked(void)
{
    uint32_t duty = s_screen_off ? 0 : (uint32_t)s_percent * 1023 / 100;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

esp_err_t backlight_ctrl_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) return ret;

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = PIN_BACKLIGHT_PWM,
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .duty       = 1023,   // 100% sang mac dinh luc khoi dong
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Da khoi tao PWM do sang man hinh tren GPIO%d", PIN_BACKLIGHT_PWM);
    return ESP_OK;
}

void backlight_ctrl_handle_set(uint8_t percent)
{
    if (percent > 100) percent = 100;

    lock();
    s_percent = percent;
    bool off = s_screen_off;
    if (!off) apply_locked();
    unlock();

    if (off) {
        ESP_LOGI(TAG, "Do sang man hinh -> %u%% (dang tat man hinh, ap dung khi bat lai)", percent);
    } else {
        ESP_LOGI(TAG, "Do sang man hinh -> %u%%", percent);
    }
}

void backlight_ctrl_set_screen_off(bool off)
{
    lock();
    bool changed = (s_screen_off != off);
    s_screen_off = off;
    apply_locked();
    uint8_t percent = s_percent;
    unlock();

    if (changed) {
        if (off) ESP_LOGI(TAG, "Man hinh TAT (den nen = 0)");
        else     ESP_LOGI(TAG, "Man hinh BAT (den nen = %u%%)", percent);
    }
}

bool backlight_ctrl_is_screen_off(void)
{
    lock();
    bool off = s_screen_off;
    unlock();
    return off;
}
