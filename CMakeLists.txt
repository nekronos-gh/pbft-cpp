# =========================
# PBFT Compilation Setup
# =========================
cmake_minimum_required(VERSION 3.16)
project(PBFT LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fno-inline" CACHE STRING "" FORCE)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

# Dependencies
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(Salticidae REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(spdlog REQUIRED)
pkg_check_modules(LIBUV REQUIRED libuv)

# Select sources
add_library(pbft_core STATIC
  src/node.cc
  src/metrics.cc
)
add_library(pbft::core ALIAS pbft_core)

# Public headers
target_include_directories(pbft_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${SALTICIDAE_INCLUDE_DIRS}
)

# Link dependencies
target_link_libraries(pbft_core
  PUBLIC
    prometheus-cpp::core # For downstream use
    prometheus-cpp::pull
    spdlog::spdlog
    fmt::fmt
  PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SALTICIDAE_LIBRARIES}
    ${LIBUV_LIBRARIES}
)

# Install and export
install(TARGETS pbft_core
  EXPORT pbftTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT pbftTargets
  FILE pbftTargets.cmake
  NAMESPACE pbft::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pbft
)

# Include subdirectory with tests
add_subdirectory(test)
