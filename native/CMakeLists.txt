cmake_minimum_required(VERSION 3.18)
project(qq_native_emoji LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hide symbols by default — only zygisk_module_entry should be exported
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_library(qqemoji SHARED
    src/module.cpp
)

target_include_directories(qqemoji PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(qqemoji
    log
    dl
)
