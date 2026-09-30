cmake_minimum_required(VERSION 3.20)
project(tetra_wireless_poc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The library, unmodified. As a subdirectory it skips its examples.
add_subdirectory(.. tetra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg>=3.0)
find_package(Threads REQUIRED)

foreach(tool receiver sender)
    add_executable(tetra_${tool} ${tool}.cpp)
    target_link_libraries(tetra_${tool} PRIVATE tetra::tetra
        PkgConfig::TURBOJPEG Threads::Threads)
    target_compile_options(tetra_${tool} PRIVATE -Wall -Wextra)
endforeach()

add_executable(test_reassembler test_reassembler.cpp)
target_compile_options(test_reassembler PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME reassembler COMMAND test_reassembler)
add_test(NAME loopback
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_loopback.sh
            $<TARGET_FILE:tetra_receiver> $<TARGET_FILE:tetra_sender>)
