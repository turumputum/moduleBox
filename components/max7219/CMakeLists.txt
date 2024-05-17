idf_component_register(SRCS "max7219.c" "max7219_task.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mqtt stateConfig)
