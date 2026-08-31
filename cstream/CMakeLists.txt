# libnominalstream — the write path. No Simulink dependency; usable standalone.

add_library(nominalstream
  src/nominal_stream.c
  src/nm_encode.c
)

target_include_directories(nominalstream
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Compression of the request body.
#
# Probed against the live server 2026-08-31: it returns 204 for `identity`, for no
# Content-Encoding header at all, for `zstd`, and for the pre-0.9.6 `x-snappy-framed`.
# So compression is optional, and OFF removes libzstd from the QNX cross-compile
# entirely. Turn it ON if payload size on a constrained link outweighs the CPU cost --
# the sample request compresses 177 bytes to 113.
option(NM_USE_ZSTD "Compress request bodies with zstd" OFF)
if(NM_USE_ZSTD)
  target_compile_definitions(nominalstream PRIVATE NM_USE_ZSTD=1)
endif()

if(NOT MSVC)
  target_compile_options(nominalstream PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The sender thread. pthreads on QNX Neutrino and Linux; the Windows desktop build
# uses the Win32 API directly rather than taking a pthreads-win32 dependency.
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(nominalstream PRIVATE Threads::Threads)
endif()

if(BUILD_TESTING)
  add_executable(test_encode tests/test_encode.c)
  target_link_libraries(test_encode PRIVATE nominalstream)
  target_include_directories(test_encode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME encode
           COMMAND test_encode
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/body_identity.pb)
endif()

# TODO, in order:
#   - ring buffer and sender thread
#   - libcurl for HTTP (official QNX builds exist; OpenSSL ships with Neutrino)
#   - optional libzstd, gated on NM_USE_ZSTD (server accepts identity, so off by default)
