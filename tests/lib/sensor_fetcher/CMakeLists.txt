cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sensor_fetcher)

target_include_directories(app PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/sensor_fetcher/include
)

target_sources(app PRIVATE 
    src/main.c
    ../../../lib/sensor_fetcher/src/sensor_fetcher.c
    ../../../lib/sensor_fetcher/src/cJSON.c
)
