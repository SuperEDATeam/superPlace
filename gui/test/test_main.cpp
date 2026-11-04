// T13：测试入口。逐个调用各用例 TU 里的函数，失败计数非零即退出 1。
#include "test_util.h"

void testCamera();
void testKind();
void testBundle();
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

int main() {
    testCamera();
    testKind();
    testBundle();
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

    std::printf("viz_tests: %d 项检查, %d 失败\n", ::viztest::checks(),
                ::viztest::failures());
    return ::viztest::failures() ? 1 : 0;
}
