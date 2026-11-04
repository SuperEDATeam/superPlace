// T10 网表（§3.9）：CSR 反查与暴力一致、偏移累积、坏文件拒绝不崩。
#include "test_util.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "viz/source.h"
#include "viz/nets.h"

using namespace viz;
namespace fs = std::filesystem;

namespace {

void writeInts(const fs::path& p, const std::vector<int>& v) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(int));
}
void writeFloats(const fs::path& p, const std::vector<float>& v) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
}

// 造 n 节点 + M 网表的 mini bundle（net k 挂 2 个引脚，引脚打乱）。
fs::path makeNetsBundle(int n, int M, bool corrupt = false) {
    const fs::path dir = fs::temp_directory_path() / "viz_test_nets";
    fs::remove_all(dir);
    fs::create_directories(dir / "frames");
    fs::create_directories(dir / "nets");

    std::ofstream cfg(dir / "scene.json");
    cfg << "{\"format\":\"superplace-viz/1\",\"nodeCount\":" << n
        << ",\"core\":{\"lx\":0,\"ly\":0,\"hx\":100,\"hy\":100},"
           "\"chip\":{\"lx\":0,\"ly\":0,\"hx\":100,\"hy\":100},"
           "\"nets\":{\"netCount\":" << M << ",\"pinCount\":" << (M * 2)
        << ",\"hasNames\":false,\"hasWeights\":false,\"offsetUpdates\":[]},"
           "\"frames\":[{\"file\":\"frames/f0000.bin\",\"stage\":\"m\",\"label\":\"a\"}]}";

    std::vector<float> wh(n * 2, 1.f);
    writeFloats(dir / "geom.bin", wh);
    std::ofstream(dir / "flags.bin").write(reinterpret_cast<const char*>(std::vector<unsigned char>(n, 0).data()), n);
    std::vector<float> pos(n * 2, 0.f);
    writeFloats(dir / "frames/f0000.bin", pos);

    int P = M * 2;
    std::vector<int> netStart(M + 1);
    for (int k = 0; k <= M; ++k) netStart[k] = k * 2;
    std::vector<int> netPins(P);
    for (int p = 0; p < P; ++p) netPins[p] = p;  // 恒等排列
    std::vector<int> pinNode(P);
    for (int p = 0; p < P; ++p) pinNode[p] = p % n;
    std::vector<float> pinOffset(P * 2, 0.f);
    for (int p = 0; p < P; ++p) { pinOffset[p*2]=1.f; pinOffset[p*2+1]=2.f; }

    writeInts(dir/"nets/net_start.bin", corrupt ? std::vector<int>{0, 5, 99} : netStart);
    writeInts(dir/"nets/net_pins.bin", netPins);
    writeInts(dir/"nets/pin_node.bin", pinNode);
    writeFloats(dir/"nets/pin_offset.bin", pinOffset);
    return dir;
}

}  // namespace

void testNets() {
    // 1) CSR 反查与暴力一致
    {
        auto dir = makeNetsBundle(4, 3);  // n=4, M=3, P=6
        std::string err;
        auto src = makeBundleSource(dir.string(), &err);
        CHECK_TRUE(static_cast<bool>(src));
        const NetList* nl = src->nets();
        CHECK_TRUE(nl != nullptr);
        if (!nl) return;
        CHECK_EQ(nl->netCount, 3);
        CHECK_EQ(nl->pinCount, 6);
        // pinNet: pin p -> net p/2
        for (int p = 0; p < 6; ++p) CHECK_EQ(nl->pinNet[p], p / 2);
        // node->pins 暴力一致：node j 挂所有 pinNode==j 的引脚
        for (int node = 0; node < 4; ++node) {
            int lo = nl->nodePinStart[node], hi = nl->nodePinStart[node+1];
            int count = 0;
            for (int p = 0; p < 6; ++p) if (nl->pinNode[p] == node) ++count;
            CHECK_EQ(hi - lo, count);
        }
    }
    // 2) 坏 net_start（netStart[M]!=P）-> 无网表不崩
    {
        auto dir = makeNetsBundle(4, 3, true);
        std::string err;
        auto src = makeBundleSource(dir.string(), &err);
        CHECK_TRUE(static_cast<bool>(src));
        if (src) CHECK_TRUE(src->nets() == nullptr);
    }
    // 3) 偏移累积：基准 + frame<=f 更新
    {
        NetList nl;
        nl.pinCount = 2;
        nl.pinOffset = {1.f, 2.f, 10.f, 20.f};  // pin0=(1,2) pin1=(10,20)
        nl.offsetUpdates.push_back({5, 1, "nets/u.bin"});
        auto dir = fs::temp_directory_path() / "viz_test_nets_off";
        fs::remove_all(dir); fs::create_directories(dir / "nets");
        // 12 字节记录：pin0 dx=9 dy=9
        std::ofstream out(dir/"nets/u.bin", std::ios::binary);
        int pin = 0; float dx=9.f, dy=9.f;
        out.write(reinterpret_cast<char*>(&pin),4);
        out.write(reinterpret_cast<char*>(&dx),4);
        out.write(reinterpret_cast<char*>(&dy),4);
        out.close();
        std::vector<float> off;
        // frame 3 < 5：无更新
        CHECK_TRUE(nl.pinOffsetForFrame(3, off, dir.string()));
        CHECK_TRUE(off[0]==1.f && off[1]==2.f && off[2]==10.f && off[3]==20.f);
        // frame 5 >= 5：pin0 更新
        CHECK_TRUE(nl.pinOffsetForFrame(5, off, dir.string()));
        CHECK_TRUE(off[0]==9.f && off[1]==9.f && off[2]==10.f && off[3]==20.f);
    }
}
