idf_component_register(
    SRCS
        "main.c"
        "config_store.c"
        "led_status.c"
        "volume_buttons.c"
        "captive_portal.c"
        "radio_player.c"
        "ota_update.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        esp_https_ota
        app_update
        spiffs
        json
        mbedtls
        driver
        esp_timer
        led_strip
        audio_pipeline
        audio_stream
        audio_sal
        esp_peripherals
        audio_board
)

spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)