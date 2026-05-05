cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
set(PICO_BOARD pico_w)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# FreeRTOS-Kernel CMake integration for RP2040.
set(FREERTOS_PORT GCC_RP2040 CACHE STRING "FreeRTOS port")
set(FREERTOS_HEAP 4 CACHE STRING "FreeRTOS heap implementation")

project(ws2812test C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

add_subdirectory(FreeRTOS-Kernel)

add_executable(ws2812test main.c)

pico_generate_pio_header(ws2812test ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_link_libraries(ws2812test
    pico_stdlib
    pico_cyw43_arch_none
    hardware_pio
    hardware_uart
    hardware_i2c
    freertos_kernel
    m
)

target_compile_definitions(ws2812test PRIVATE
    PICO_DEFAULT_UART_INSTANCE=uart1
    PICO_DEFAULT_UART_TX_PIN=8
    PICO_DEFAULT_UART_RX_PIN=9
)

pico_enable_stdio_usb(ws2812test 0)
pico_enable_stdio_uart(ws2812test 1)
pico_add_extra_outputs(ws2812test)
