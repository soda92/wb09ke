# CMakeLists.txt for Zephyr port of WB09_Blinky iBeacon
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wb09_ibeacon)

target_sources(app PRIVATE src/main.c)
