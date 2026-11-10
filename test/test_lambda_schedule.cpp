#include <limits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "gp/lambda_schedule.h"
#include "test_util.h"

int main() {
    sp::Config cfg;
    CHECK_NEAR(sp::hpwlReference(cfg, 1e8, false), 350000.0, 0.0);
    cfg.lambda_schedule = "relative";
    cfg.delta_hpwl_relative = .007;
    const double small = sp::hpwlReference(cfg, 5e7, false);
    const double large = sp::hpwlReference(cfg, 5e8, false);
    CHECK_NEAR(large, 10.0 * small, 1e-6);
    CHECK_EQ(sp::lambdaMultiplier(1e5, small), sp::lambdaMultiplier(1e6, large));
    CHECK_NEAR(sp::hpwlReference(cfg, 5e8, true), 350000.0, 0.0);
    CHECK_NEAR(sp::hpwlReference(cfg, 0.0, false), 350000.0, 0.0);
    cfg.delta_hpwl_relative = std::numeric_limits<double>::infinity();
    bool rejected = false;
    try { sp::hpwlReference(cfg, 1e8, false); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK_TRUE(rejected);

    // Exercise JSON validation, not only the schedule helper.
    char path[] = "/tmp/eplace-m3-config-XXXXXX";
    const int fd = mkstemp(path);
    CHECK_TRUE(fd >= 0);
    if (fd >= 0) {
        close(fd);
        for (const char* invalid : {"{\"lambda_schedule\":\"typo\"}",
                                   "{\"delta_hpwl_ref\":0}",
                                   "{\"delta_hpwl_relative\":-1}",
                                   "{\"delta_hpwl_relative\":1e999}"}) {
            { std::ofstream out(path); out << invalid; }
            bool bad = false;
            try { sp::Config{}.loadJson(path); }
            catch (const std::exception&) { bad = true; }
            CHECK_TRUE(bad);
        }
        std::filesystem::remove(path);
    }
    const sp::Config defaults;
    sp::Config jsonDefaults;
    jsonDefaults.loadJson((std::filesystem::path(SP_TEST_DATA_DIR).parent_path() /
                           "configs/default.json").string());
    CHECK_EQ(defaults.lambda_schedule, jsonDefaults.lambda_schedule);
    CHECK_EQ(defaults.delta_hpwl_relative, jsonDefaults.delta_hpwl_relative);
    CHECK_EQ(defaults.delta_hpwl_ref, jsonDefaults.delta_hpwl_ref);
    CHECK_EQ(defaults.cgp_target_overflow, jsonDefaults.cgp_target_overflow);
    return sptest::summary("test_lambda_schedule");
}
