// T15 红测试：FrameView 持有数据的生命周期必须独立于 LRU 缓存淘汰。
//
// 复现 work_report/06 §8.3 坑(5)：FrameView 曾用裸指针指向 BundleSource 的
// 帧缓存，缓存按 LRU 只留 24 帧。取第 hold 帧后再取 ≥25 个【其他】帧，
// hold 帧被挤出缓存、其缓冲被释放并被后续帧复用——此时旧实现里原来那个
// FrameView 读到的已是别的帧的数据（或悬垂）。
//
// 验收（06 §T15）：取第 N 帧后再取 ≥25 个其他帧，原 FrameView 数据逐位不变。
// 该测试在裸指针旧实现上必须失败；shared_ptr 所有权实现后必须通过。
#include "test_util.h"

#include <cmath>
#include <cstdio>
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

// 造 nf 帧、nn 节点的 bundle。第 f 帧节点 i 的位置编码为 f*1000+i，
// 这样每帧的数值在全 bundle 内唯一、可逐位对拍。
fs::path makeMultiBundle(int nn, int nf) {
    const fs::path dir = fs::temp_directory_path() / "viz_t15_bundle";
    fs::remove_all(dir);
    fs::create_directories(dir / "frames");

    std::ofstream cfg(dir / "scene.json");
    cfg << "{\"format\":\"superplace-viz/1\",\"benchmark\":\"t\",\"nodeCount\":" << nn
        << ",\"core\":{\"lx\":0,\"ly\":0,\"hx\":100000,\"hy\":100000},"
           "\"chip\":{\"lx\":0,\"ly\":0,\"hx\":100000,\"hy\":100000},\"frames\":[";
    for (int f = 0; f < nf; ++f) {
        if (f) cfg << ",";
        cfg << "{\"file\":\"frames/f" << f << ".bin\",\"stage\":\"mGP\",\"label\":\"f"
            << f << "\"}";
    }
    cfg << "]}";

    std::vector<float> wh(nn * 2, 1.f);
    writeFloats(dir / "geom.bin", wh);
    const std::vector<unsigned char> fl(nn, 0);
    std::ofstream ff(dir / "flags.bin", std::ios::binary);
    ff.write(reinterpret_cast<const char*>(fl.data()), nn);

    for (int f = 0; f < nf; ++f) {
        std::vector<float> pos(static_cast<size_t>(nn) * 2);
        for (int i = 0; i < nn; ++i) {
            pos[static_cast<size_t>(i)] = f * 1000.f + i;
            pos[static_cast<size_t>(nn + i)] = -(f * 1000.f + i);
        }
        writeFloats(dir / ("frames/f" + std::to_string(f) + ".bin"), pos);
    }
    return dir;
}

}  // namespace

void testFrameViewLifetime() {
    const int nn = 8, nf = 30, hold = 3;
    const fs::path dir = makeMultiBundle(nn, nf);
    std::string err;
    auto src = makeBundleSource(dir.string(), &err);
    CHECK_TRUE(static_cast<bool>(src));
    if (!src) return;

    FrameView fv;
    CHECK_TRUE(src->frame(hold, fv));

    // 先把"持有帧"的原始数据记下来（此时缓存未淘汰，读的是真数据）。
    std::vector<float> savedX(nn), savedY(nn);
    for (int i = 0; i < nn; ++i) {
        savedX[static_cast<size_t>(i)] = fv.x[i];
        savedY[static_cast<size_t>(i)] = fv.y[i];
    }

    // 取 ≥25 个【其他】帧，把 hold 帧挤出 24 帧 LRU 缓存。
    FrameView scratch;
    int taken = 0;
    for (int f = 0; f < nf && taken < 26; ++f) {
        if (f == hold) continue;
        CHECK_TRUE(src->frame(f, scratch));
        ++taken;
    }
    CHECK_EQ(taken, 26);

    // 核心断言：原来那个 FrameView 的数据必须逐位不变。
    // 旧裸指针实现下这里要么读到被复用的别帧数据（数值不符），要么悬垂崩溃。
    bool sane = fv.valid();
    CHECK_TRUE(sane);
    if (sane) {
        for (int i = 0; i < nn; ++i) {
            CHECK_NEAR(fv.x[i], savedX[static_cast<size_t>(i)], 1e-3f);
            CHECK_NEAR(fv.y[i], savedY[static_cast<size_t>(i)], 1e-3f);
        }
    }
}
