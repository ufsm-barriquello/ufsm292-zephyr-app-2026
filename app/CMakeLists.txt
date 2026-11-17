# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ufsm292_zephyr_app)

target_sources(app PRIVATE src/main.c)

# Inclusao do DataLogger
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/datalogger lib_datalogger)
