cmake_minimum_required(VERSION 3.22.1)

project("ds_wayland_consumer")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(
    ds_wayland_consumer
    SHARED
    ds_wayland_jni.c
    display_consumer.c
    socket_utils.c
)

target_include_directories(
    ds_wayland_consumer
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(
    ds_wayland_consumer
    ${log-lib}
    ${android-lib}
)
