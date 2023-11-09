#idf_component_register(SRCS "rotary_encoder.c" INCLUDE_DIRS include)

idf_component_register(SRCS "rotary_encoder.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_peripherals )
