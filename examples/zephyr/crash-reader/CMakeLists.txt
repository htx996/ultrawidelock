# Reads the crash record the witness image burned to flash. Bench tool.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ultrawidelock_crash_reader)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ..)
