// viz bundle 写出器验证（work_report/06 §3）。
//
// 前端回放的正确性全部建立在 bundle 的几条约定上，这里逐条锁死：
//   1. 只写真实节点 [0, numNodes)，filler 不进 bundle
//   2. 与上一帧逐位相同的帧被跳过
//   3. 标志位在 open 时取一次——mLG 之后宏被置 F_FIXED，不能把它改写成固定终端
//   4. 帧文件是"先 x[N] 再 y[N]"的 float32，scene.json 字段齐全且 t 单调
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "db/place_db.h"
#include "io/viz_writer.h"
#include "nlohmann/json.hpp"
#include "test_util.h"

namespace fs = std::filesystem;

namespace {

/// 4 个真实节点（2 个标准单元、1 个可移动宏、1 个固定终端）+ 1 个 filler
sp::PlaceDB makeDb() {
    sp::PlaceDB db;
    db.addNode("c0", 2.f, 1.f, 0);
    db.addNode("c1", 3.f, 1.f, 0);
    db.addNode("m0", 6.f, 6.f, sp::F_MACRO);
    db.addNode("t0", 4.f, 4.f, sp::F_FIXED);
    db.addNode("filler", 1.f, 1.f, sp::F_FILLER);
    db.numMovable = 3;
    db.numNodes = 4;
    db.numFillers = 1;
    const float xs[] = {10.f, 20.f, 30.f, 40.f, 99.f};
    const float ys[] = {11.f, 21.f, 31.f, 41.f, 99.f};
    for (int i = 0; i < 5; ++i) {
        db.node_x[static_cast<size_t>(i)] = xs[i];
        db.node_y[static_cast<size_t>(i)] = ys[i];
    }
    db.rows.push_back(sp::PlaceDB::SiteRow{0.f, 1.f, 0.f, 1.f, 100});
    db.coreRegion = {0.f, 0.f, 100.f, 100.f};
    db.chipRegion = {-5.f, -5.f, 105.f, 105.f};
    db.rowHeight = 1.f;
    return db;
}

std::vector<float> readFloats(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    const auto bytes = static_cast<size_t>(in.tellg());
    std::vector<float> v(bytes / sizeof(float));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(bytes));
    return v;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "sp_test_viz_writer";
    const fs::path result = dir / "result";
    const fs::path bundle = result / "viz";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(result);
    { std::ofstream(result / "metrics.csv") << "stage,iter,hpwl\nmGP,0,1\n"; }

    sp::PlaceDB db = makeDb();
    sp::VizBundleWriter w;
    w.open(bundle.string(), "tiny", db, 10);

    // 模拟 mLG：宏被钉死。bundle 里的标志位必须仍是开跑前的 F_MACRO
    db.node_flags[2] |= sp::F_FIXED;

    w.append(db, {"input", -1, 0.0, false, "输入"});
    w.append(db, {"parse", -1, 0.0, true, "parse 结束"});   // 位置没动：应被跳过
    db.node_x[0] = 15.f;
    db.node_x[4] = 77.f;                                   // 只动 filler 不算变化
    w.append(db, {"mGP", 0, 0.0, false, "mGP iter 0"});
    db.node_x[4] = 55.f;                                   // 再只动 filler：应被跳过
    w.append(db, {"FILLERONLY", 0, 5.0, false, "fo"});
    db.node_x[2] = 35.f;
    w.append(db, {"mLG", -1, 5.0, true, "mLG"});
    w.finish(result.string());

    std::printf("  [dedup]   录 %d 帧，跳过重复 %d 帧\n", w.frameCount(), w.skippedDuplicates());
    CHECK_EQ(w.frameCount(), 3);
    CHECK_EQ(w.skippedDuplicates(), 2);

    // ---- 静态部分：只含 4 个真实节点
    CHECK_EQ(static_cast<long>(fs::file_size(bundle / "geom.bin")), 4L * 2 * 4);
    CHECK_EQ(static_cast<long>(fs::file_size(bundle / "flags.bin")), 4L);
    {
        std::ifstream in(bundle / "flags.bin", std::ios::binary);
        std::vector<char> f(4);
        in.read(f.data(), 4);
        std::printf("  [flags]   宏的标志位 = %d（open 时的 F_MACRO=%d，未被 mLG 后的 F_FIXED 污染）\n",
                    f[2], sp::F_MACRO);
        CHECK_EQ(static_cast<int>(f[2]), static_cast<int>(sp::F_MACRO));
    }
    {
        std::ifstream in(bundle / "names.txt");
        std::vector<std::string> names;
        for (std::string s; std::getline(in, s);) names.push_back(s);
        CHECK_EQ(static_cast<int>(names.size()), 4);
        CHECK_EQ(names[2], std::string("m0"));
    }

    // ---- 帧文件：先 x[4] 再 y[4]，不含 filler
    const std::vector<float> f1 = readFloats(bundle / "frames/f0001.bin");
    CHECK_EQ(static_cast<int>(f1.size()), 8);
    CHECK_NEAR(f1[0], 15.0, 0);   // x[0] 已移动
    CHECK_NEAR(f1[2], 30.0, 0);   // x[2] 宏尚未移动
    CHECK_NEAR(f1[4], 11.0, 0);   // y[0]
    const std::vector<float> f2 = readFloats(bundle / "frames/f0002.bin");
    CHECK_NEAR(f2[2], 35.0, 0);   // mLG 后宏的新位置

    // ---- scene.json
    nlohmann::json j;
    std::ifstream(bundle / "scene.json") >> j;
    CHECK_EQ(j["format"].get<std::string>(), std::string("superplace-viz/1"));
    CHECK_EQ(j["nodeCount"].get<int>(), 4);
    CHECK_EQ(j["benchmark"].get<std::string>(), std::string("tiny"));
    CHECK_TRUE(j["hasNames"].get<bool>());
    CHECK_TRUE(j["hasMetrics"].get<bool>());
    CHECK_EQ(static_cast<int>(j["frames"].size()), 3);
    CHECK_EQ(j["frames"][2]["stage"].get<std::string>(), std::string("mLG"));
    CHECK_TRUE(j["frames"][2]["breakBefore"].get<bool>());
    CHECK_TRUE(!j["frames"][1]["breakBefore"].get<bool>());
    CHECK_EQ(j["frames"][1]["iter"].get<int>(), 0);
    CHECK_TRUE(fs::exists(bundle / "metrics.csv"));
    CHECK_TRUE(!fs::exists(bundle / "scene.json.tmp"));   // 原子写的临时文件不能残留

    fs::remove_all(dir, ec);
    return sptest::summary("test_viz_writer");
}
