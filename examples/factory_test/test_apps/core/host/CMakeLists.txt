cmake_minimum_required(VERSION 3.16)
project(factory_core_host_tests LANGUAGES CXX)

enable_testing()

add_executable(
    factory_core_host_tests
    test_factory_core_host.cpp
    ../../../main/factory_core.cpp
)
target_include_directories(
    factory_core_host_tests PRIVATE
    ../common
    ../../../main
)
target_compile_features(factory_core_host_tests PRIVATE cxx_std_17)

add_test(NAME factory_core_host_tests COMMAND factory_core_host_tests)
