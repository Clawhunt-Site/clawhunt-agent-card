idf_component_register(
    SRCS "main.c"
         "card_app.c"
         "card_model.c"
         "cloud.c"
         "crab_sprite.c"
         "ground_strips.c"
         "host_link.c"
         "net.c"
         "setup.c"
         "sfx.c"
         "ui_card.c"
         "../assets/fonts/font_px12.c"
         "../assets/fonts/font_px24.c"
         "../assets/fonts/font_px36.c"
         "../assets/fonts/font_px48.c"
         "../assets/fonts/font_px72.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_driver_usb_serial_jtag json esp_wifi esp_netif esp_event nvs_flash
             esp_http_client esp-tls mbedtls esp_timer esp_http_server dns_server lwip
    EMBED_TXTFILES "setup_page.html"
)

# cJSON parses recursively, about 64 bytes of stack per level, and allows 1000 levels by
# default. A few hundred "[" in a setup-page body or a USB line would overflow the task
# that parses it. Nothing the card reads is nested deeper than three levels.
idf_component_get_property(json_lib json COMPONENT_LIB)
target_compile_definitions(${json_lib} PRIVATE CJSON_NESTING_LIMIT=8)
