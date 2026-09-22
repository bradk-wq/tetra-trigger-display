cmake_minimum_required(VERSION 3.20)
project(tetra VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "Drive a Mobile Pixels Tetra over USB-A")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(TETRA_IS_TOP_LEVEL ON)
else()
    set(TETRA_IS_TOP_LEVEL OFF)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(TETRA_BUILD_EXAMPLES "Build the example programs" ${TETRA_IS_TOP_LEVEL})
option(TETRA_INSTALL "Generate install and package rules" ${TETRA_IS_TOP_LEVEL})

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
# The encoder uses the TurboJPEG 3 API (tj3*), which 2.x does not provide.
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg>=3.0)

add_library(tetra
    src/device.cpp
    src/display.cpp
    src/encoder.cpp
    src/surface.cpp)
add_library(tetra::tetra ALIAS tetra)

set_target_properties(tetra PROPERTIES
    VERSION ${PROJECT_VERSION}
    EXPORT_NAME tetra)

include(GNUInstallDirs)

target_include_directories(tetra PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(tetra PUBLIC cxx_std_17)
target_link_libraries(tetra PRIVATE PkgConfig::LIBUSB PkgConfig::TURBOJPEG)
target_compile_options(tetra PRIVATE -Wall -Wextra)

if(TETRA_BUILD_EXAMPLES)
    add_library(tetra_examples_support STATIC
        examples/app.cpp
        examples/draw.cpp
        examples/image.cpp)
    target_include_directories(tetra_examples_support PUBLIC examples)
    target_link_libraries(tetra_examples_support
        PUBLIC tetra::tetra
        PRIVATE PkgConfig::TURBOJPEG)
    target_compile_options(tetra_examples_support PRIVATE -Wall -Wextra)

    foreach(example testcard gallery bounce)
        add_executable(${example} examples/${example}.cpp)
        target_link_libraries(${example} PRIVATE tetra_examples_support)
        target_compile_options(${example} PRIVATE -Wall -Wextra)
    endforeach()

    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
    if(SDL2_FOUND)
        add_executable(sdl_mirror examples/sdl_mirror.cpp)
        target_link_libraries(sdl_mirror PRIVATE tetra_examples_support
            PkgConfig::SDL2)
        target_compile_options(sdl_mirror PRIVATE -Wall -Wextra)
    else()
        message(STATUS "SDL2 not found, skipping the sdl_mirror example")
    endif()
endif()

include(CMakePackageConfigHelpers)

# Build-tree export, so a consumer can point find_package straight at this
# directory's build without installing anything:
#
#   find_package(tetra REQUIRED PATHS /path/to/tetra-trigger-display/build)
#
# export(TARGETS) rather than export(EXPORT), so this does not depend on the
# install rules below having been generated.
export(TARGETS tetra
    FILE ${CMAKE_CURRENT_BINARY_DIR}/tetraTargets.cmake
    NAMESPACE tetra::)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tetraConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/tetraConfig.cmake
    INSTALL_DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/tetraConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

if(TETRA_INSTALL)
    set(TETRA_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/tetra)

    install(TARGETS tetra
        EXPORT tetraTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY include/tetra
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT tetraTargets
        FILE tetraTargets.cmake
        NAMESPACE tetra::
        DESTINATION ${TETRA_CMAKE_DIR})

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tetraConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/install/tetraConfig.cmake
        INSTALL_DESTINATION ${TETRA_CMAKE_DIR})

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/install/tetraConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/tetraConfigVersion.cmake
        DESTINATION ${TETRA_CMAKE_DIR})
endif()
