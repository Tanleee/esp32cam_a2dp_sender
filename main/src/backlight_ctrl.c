#include "backlight_ctrl.h"
#include "board_config.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "BACKLIGHT";

#define LEDC_TIMER     LEDC_TIMER_0
#define LEDC_MODE      LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL   LEDC_CHANNEL_0
#define LEDC_DUTY_RES  LEDC_TIMER_10_BIT      // 0-1023
#define LEDC_FREQ_HZ   5000

esp_err_t backlight_ctrl_init(void)
{
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
    uint32_t duty = (uint32_t)percent * 1023 / 100;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    ESP_LOGI(TAG, "Do sang man hinh -> %u%%", percent);
}