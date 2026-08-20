# nRF52840 dongle as a USB-to-serial adapter. Bench tool, not a product.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ultrawidelock_uart_bridge)

target_sources(app PRIVATE src/main.c)
