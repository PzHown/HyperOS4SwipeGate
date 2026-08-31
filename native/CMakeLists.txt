cmake_minimum_required(VERSION 3.22.1)
project(hyperos4swipegate LANGUAGES C CXX ASM)

add_library(
        hyperos4swipegate
        SHARED
        src/main.cpp
        src/swipe_semantic_resolver.cpp
        src/swipe_hook_entry.S
        src/back_break_probe.cpp
        src/config_reader.cpp
        src/log_filter.cpp
        src/control_channel.cpp
        src/broadcast_send_capture_hook.S
)

target_include_directories(hyperos4swipegate PRIVATE include)
target_compile_features(hyperos4swipegate PRIVATE cxx_std_20)
target_compile_options(hyperos4swipegate PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -fvisibility=hidden
        -ffunction-sections
        -fdata-sections
)

target_link_options(hyperos4swipegate PRIVATE
        "-Wl,--wrap=__system_property_get"
        "-Wl,--wrap=dprintf"
        "-Wl,--wrap=__android_log_write"
        "-Wl,-z,max-page-size=16384"
        "-Wl,--gc-sections"
)

find_library(log-lib log)
find_library(dl-lib dl)

target_link_libraries(hyperos4swipegate PRIVATE ${log-lib} ${dl-lib})
