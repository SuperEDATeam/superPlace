// T13：BundleSource——正常加载、尺寸不符/缺文件报错而非崩溃、LRU 淘汰后重取正确、
// summary.json 健壮解析（缺文件 null、损坏 null、正常解析）。
#include "test_util.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "viz/source.h"

using namespace viz;
namespace fs = std::filesystem;

namespace {

void writeFloats(const fs::path& p, const std::vector<float>& v) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(v.data()),
              static_cast<std::streamsize>(v.size() * sizeof(float)));
}

void writeBytes(const fs::path& p, const std::vector<unsigned char>& v) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(v.data()),
              static_cast<std::streamsize>(v.size()));
}

// 造一个最小合法 bundle（n 个节点、1 帧），可选覆盖 summary.json 内容。
fs::path makeMiniBundle(int n, const char* summaryJson, bool writeGeom = true,
                        bool hasMetrics = false) {
    const fs::path dir = fs::temp_directory_path() / "viz_test_bundle";
    fs::remove_all(dir);
    fs::create_directories(dir / "frames");

    std::ofstream cfg(dir / "scene.json");
    cfg << "{\"format\":\"superplace-viz/1\",\"benchmark\":\"t\",\"nodeCount\":" << n
        << ",\"hasMetrics\":" << (hasMetrics ? "true" : "false")
        << ",\"core\":{\"lx\":0,\"ly\":0,\"hx\":100,\"hy\":100},"
           "\"chip\":{\"lx\":0,\"ly\":0,\"hx\":100,\"hy\":100},"
           "\"frames\":[{\"file\":\"frames/f0000.bin\",\"stage\":\"mGP\",\"label\":\"mGP iter 0\"}]}";

    if (writeGeom) {
        std::vector<float> wh(n * 2, 1.f);
        writeFloats(dir / "geom.bin", wh);
        writeBytes(dir / "flags.bin", std::vector<unsigned char>(n, 0));
        std::vector<float> pos(n * 2, 0.f);
        for (int i = 0; i < n; ++i) {
            pos[i] = static_cast<float>(i);
            pos[n + i] = static_cast<float>(i) * 0.5f;
        }
        writeFloats(dir / "frames/f0000.bin", pos);
    }
    if (summaryJson) {
        std::ofstream out(dir / "summary.json");
        out << summaryJson;
    }
    return dir;
}

}  // namespace

