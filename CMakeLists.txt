cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(adb_zephyr)
target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
    src/main.c
    src/packet.c
    src/adb_shell.c
)
target_sources_ifdef(CONFIG_ADB_SYNC app PRIVATE src/adb_sync.c)

