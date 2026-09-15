idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs wear_levelling esp_partition app_update esp_system)
