# The HAL's component is named after its directory (CardenzaHAL in a clone, cardenza in CardenzaSynth).
get_filename_component(cardenza_hal_name "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
get_filename_component(cardenza_hal_name "${cardenza_hal_name}" NAME)
idf_component_register(SRCS "main.cpp" REQUIRES ${cardenza_hal_name} esp_psram esp_mm heap log freertos)
