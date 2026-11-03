// ⑥ 密度无串帧无头诊断：对 newblue1 帧 3/20/30/61 本查看器现算密度，
// 与独立真值 JSON（maxNorm / nonzero_bins / top bin）逐项比对。
// 仅当环境变量 VIZ_REF_BUNDLE 与 VIZ_REF_JSON 都设置时才执行；否则 no-op，
// 保证 ctest 在 CI（只有合成样本）下保持绿。
#include "test_util.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "viz/density.h"
#include "viz/source.h"

using json = nlohmann::json;
using namespace viz;

static std::string envOr(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(d);
}

void testDensityReference() {
    const std::string bundle = envOr("VIZ_REF_BUNDLE", "");
    const std::string jpath = envOr("VIZ_REF_JSON", "");
    if (bundle.empty() || jpath.empty()) {
        // 无独立真值环境：跳过（非失败）。
        CHECK_TRUE(true);
        return;
    }

    std::string err;
    auto src = makeBundleSource(bundle, &err);
    CHECK_TRUE(static_cast<bool>(src));
    if (!src) return;

    std::ifstream jf(jpath);
    json ref;
    jf >> ref;

    DensityField density;
    const int frames[4] = {3, 20, 30, 61};
    for (int f : frames) {
        FrameView fv;
        CHECK_TRUE(src->frame(f, fv));
        if (!fv.valid()) continue;
        density.build(src->scene().chip, fv.x, fv.y, src->geometry(), 128, 128);

        const float gotMax = density.maxNorm();
        int gotNonzero = 0;
        int topRow = 0, topCol = 0;
        float topVal = -1.f;
        for (int r = 0; r < density.gridRows(); ++r)
            for (int c = 0; c < density.gridCols(); ++c) {
                const float v = density.field()[static_cast<size_t>(r * density.gridCols() + c)];
                if (v > 0.f) ++gotNonzero;
                if (v > topVal) { topVal = v; topRow = r; topCol = c; }
            }

        const json e = ref["frames"][std::to_string(f)];
        const float expMax = e["max_norm"].get<float>();
        const int expNonzero = e["nonzero_bins"].get<int>();
        const int expRow = e["top5"][0]["row"].get<int>();
        const int expCol = e["top5"][0]["col"].get<int>();

        // 主判据：串帧会导致 maxNorm 量级错误、nonzero 计数错位、top bin 位置漂移。
        std::printf("[density] f%d  maxNorm got=%.2f exp=%.2f  (相对差 %.1f%%)\n",
                    f, gotMax, expMax,
                    expMax > 0 ? 100.0f * std::fabs(gotMax - expMax) / expMax : 0.f);
        std::printf("[density] f%d  nonzero got=%d exp=%d   topbin got=(%d,%d) exp=(%d,%d)\n",
                    f, gotNonzero, expNonzero, topRow, topCol, expRow, expCol);

        // 容差：现算与独立脚本的归一化应一致；放宽到 5%。
        CHECK_TRUE(std::fabs(gotMax - expMax) / expMax < 0.05f);
        CHECK_TRUE(std::abs(gotNonzero - expNonzero) <= 128);  // ±一行内
        CHECK_TRUE(topRow == expRow && topCol == expCol);
    }
}
