# libnominalstream — the write path. No Simulink dependency; usable standalone.

add_library(nominalstream
  src/nominal_stream.c
  src/nm_encode.c
  src/nm_ring.c
  src/nm_http.c
  # Proxy mode. nm_frame is its own translation unit so a static link into a real-time
  # application drops the decoder as dead code -- a target only ever sends frames.
  src/nm_frame.c
  src/nm_udp.c
)

if(WIN32)
  # getaddrinfo and the socket calls in nm_udp.c
  target_link_libraries(nominalstream PUBLIC ws2_32)
endif()

# libcurl carries the HTTPS.
#
# Supply it deliberately rather than by accident: configure with the vcpkg toolchain
# (see README) so this resolves to a curl we chose. Without one, CMake may find
# whatever happens to be installed -- on this dev machine that was GStreamer's bundled
# copy, which is not something to ship against.
#
# Consuming curl through its CMake config matters for static builds: the imported
# target carries INTERFACE_COMPILE_DEFINITIONS "CURL_STATICLIB", so the define curl's
# INSTALL.md warns about is propagated for us rather than needing to be set by hand.
#
# On QNX and Antelope this finds the platform's curl instead, which is the whole reason
# the C design is tractable there -- curl.se publishes official QNX builds and OpenSSL
# ships with Neutrino.
find_package(CURL REQUIRED)
target_link_libraries(nominalstream PUBLIC CURL::libcurl)

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

add_executable(nmstream-demo examples/nmstream_demo.c)
target_link_libraries(nmstream-demo PRIVATE nominalstream)
if(NOT WIN32)
  target_link_libraries(nmstream-demo PRIVATE m)
endif()
if(BUILD_TESTING)
  add_executable(test_encode tests/test_encode.c)
  target_link_libraries(test_encode PRIVATE nominalstream)
  target_include_directories(test_encode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME encode
           COMMAND test_encode
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/body_identity.pb)

  add_executable(test_ring tests/test_ring.c)
  target_link_libraries(test_ring PRIVATE nominalstream)
  target_include_directories(test_ring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME ring COMMAND test_ring)

  add_executable(test_frame tests/test_frame.c)
  target_link_libraries(test_frame PRIVATE nominalstream)
  target_include_directories(test_frame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME frame COMMAND test_frame)

  add_executable(test_http tests/test_http.c)
  target_link_libraries(test_http PRIVATE nominalstream)
  target_include_directories(test_http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_http PRIVATE Threads::Threads)
  endif()
  add_test(NAME http COMMAND test_http)
endif()

# Next: the Simulink S-function, in a sibling simulink/ directory, then the QNX and
# Antelope cross-compiles.
