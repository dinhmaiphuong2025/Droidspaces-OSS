cmake_minimum_required(VERSION 3.22.1)
project("ds_wl_server" LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_definitions(-DHAVE_CONFIG_H)

set(LIBFFI_SRCS
    libffi/src/closures.c
    libffi/src/debug.c
    libffi/src/java_raw_api.c
    libffi/src/prep_cif.c
    libffi/src/raw_api.c
    libffi/src/types.c
    libffi/src/aarch64/sysv.S
    libffi/src/aarch64/ffi.c
)

set(WAYLAND_SERVER_SRCS
    wayland-server/wayland-util.c
    wayland-server/wayland-os.c
    wayland-server/event-loop.c
    wayland-server/connection.c
    wayland-server/wayland-shm.c
    wayland-server/wayland-server.c
)

set(PROTOCOLS_SRCS
    protocols/wayland-protocol.c
    protocols/xdg-shell-protocol.c
    protocols/linux-dmabuf-v1-protocol.c
    protocols/viewporter-protocol.c
)

set(SERVER_SRCS
    server/ds_compositor.c
    server/ds_xdg_shell.c
    server/ds_dmabuf.c
    server/ds_output.c
    server/ds_seat.c
    server/ds_viewporter.c
    server/ds_presenter.c
    server/ds_server.c
    ds_jni.c
)

set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/libffi/include -I${CMAKE_CURRENT_SOURCE_DIR}/libffi/src/aarch64")

add_library(ds_wl_server SHARED
    ${LIBFFI_SRCS}
    ${WAYLAND_SERVER_SRCS}
    ${PROTOCOLS_SRCS}
    ${SERVER_SRCS}
)

target_include_directories(ds_wl_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libffi/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libffi/src/aarch64
    ${CMAKE_CURRENT_SOURCE_DIR}/wayland-server
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols
    ${CMAKE_CURRENT_SOURCE_DIR}/server
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ds_wl_server
    android
    log
    EGL
    GLESv2
)
