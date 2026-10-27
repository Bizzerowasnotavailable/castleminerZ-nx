cmake_minimum_required(VERSION 3.20)
project(castleminerz_tools VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(cmz_stfs
    source/stfs.cpp
    source/stfs_devoptab.cpp
    source/game_image.cpp
    source/package_search.cpp
)
target_include_directories(cmz_stfs PUBLIC include)

if(MSVC)
    target_compile_options(cmz_stfs PRIVATE /W4 /permissive-)
else()
    target_compile_options(cmz_stfs PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(cmztool tools/cmztool.cpp)
target_link_libraries(cmztool PRIVATE cmz_stfs)

include(CTest)
if(BUILD_TESTING)
    add_executable(stfs_tests tests/stfs_tests.cpp)
    target_link_libraries(stfs_tests PRIVATE cmz_stfs)
    add_test(NAME stfs_tests COMMAND stfs_tests)
endif()
