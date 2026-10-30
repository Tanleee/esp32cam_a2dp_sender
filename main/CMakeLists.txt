idf_component_register(SRCS "main.c"
                            "src/i2s_slave_rx.c"
                            "src/bt_a2dp_source.cpp"
                            "src/uart_link.c"
                            "src/mic_capture.c"
                            "src/wifi_mgr.c"
                            "src/ai_client.c"
                            "src/app_controller.c"
                            "src/volume_button.c"
                            "src/system_ctrl.c"
                       INCLUDE_DIRS "inc"
                       REQUIRES esp_driver_i2s bt nvs_flash esp_ringbuf ESP32-A2DP
                                esp_driver_uart esp_driver_gpio esp_wifi esp_netif esp_event
                                esp_http_client esp-tls mbedtls esp_coex lwip)
# LUU Y: them "lwip" vao REQUIRES vi system_ctrl.c dung esp_netif_sntp.h (dong
# bo thoi gian qua SNTP, API moi cua IDF v5/v6) - header nay nam trong thu vien
# SNTP cua lwIP duoc esp_netif boc lai. Neu build bao khong tim thay
# esp_netif_sntp.h, kiem tra lai idf.py --list-components xem component chua
# ham esp_netif_sntp_init/esp_netif_sntp_sync_wait ten chinh xac la gi tren
# phien ban ESP-IDF ban dang dung (v6.0.2 theo log truoc do).
