add_library(nano_consensus consensus.hpp consensus.cpp)

target_link_libraries(nano_consensus PUBLIC nano_lib)
