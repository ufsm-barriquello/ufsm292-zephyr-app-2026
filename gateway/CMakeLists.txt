#-------------------------------------------------------------------------------
# Zephyr Example Application
#
# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

# This workspace does not assume a Zephyr SDK is installed. Default to the
# system arm-none-eabi toolchain (gnuarmemb) so the SAM D21/R21 boards build
# out of the box. Still overridable via -DZEPHYR_TOOLCHAIN_VARIANT=... or the
# environment.
if(NOT DEFINED ZEPHYR_TOOLCHAIN_VARIANT AND NOT DEFINED ENV{ZEPHYR_TOOLCHAIN_VARIANT})
  set(ZEPHYR_TOOLCHAIN_VARIANT gnuarmemb)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(gateway LANGUAGES C)

target_sources(app PRIVATE src/main.c src/radio_socket.c)

# The code below locates the git index file for this repository and adds it as a dependency for
# the application VERSION file so that if the repo has a new commit added, even if no files in
# the build have changed, the application version file will be regenerated with the new git commit
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
    WORKING_DIRECTORY .
    OUTPUT_VARIABLE application_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE stderr
    RESULT_VARIABLE return_code
  )
  # If there is an error e.g. it is not a git repo, we just silently ignore it and continue
  # without a dependency, this will be the case with freestanding applications.
  if(NOT return_code)
    if(NOT "${stderr}" STREQUAL "")
      message(WARNING "Application build version git rev-parse warned: ${stderr}")
    endif()

    set_property(TARGET app_version_h PROPERTY APP_VERSION_DEPENDS ${application_git_dir}/index)
  endif()
endif()
