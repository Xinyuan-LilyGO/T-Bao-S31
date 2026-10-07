idf_component_register(
    SRCS
        test_factory_core.cpp
        ../../../main/factory_core.cpp
    INCLUDE_DIRS
        .
        ../common
        ../../../main
    PRIV_REQUIRES unity
)
