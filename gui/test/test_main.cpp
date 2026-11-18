// T13：测试入口。逐个调用各用例 TU 里的函数，失败计数非零即退出 1。
#include "test_util.h"
#include <cstring>
#include <unistd.h>

void testCamera();
void testCull();
void testCullExtra();
void testKind();
void testBundle();
void testNets();
void testDisplacement();
void testSpatial();
void testDensity();
void testFrameMap();
void testFrameMapHonesty();
void testFrameMapLongIter();
void testFrameMapBlankStage();
void testFrameMapNonContiguous();
void testFrameMapRowIterBoundaries();
void testFrameMapIllegalNoFallback();
void testFrameMapApproxSkipsIllegal();
void testScale();
void testFrameViewLifetime();
void testPlayback();
void testPlaybackDegrade();
void testPlaybackDuplicateT();
void testPlaybackZeroT();
void testPlaybackFixes();
void testPrefetch();
void testPrefetchShutdown();
void testPrefetchLRU();
void testDensityReference();
void testDrawValidPolicy();
void testDensityCache();
void testDensityCacheBudget();
void testDensityCacheFault();
void testDensityCacheLifetime();
void testGds();
void testChildProcess();

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--log-fixture") == 0) {
        const char* out[] = {"stdout-one\n", "stderr-one\n", "stdout-two\n", "stderr-two\n"};
        for (int i = 0; i < 4; ++i) {
            const auto n = std::strlen(out[i]);
            if (write(i % 2 ? STDERR_FILENO : STDOUT_FILENO, out[i], n) != static_cast<ssize_t>(n)) return 2;
        }
        const auto n = std::strlen(argv[2]);
        if (write(STDOUT_FILENO, argv[2], n) != static_cast<ssize_t>(n)) return 2;
        if (write(STDOUT_FILENO, "\n", 1) != 1) return 2;
        return 0;
    }
    testCamera();
    testCull();
    testCullExtra();
    testKind();
    testBundle();
    testNets();
    testDisplacement();
    testSpatial();
    testDensity();
    testFrameMap();
    testFrameMapHonesty();
    testFrameMapLongIter();
    testFrameMapBlankStage();
    testFrameMapNonContiguous();
    testFrameMapRowIterBoundaries();
    testFrameMapIllegalNoFallback();
    testFrameMapApproxSkipsIllegal();
    testScale();
    testFrameViewLifetime();
    testPlayback();
    testPlaybackDegrade();
    testPlaybackDuplicateT();
    testPlaybackZeroT();
    testPlaybackFixes();
    testPrefetch();
    testPrefetchShutdown();
    testPrefetchLRU();
    testDensityReference();
    testDrawValidPolicy();
    testDensityCache();
    testDensityCacheBudget();
    testDensityCacheFault();
    testDensityCacheLifetime();
    testGds();
    testChildProcess();

    std::printf("viz_tests: %d 项检查, %d 失败\n", ::viztest::checks(),
                ::viztest::failures());
    return ::viztest::failures() ? 1 : 0;
}
