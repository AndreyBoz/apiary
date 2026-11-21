idf_component_register(SRCS "main.c" "network.c" REQUIRES apiary_board sensors ui config esp_timer esp_system freertos esp_event esp_http_server esp_netif esp_wifi mqtt nvs_flash)
