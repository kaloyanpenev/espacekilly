# udp_broadcaster: sends the CreateMarket() order stream over UDP multicast.
# Links udp_wire (which brings the shared codec + the matcher ground-truth header).
add_executable(udp_broadcaster main.cpp)
target_link_libraries(udp_broadcaster PRIVATE udp_wire)
