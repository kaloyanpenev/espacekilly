# udp_client: joins the multicast group and parses the whole order stream.
# Links udp_wire (shared codec + matcher ground-truth header).
add_executable(udp_client main.cpp)
target_link_libraries(udp_client PRIVATE udp_wire)
