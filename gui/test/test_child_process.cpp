#include "test_util.h"
#include "viz/child_process.h"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/wait.h>
#include <unistd.h>

void testChildProcess() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("viz-process-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const auto log = dir / "run.log";
    pid_t pid = -1;
    CHECK_EQ(viz::spawnLogged({}, log.string(), pid), EINVAL);
    CHECK_EQ(pid, -1);
    CHECK_TRUE(viz::spawnLogged({"/this-program-does-not-exist"}, log.string(), pid) != 0);
    CHECK_TRUE(viz::spawnLogged({"/bin/true"}, (dir / "missing/run.log").string(), pid) != 0);
    // The test executable itself alternates writes to stdout/stderr, proving shared offsets.
    CHECK_EQ(viz::spawnLogged({"/proc/self/exe", "--log-fixture", "space ; $ literal"}, log.string(), pid), 0);
    int status = 0;
    CHECK_EQ(waitpid(pid, &status, 0), pid);
    CHECK_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::ifstream in(log);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_STREQ(text, "stdout-one\nstderr-one\nstdout-two\nstderr-two\nspace ; $ literal\n");
    fs::remove_all(dir);
}
