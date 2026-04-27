cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
set(PICO_BOARD pico_w)

project(ws2812test C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(ws2812test main.c)

pico_generate_pio_header(ws2812test ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_link_libraries(ws2812test
    pico_stdlib
    pico_cyw43_arch_none
    hardware_pio
    hardware_uart
    m                      # sinf() 需要這個，原本沒加會 link error
)

pico_enable_stdio_usb(ws2812test 0)
pico_enable_stdio_uart(ws2812test 0)
pico_add_extra_outputs(ws2812test)