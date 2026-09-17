set(SRCS main.c director.c brightness.c batlog.c codec_port.c advisor_llm_esp.c battery_port_axp2101.c imu_port_qmi8658.c)
if(CONFIG_POCKET_TANK_DISPLAY_CO5300)
    list(APPEND SRCS display_port_co5300.c)
else()
    list(APPEND SRCS display_port_stub.c)
endif()
if(CONFIG_POCKET_TANK_TOUCH_CST9217)
    list(APPEND SRCS touch_port_cst9217.c)
else()
    list(APPEND SRCS touch_port_stub.c)
endif()
if(CONFIG_POCKET_TANK_AUDIO_ES8311)
    list(APPEND SRCS audio_port_es8311.c)
else()
    list(APPEND SRCS audio_port_stub.c)
endif()
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES tankcore llm esp_partition spi_flash esp_timer esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_usb_serial_jtag esp_lcd nvs_flash esp_app_format
    EMBED_FILES "tokenizer.bin" "../../assets/sounds/sounds.bin"
)
