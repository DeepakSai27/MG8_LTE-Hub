cmake_minimum_required(VERSION 3.20.0)

# The MG8 board definition is shared with the bring-up firmware one level up
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mg8_hub)

target_sources(app PRIVATE
	src/main.c
	src/modem.c
	src/clock.c
	src/tag_format.c
	src/tag_client.c
	src/upload.c
)
