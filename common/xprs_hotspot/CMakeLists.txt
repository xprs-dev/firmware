# The walk-up hotspot: open AP, captive DNS, and a chat page that signs real
# XPRS packets in the browser (chat_page.c is GENERATED -- edit
# chat_page.html / xprs_crypto.js and run tool/embed_page.py).
idf_component_register(
    SRCS "xprs_hotspot.c" "chat_page.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi esp_netif geogram_dns log
)
