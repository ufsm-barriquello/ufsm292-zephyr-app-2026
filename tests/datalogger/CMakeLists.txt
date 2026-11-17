# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_datalogger)

target_sources(app PRIVATE src/main.c)

# Inclui os fontes do datalogger para teste
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/datalogger lib_datalogger)
