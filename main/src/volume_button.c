#include "volume_button.h"
#include "uart_link.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "VOL_BTN";

// 2 nut nhan tang/giam am luong duoc chuyen tu ESP32-C3 sang board nay.
// Board khong co board_config.h rieng nen khai bao pin ngay tai day.
// GPIO input khong pull-up/pull-down (dung dien tro ngoai), nhan = muc HIGH,
// ngat canh len, giong het logic debounce ben C3 truoc day.
#define PIN_BTN_VOL_UP     18
#define PIN_BTN_VOL_DOWN   19
#define DEBOUNCE_MS        300

static QueueHandle_t s_btn_queue = NULL;

static void IRAM_ATTR btn_isr_handler(void *arg)
{
    gpio_num_t pin = (gpio_num_t)(uint32_t)arg;
    BaseType_t higher_prio_woken = pdFALSE;
    xQueueSendFromISR(s_btn_queue, &pin, &higher_prio_woken);
    if (higher_prio_woken) portYIELD_FROM_ISR();
}

static void button_task(void *arg)
{
    gpio_num_t pin;
    int64_t last_up_ms = 0, last_down_ms = 0;
    while (1) {
        if (xQueueReceive(s_btn_queue, &pin, portMAX_DELAY) != pdTRUE) continue;
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t *last_ms = (pin == PIN_BTN_VOL_UP) ? &last_up_ms : &last_down_ms;
        if (now_ms - *last_ms < DEBOUNCE_MS) continue;
        *last_ms = now_ms;

        // Board nay khong phat am thanh truc tiep (khong co i2s_audio),
        // nen chi gui lenh qua UART de ESP32-C3 tu dieu chinh volume cuc bo.
        if (pin == PIN_BTN_VOL_UP) {
            uart_link_send_vol_up();
            ESP_LOGI(TAG, "VOL_UP -> da gui lenh qua UART sang ESP32-C3");
        } else {
            uart_link_send_vol_down();
            ESP_LOGI(TAG, "VOL_DOWN -> da gui lenh qua UART sang ESP32-C3");
        }
    }
}

static void configure_button_gpio(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&cfg);
    gpio_isr_handler_add(pin, btn_isr_handler, (void *)(uint32_t)pin);
}

esp_err_t volume_button_init(void)
{
    s_btn_queue = xQueueCreate(8, sizeof(gpio_num_t));
    if (!s_btn_queue) return ESP_ERR_NO_MEM;
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    configure_button_gpio(PIN_BTN_VOL_UP);
    configure_button_gpio(PIN_BTN_VOL_DOWN);
    xTaskCreate(button_task, "vol_btn", 3072, NULL, 5, NULL);
    return ESP_OK;
}
