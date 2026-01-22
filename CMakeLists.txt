cmake_minimum_required(VERSION 3.15)
project(pgpool VERSION 1.0.0 LANGUAGES C)

# ------------------------------------------------------------
# Standards
# ------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ------------------------------------------------------------
# Options
# ------------------------------------------------------------
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(PGPOOL_BUILD_EXAMPLES "Build example programs" ON)
option(PGPOOL_BUILD_TESTS "Build test suite" ON)

# ------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------
find_package(PostgreSQL REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ------------------------------------------------------------
# Library
# ------------------------------------------------------------
add_library(pgpool src/pgpool.c src/pgtypes.c)

if(WIN32)
    target_sources(pgpool PRIVATE src/win_strptime.c)
endif()

add_library(pgpool::pgpool ALIAS pgpool)

# Export symbols on Windows for shared libs
if(WIN32 AND BUILD_SHARED_LIBS)
    set_target_properties(pgpool PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

set_target_properties(pgpool PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

# ------------------------------------------------------------
# Include directories
# ------------------------------------------------------------
target_include_directories(pgpool
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${PostgreSQL_INCLUDE_DIRS}
)

# ------------------------------------------------------------
# Link libraries
# ------------------------------------------------------------
target_link_libraries(pgpool
    PUBLIC
        ${PostgreSQL_LIBRARIES}
        Threads::Threads
)

if(WIN32)
    target_link_libraries(pgpool PRIVATE ws2_32)
endif()

# ------------------------------------------------------------
# Warnings
# ------------------------------------------------------------
if(MSVC)
    target_compile_options(pgpool PRIVATE /W4)
    target_compile_definitions(pgpool PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(pgpool PRIVATE -Wall -Wextra -pedantic)

    # Enables strptime posix function.
    target_compile_definitions(pgpool PRIVATE _XOPEN_SOURCE=700)
endif()

# ------------------------------------------------------------
# Examples
# ------------------------------------------------------------
if(PGPOOL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ------------------------------------------------------------
# Tests
# ------------------------------------------------------------
if(PGPOOL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ------------------------------------------------------------
# Installation
# ------------------------------------------------------------
include(GNUInstallDirs)

# ---- Headers go into include/pgpool/ ----
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pgpool
    FILES_MATCHING PATTERN "*.h"
)

# ---- Library ----
install(TARGETS pgpool
    EXPORT pgpoolTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # Windows DLLs
)

# ---- CMake package config ----
install(EXPORT pgpoolTargets
    FILE pgpoolTargets.cmake
    NAMESPACE pgpool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pgpool
)

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

# ------------------------------------------------------------
# pkg-config
# ------------------------------------------------------------

# On Windows (MSYS/MinGW), pkg-config usually lives in share/pkgconfig
if(WIN32)
    set(PGPOOL_PKGCONFIG_DIR ${CMAKE_INSTALL_DATAROOTDIR}/pkgconfig)
else()
    set(PGPOOL_PKGCONFIG_DIR ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pgpool.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pgpool.pc
    @ONLY
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/pgpool.pc
    DESTINATION ${PGPOOL_PKGCONFIG_DIR}
)

# ------------------------------------------------------------
# Summary
# ------------------------------------------------------------
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
message(STATUS "  pkg-config dir: ${PGPOOL_PKGCONFIG_DIR}")
message(STATUS "")
