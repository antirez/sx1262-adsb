if(NOT CMAKE_SCRIPT_MODE_FILE)
    include(../models/assets.cmake)
endif()
idf_component_register(SRCS app.c radio.c radio_esp.c sampler.c decoder.c display.c history.c cpr.c storage.c observation.c tuning.c "${assets}"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer esp_psram spiffs json nvs_flash)
