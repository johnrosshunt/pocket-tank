set(SRCS main.c board_tab5.c director.c brightness.c batlog.c codec_port_tab5.c advisor_llm_esp.c battery_port_axp2101.c imu_port_tab5.c)
if(CONFIG_POCKET_TANK_DISPLAY_TAB5)
    list(APPEND SRCS display_port_tab5.c)
elseif(CONFIG_POCKET_TANK_DISPLAY_SH8601)
    list(APPEND SRCS display_port_sh8601.c)
else()
    list(APPEND SRCS display_port_stub.c)
endif()
if(CONFIG_POCKET_TANK_TOUCH_TAB5)
    list(APPEND SRCS touch_port_tab5.c)
else()
    list(APPEND SRCS touch_port_stub.c)
endif()
if(CONFIG_POCKET_TANK_AUDIO_TAB5)
    list(APPEND SRCS audio_port_tab5.c)
else()
    list(APPEND SRCS audio_port_stub.c)
endif()
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES tankcore llm esp_psram esp_mm esp_driver_ledc esp_driver_ppa esp_hw_support esp_partition spi_flash esp_timer esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_usb_serial_jtag esp_lcd nvs_flash esp_app_format
    EMBED_FILES "tokenizer.bin" "../../assets/sounds/sounds.bin"
)
