idf_component_register(SRCS "apds9960.c" "swiper.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mqtt stateConfig rgbHsv)
