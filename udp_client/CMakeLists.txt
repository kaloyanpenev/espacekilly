# udp_client: joins the multicast group and parses the whole order stream.
# Links the matcher library only for its public header (ground-truth order API).
add_executable(udp_client main.cpp)
target_link_libraries(udp_client PRIVATE udp_wire)
