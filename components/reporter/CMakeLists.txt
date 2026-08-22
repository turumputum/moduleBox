idf_component_register(SRCS "crosslink_expr.c" "reporter.c"
                    INCLUDE_DIRS "include"
                    REQUIRES stateConfig myCDC myMqtt tinyosc)

