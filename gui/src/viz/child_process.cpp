#include "viz/child_process.h"

#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

extern char** environ;
namespace viz {
int spawnLogged(const std::vector<std::string>& arguments, const std::string& log, pid_t& pid) {
    pid = -1;
    if (arguments.empty() || arguments.front().empty()) return EINVAL;
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result) return result;
    result = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                            O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    if (!result) {
        std::vector<char*> argv;
        for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        result = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    return result;
}
}  // namespace viz
