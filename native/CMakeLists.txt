cmake_minimum_required(VERSION 3.22.1)
project(hyperos4swipegate_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(hyperos4swipegate SHARED
    src/main.cpp
)

target_include_directories(hyperos4swipegate PRIVATE include)

target_compile_options(hyperos4swipegate PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

target_link_options(hyperos4swipegate PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
)

find_library(log-lib log)
find_library(dl-lib dl)

target_link_libraries(hyperos4swipegate PRIVATE ${log-lib} ${dl-lib})
