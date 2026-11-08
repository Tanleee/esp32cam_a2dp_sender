#include "net_check.h"
#include "wifi_mgr.h"
#include "uart_link.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include <errno.h>
#include <string.h>

static const char *TAG = "NET_CHECK";

#define SERVER_HOST        "generativelanguage.googleapis.com"
#define SERVER_PORT        443
#define PROBE_TIMEOUT_MS   2500   // moi lan thu ket noi TCP toi da 2.5s

static TaskHandle_t  s_task = NULL;
static volatile bool s_busy = false;

// Thu mo ket noi TCP toi addr, toi da timeout_ms. true = ket noi duoc.
static bool tcp_probe(const struct sockaddr *addr, socklen_t alen, int timeout_ms)
{
    int s = socket(addr->sa_family, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) return false;

    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    bool ok = false;
    int r = connect(s, addr, alen);
    if (r == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        struct timeval tv = {
            .tv_sec  = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        if (select(s + 1, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0;
            socklen_t el = sizeof(err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) ok = true;
        }
    }
    close(s);
    return ok;
}

// Thu ket noi TCP toi 1 dia chi IP co dinh (khong qua DNS).
static bool probe_ip(const char *ip, uint16_t port)
{
    struct sockaddr_in sa = { 0 };
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = inet_addr(ip);
    return tcp_probe((struct sockaddr *)&sa, sizeof(sa), PROBE_TIMEOUT_MS);
}

const char *net_check_run(void)
{
    // 1) Tang lien ket: radio bat chua, da co IP tu AP chua.
    if (!wifi_mgr_is_enabled())   return "WIFI_OFF";
    if (!wifi_mgr_is_connected()) return "NO_WIFI";

    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    // 2) Internet that su: ket noi TCP toi IP cong cong (khong phu thuoc DNS).
    //    Co WiFi + IP nhung AP khong ra ngoai duoc (het data, captive portal,
    //    modem mat cap quang...) thi buoc nay se that bai.
    bool reachable = probe_ip("8.8.8.8", 443) || probe_ip("1.1.1.1", 443);
    if (!reachable) {
        ESP_LOGW(TAG, "Co WiFi (rssi=%d) nhung khong ra duoc Internet", rssi);
        return "NO_INTERNET";
    }

    // 3) DNS + may chu Gemini co truy cap duoc khong.
    struct addrinfo hints = { 0 };
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(SERVER_HOST, NULL, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "Khong phan giai duoc DNS cho %s", SERVER_HOST);
        if (res) freeaddrinfo(res);
        return "NO_DNS";
    }

    struct sockaddr_in sa = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    sa.sin_port = htons(SERVER_PORT);
    if (!tcp_probe((struct sockaddr *)&sa, sizeof(sa), PROBE_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Khong ket noi duoc toi %s:%d", SERVER_HOST, SERVER_PORT);
        return "NO_SERVER";
    }

    ESP_LOGI(TAG, "Internet OK (rssi=%d)", rssi);
    return NULL;
}

static void net_check_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const char *err = net_check_run();
        if (err) {
            uart_link_send_net_err(err);
        } else {
            uart_link_send_net_ok();
        }
        s_busy = false;
    }
}

esp_err_t net_check_init(void)
{
    BaseType_t ok = xTaskCreate(net_check_task, "net_check", 5120, NULL, 3, &s_task);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

void net_check_handle_request(void)
{
    if (!s_task || s_busy) {
        uart_link_send_net_err("BUSY");
        return;
    }
    s_busy = true;
    xTaskNotifyGive(s_task);
}
