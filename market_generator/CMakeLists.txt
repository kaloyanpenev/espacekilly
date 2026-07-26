# market_generator: generates the whole market as a std::vector<NewRequest>
# and will publish it into a shared mmap-ed region for ./exchange to consume.
add_executable(market_generator main.cpp)
target_link_libraries(market_generator PRIVATE matcher)