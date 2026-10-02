cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radio_socket_test)
target_sources(app PRIVATE src/main.c ../../../gateway/src/radio_socket.c
  ../../../lib/radio/sensor_frame.c)
target_include_directories(app PRIVATE ../../../gateway/src ../../../include)
