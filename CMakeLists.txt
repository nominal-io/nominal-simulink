cmake_minimum_required(VERSION 3.16)

project(nominal-simulink
        VERSION 0.1.0
        DESCRIPTION "Nominal streaming client for Simulink and real-time targets"
        LANGUAGES C)

# One definition, three toolchains: the host compiler for desktop, qcc for QNX
# Neutrino 7.1 (Simulink Real-Time R2021a-R2025b), and Speedgoat's Linux
# cross-compiler for Antelope RTOS (R2026a+). Select with -DCMAKE_TOOLCHAIN_FILE.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static by default. Simulink Real-Time packs .a files into the real-time
# application itself, which removes any need to install anything on the target.
option(BUILD_SHARED_LIBS "Build shared instead of static" OFF)

include(CTest)

add_subdirectory(cstream)
