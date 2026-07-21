# udp_broadcaster: sends the CreateMarket() order stream over UDP multicast.
# Links the matcher library purely to consume its public header (the order API
# in <matcher/matcher.h> is the ground truth); no matcher symbols are called.
add_executable(udp_broadcaster main.cpp)
target_link_libraries(udp_broadcaster PRIVATE udp_wire)
