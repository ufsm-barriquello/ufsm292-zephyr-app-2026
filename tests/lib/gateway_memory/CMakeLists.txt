# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

set(MEMORY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/gateway_memory)
set(INC_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

if(BOARD MATCHES "^unit_testing")
  set(SOURCES
    src/main.c
    ${MEMORY_DIR}/memory_table.c
  )

  find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

  project(gateway_memory_test)

  target_include_directories(testbinary PRIVATE ${INC_DIR})
else()
  find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

  project(gateway_memory_test)

  target_sources(app PRIVATE
    src/main.c
  )

  target_include_directories(app PRIVATE ${INC_DIR})
endif()
