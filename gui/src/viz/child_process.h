#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

namespace viz {
// Launch fixed argv, never a shell. stdout/stderr share one open file description.
int spawnLogged(const std::vector<std::string>& arguments, const std::string& log, pid_t& pid);
}  // namespace viz
