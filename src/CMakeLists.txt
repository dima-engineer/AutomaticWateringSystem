# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       REQUIRES esp_adc driver nvs_flash esp_hw_support
                                esp_wifi esp_event esp_netif esp-mqtt)
