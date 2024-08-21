idf_component_register(SRCS "rtp_play.c" "rtp_client_stream.c"
                    INCLUDE_DIRS "include"
                    REQUIRES audio_hal audio_stream audio_sal stateConfig reporter)
