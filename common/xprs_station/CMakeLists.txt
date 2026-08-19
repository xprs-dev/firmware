# The station's generic RAM stores (devices heard, chat ring, traffic
# stats) -- see xprs_station.h. Board firmwares ingest what their bearers
# hear and render from the snapshots; persistence goes wherever the board
# keeps its storage.
idf_component_register(
    SRCS "xprs_station.c"
    INCLUDE_DIRS "."
    REQUIRES geogram_xprs log esp_timer)
