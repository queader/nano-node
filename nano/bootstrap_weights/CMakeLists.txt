add_library(
  nano_bootstrap_weights bootstrap_weights_beta.hpp bootstrap_weights_live.hpp
                         bootstrap_weights.cpp)

target_link_libraries(nano_bootstrap_weights nano_lib)
