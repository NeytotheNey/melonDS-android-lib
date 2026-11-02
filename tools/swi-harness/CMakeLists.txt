# Headless PC test harness that logs every SWI (BIOS call) at the SWI boundary.
# Builds the core from ../../src with MELONADE_SWI_LOG defined; the Android
# build never sets that define, so it is unchanged.
#
#   cmake -G Ninja -S tools/swi-harness -B <builddir> -DCMAKE_BUILD_TYPE=Release
#   cmake --build <builddir>
#
# -DSWI_HARNESS_JIT=ON also builds the x86-64 JIT (for --jit runs that mimic
# the phone, which runs the ARM64 JIT).

cmake_minimum_required(VERSION 3.16)

project(swi_harness LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

set(MELONDS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(CMAKE_MODULE_PATH "${MELONDS_ROOT}/cmake" ${CMAKE_MODULE_PATH})

# what src/CMakeLists.txt normally gets from the top-level project
set(melonDS_VERSION "1.1")
set(melonDS_HOMEPAGE_URL "https://melonds.kuribo64.net")
option(SWI_HARNESS_JIT "Build the JIT too (x86-64 only), selected at run time with --jit" OFF)
set(ENABLE_JIT ${SWI_HARNESS_JIT})
if (ENABLE_JIT)
    set(ARCHITECTURE x86_64)
    add_definitions(-DARCHITECTURE_x86_64=1)
endif()
set(ENABLE_OGLRENDERER OFF)
set(ENABLE_GDBSTUB OFF)

if (WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
    if (NOT MINGW)
        add_definitions(-D__WIN32__ -D_CRT_SECURE_NO_WARNINGS -D_USE_MATH_DEFINES)
    endif()
endif()

add_compile_definitions(MELONADE_SWI_LOG)

add_subdirectory("${MELONDS_ROOT}/src" "${CMAKE_BINARY_DIR}/core")

add_executable(swi-harness
    main.cpp
    SWILog.cpp
    HarnessLog.h)

target_include_directories(swi-harness PRIVATE "${MELONDS_ROOT}/src")
target_link_libraries(swi-harness PRIVATE core)

if (MINGW)
    target_link_options(swi-harness PRIVATE -static)
endif()