void testBundle() {
    // 1) 仓库自带合成样本正常加载
    {
        std::string err;
        auto src = makeBundleSource(SYNTHETIC_BUNDLE, &err);
        CHECK_TRUE(static_cast<bool>(src));
        if (src) {
            CHECK_EQ(src->scene().nodeCount, 14681);
            CHECK_EQ(src->frameCount(), 40);
            CHECK_TRUE(src->metrics() != nullptr);
            CHECK_TRUE(src->summary() == nullptr);   // 合成样本无 summary.json
            FrameView fv;
            CHECK_TRUE(src->frame(0, fv));
            CHECK_TRUE(src->frame(39, fv));
            CHECK_TRUE(!src->frame(-1, fv));
            CHECK_TRUE(!src->frame(40, fv));
        }
    }

    // 2) nodeCount 与 geom.bin 大小不符 → 报错，不崩溃
    {
        const fs::path dir = makeMiniBundle(10, nullptr, false);
        // 故意写不足的 geom.bin（只给 4 个 float，应为 20 个）
        writeFloats(dir / "geom.bin", {1.f, 1.f, 1.f, 1.f});
        writeBytes(dir / "flags.bin", std::vector<unsigned char>(10, 0));
        std::string err;
        auto src = makeBundleSource(dir.string(), &err);
        CHECK_TRUE(!src);
        CHECK_TRUE(!err.empty());
    }

    // 3) 缺 geom.bin → 报错
    {
        const fs::path dir = makeMiniBundle(4, nullptr, false);
        std::string err;
        auto src = makeBundleSource(dir.string(), &err);
        CHECK_TRUE(!src);
    }

    // 4) LRU 淘汰后重取位置仍正确（合成样本 40 帧 > 缓存 24）
    {
        std::string err;
        auto src = makeBundleSource(SYNTHETIC_BUNDLE, &err);
        CHECK_TRUE(static_cast<bool>(src));
        if (src) {
            FrameView fv;
            src->frame(0, fv);
            const float x0 = fv.x[0], y0 = fv.y[0];
            for (int i = 0; i < src->frameCount(); ++i) src->frame(i, fv);  // 触发淘汰
            CHECK_TRUE(src->frame(0, fv));
            CHECK_NEAR(fv.x[0], x0, 1e-4f);
            CHECK_NEAR(fv.y[0], y0, 1e-4f);
        }
    }

    // 5a) 合法 summary.json 被解析
    {
        const fs::path dir = makeMiniBundle(
            4, "{\"gp_hpwl\":12345.6,\"cgp_macros_moved\":0,"
               "\"durations_ms\":{\"mGP\":42.5,\"cGP\":3.1}}");
        std::string err;
        auto src = makeBundleSource(dir.string(), &err);
        CHECK_TRUE(static_cast<bool>(src));
        if (src) {
            const SummaryTable* s = src->summary();
            CHECK_TRUE(s != nullptr);
            if (s) {
                const SummaryValue* v = s->find("gp_hpwl");
                CHECK_TRUE(v != nullptr && !v->isString);
                if (v) CHECK_NEAR(v->number, 12345.6, 1e-2);
                CHECK_EQ(static_cast<int>(s->durationsMs.size()), 2);
            }
        }
    }

    // 5b) 损坏 summary.json → null，不崩
    {
        const fs::path dir = makeMiniBundle(4, "{ this is not json ,,");
        std::string err;
        auto src = makeBundleSource(dir.string(), &err);
        CHECK_TRUE(static_cast<bool>(src));
        if (src) CHECK_TRUE(src->summary() == nullptr);
    }

    // 6) R2-2：缺 stage 名记为未知（空串），【不继承】上一阶段；维度一致、缺失为 NaN。
    {
        const fs::path dir = makeMiniBundle(4, nullptr, true, true);
        std::ofstream m(dir / "metrics.csv");
        m << "stage,iter,hpwl,overflow\n"
             "mGP,0,1000,0.9\n"
             ",1,999,\n"          // 空阶段 + 缺 overflow：不得继承 mGP，不补 0
             "mGP,2,998,0.7\n";
        m.close();
        std::string err;
        auto src = makeBundleSource(dir.string(), &err);
        CHECK_TRUE(static_cast<bool>(src));
        if (src && src->metrics()) {
            const MetricsTable& mt = *src->metrics();
            CHECK_EQ(mt.rowCount, 3);
            CHECK_EQ(mt.stage[0], "mGP");
            CHECK_EQ(mt.stage[1], "");          // 关键：空，而非继承 "mGP"
            CHECK_EQ(mt.stage[2], "mGP");
            CHECK_EQ(static_cast<int>(mt.values.size()), 4);
            for (const auto& col : mt.values) CHECK_EQ(static_cast<int>(col.size()), 3);
            const int ov = mt.columnIndex("overflow");
            CHECK_TRUE(ov >= 0);
            CHECK_TRUE(std::isnan(mt.values[static_cast<size_t>(ov)][1]));  // 缺列 NaN 非 0
        }
    }

    // 7) R2-2：无 stage 列的旧源 → 全部记为未知，不臆造分段。
    {
        const fs::path dir = makeMiniBundle(4, nullptr, true, true);
        std::ofstream m(dir / "metrics.csv");
        m << "iter,hpwl\n0,1000\n1,999\n";
        m.close();
        std::string err;
        auto src = makeBundleSource(dir.string(), &err);
        CHECK_TRUE(static_cast<bool>(src));
        if (src && src->metrics()) {
            const MetricsTable& mt = *src->metrics();
            CHECK_EQ(mt.rowCount, 2);
            CHECK_EQ(mt.stage[0], "");
            CHECK_EQ(mt.stage[1], "");
        }
    }
}
