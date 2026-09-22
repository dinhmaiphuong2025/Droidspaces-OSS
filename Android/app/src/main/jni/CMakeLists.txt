cmake_minimum_required(VERSION 3.22.1)
project("ds_wl_server" LANGUAGES C)

add_library(ds_wl_server SHARED
    ds_wl_sock.c
    ds_wl_consumer.c
    ds_wl_jni.c
)

target_include_directories(ds_wl_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ds_wl_server
    android
    log
)
