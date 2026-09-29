idf_component_register(SRCS "app_main.cpp" "detector.cpp"
 INCLUDE_DIRS "." "../common"
 REQUIRES esp_wifi esp_event esp_netif esp_timer nvs_flash esp_driver_uart esp_driver_usb_serial_jtag esp_driver_gpio esp_driver_ledc esp_csi_gain_ctrl json)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
