# XRay instrumentation configuration
# Clang XRay inserts lightweight probes at function entry/exit for precise tracing

if(NOT ENABLE_XRAY)
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
  message(FATAL_ERROR "XRay instrumentation is only supported by Clang")
endif()

add_compile_options(-fxray-instrument -fxray-instruction-threshold=1)
add_link_options(-fxray-instrument)

message(STATUS "XRay instrumentation enabled")
