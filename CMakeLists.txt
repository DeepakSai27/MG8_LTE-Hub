cmake_minimum_required(VERSION 3.20.0)

# The MG8 board definition lives in boards/arm/mg8_nrf52840
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mg8_bringup)

target_sources(app PRIVATE src/main.c src/flash_test.c src/ble_test.c)
