#pragma once

#include <string>
#include <vector>

namespace nano::weights
{
extern std::vector<std::pair<std::string, std::string>> preconfigured_weights_live;
extern uint64_t max_blocks_live;
extern std::vector<std::pair<std::string, std::string>> preconfigured_weights_beta;
extern uint64_t max_blocks_beta;
}
