idf_component_register(SRCS "main.c"
                            "i2s_slave_rx.c"
                            "bt_a2dp_source.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2s bt nvs_flash esp_ringbuf ESP32-A2DP)