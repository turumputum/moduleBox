idf_component_register(SRCS main.c aip.c
                    INCLUDE_DIRS "." 
                    REQUIRES boards freertos soc sdmmc driver fatfs)

target_sources(${COMPONENT_TARGET} PUBLIC
)