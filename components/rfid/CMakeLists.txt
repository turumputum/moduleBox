idf_component_register(SRCS "pn532_uart.c" "rfid.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mqtt stateConfig)
