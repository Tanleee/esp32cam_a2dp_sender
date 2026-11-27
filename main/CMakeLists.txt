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
                            "src/backlight_ctrl.c"
                            "src/net_check.c"
                            "src/remote_link.c"
                            "src/web_server.c"
                       INCLUDE_DIRS "inc"
                       EMBED_FILES "web/index.html.gz"
                       REQUIRES esp_driver_i2s bt nvs_flash esp_ringbuf ESP32-A2DP
                                esp_driver_uart esp_driver_gpio esp_wifi esp_netif esp_event
                                esp_http_client esp-tls mbedtls esp_coex lwip esp_driver_ledc esp_http_server)