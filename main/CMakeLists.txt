idf_component_register(
    SRCS "main.c" "ota_update.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "../certs/github_root_ca.pem"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server
             mdns lwip esp_https_ota esp_http_client esp_driver_gpio
             bootloader_support
    PRIV_REQUIRES app_update
)