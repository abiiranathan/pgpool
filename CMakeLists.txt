cmake_minimum_required(VERSION 3.15)
project(pgpool VERSION 1.0.0 LANGUAGES C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(PGPOOL_BUILD_EXAMPLES "Build example programs" ON)
option(PGPOOL_BUILD_TESTS "Build test suite" ON)

# Find PostgreSQL libpq
find_package(PostgreSQL REQUIRED)

# Find threading library
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Define the library
add_library(pgpool
    src/pgpool.c
)

# Set library properties
set_target_properties(pgpool PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER include/pgpool.h
)

# Include directories
target_include_directories(pgpool
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PostgreSQL_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(pgpool
    INTERFACE
        ${PostgreSQL_LIBRARIES}
        Threads::Threads
)

# Compiler-specific flags
if(MSVC)
    target_compile_options(pgpool PRIVATE /W4)
    target_compile_definitions(pgpool PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(pgpool PRIVATE -Wall -Wextra -pedantic)
endif()

# Platform-specific configurations
if(WIN32)
    target_link_libraries(pgpool PRIVATE ws2_32)
endif()

# Examples
if(PGPOOL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
if(PGPOOL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS pgpool
    EXPORT pgpoolTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install export targets
install(EXPORT pgpoolTargets
    FILE pgpoolTargets.cmake
    NAMESPACE pgpool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pgpool
)

# Create and install package config files
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pgpoolConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/pgpoolConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pgpool
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/pgpoolConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/pgpoolConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/pgpoolConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pgpool
)

# pkg-config file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pgpool.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pgpool.pc
    @ONLY
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/pgpool.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# Print summary
message(STATUS "")
message(STATUS "pgpool Configuration Summary:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build shared libs: ${BUILD_SHARED_LIBS}")
message(STATUS "  Build examples: ${PGPOOL_BUILD_EXAMPLES}")
message(STATUS "  Build tests: ${PGPOOL_BUILD_TESTS}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  PostgreSQL include: ${PostgreSQL_INCLUDE_DIRS}")
message(STATUS "  PostgreSQL libraries: ${PostgreSQL_LIBRARIES}")
message(STATUS "")
