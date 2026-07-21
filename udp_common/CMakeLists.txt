# udp_wire: header-only wire codec shared by the broadcaster and client.
# INTERFACE library = no compiled sources; it just propagates its include dir
# and the matcher link (for the ground-truth order API in <matcher/matcher.h>).
add_library(udp_wire INTERFACE)
target_include_directories(udp_wire INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(udp_wire INTERFACE matcher)
