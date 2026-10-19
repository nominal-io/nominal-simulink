cmake_minimum_required(VERSION 3.16)

# Read rather than repeated: simulink/VERSION also versions the toolbox and the block
# library, and two copies would drift.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/simulink/VERSION" NM_VERSION LIMIT_COUNT 1)

# On a Mac, the MEX has to match the architecture of the MATLAB that loads it, which isn't
# always the Mac's own. MATLAB runs natively on Apple silicon from R2023b, and earlier
# releases are Intel builds, which run under Rosetta. So build for the MATLAB we build
# against. Both settings have to come before project().
if(CMAKE_HOST_APPLE AND DEFINED Matlab_ROOT_DIR AND NOT CMAKE_OSX_ARCHITECTURES)
  if(EXISTS "${Matlab_ROOT_DIR}/bin/maca64")
    set(CMAKE_OSX_ARCHITECTURES arm64)
  elseif(EXISTS "${Matlab_ROOT_DIR}/bin/maci64")
    set(CMAKE_OSX_ARCHITECTURES x86_64)
  endif()
endif()

# Without a deployment target, the MEX loads only on the macOS version that built it or a
# later one. 11.0 is the first macOS for Apple silicon.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Oldest macOS the binaries load on")
endif()

project(nominal-simulink
        VERSION ${NM_VERSION}
        DESCRIPTION "Nominal streaming client for Simulink and real-time targets"
        LANGUAGES C)

# Desktop builds use the host compiler. For QNX Neutrino 7.1 (Simulink Real-Time
# R2021a-R2025b), pass -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-qnx710-x86_64.cmake.
# Antelope RTOS (R2026a+) has no toolchain file yet.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Extensions on, deliberately: strict -std=c99 hides POSIX declarations behind feature
# test macros, so nanosleep and clock_gettime would come through as implicit
# declarations. -std=gnu99 exposes them without sprinkling _POSIX_C_SOURCE through the
# sources, where a single misordered include would break it again.
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static by default. Simulink Real-Time packs .a files into the real-time
# application itself, which removes any need to install anything on the target.
option(BUILD_SHARED_LIBS "Build shared instead of static" OFF)

include(CTest)

add_subdirectory(cstream)
add_subdirectory(simulink)
