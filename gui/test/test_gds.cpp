// T34：GDSII excess-64 8 字节实数编码逐字节对照标准编码器（gdspy 同算法）。
#include "test_util.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include "viz/gds_writer.h"
#include "viz/gds_reader.h"

using namespace viz;
namespace fs = std::filesystem;

void testGds() {
    // 参考编码器输出（excess-64，以 16 为底）：
    //   1e-3  -> 3e 41 89 37 4b c6 a7 f0
    //   1e-9  -> 39 44 b8 2f a0 9b 5a 54
    const std::array<uint8_t, 8> expect1{{0x3e, 0x41, 0x89, 0x37, 0x4b, 0xc6, 0xa7, 0xf0}};
    const std::array<uint8_t, 8> expect2{{0x39, 0x44, 0xb8, 0x2f, 0xa0, 0x9b, 0x5a, 0x54}};

    const auto u1 = encodeGdsReal(1e-3);
    const auto u2 = encodeGdsReal(1e-9);
    for (int i = 0; i < 8; ++i) CHECK_EQ(u1[i], expect1[i]);
    for (int i = 0; i < 8; ++i) CHECK_EQ(u2[i], expect2[i]);

    // 0 写成全零。
    const auto u0 = encodeGdsReal(0.0);
    for (int i = 0; i < 8; ++i) CHECK_EQ(u0[i], 0u);

    // D1：BGNLIB / BGNSTR 时间戳为两组各 6 个 int16 = 24 字节载荷，
    // 记录总长 = 4 + 24 = 28。写出最小文件后回读核对这两条记录。
    const fs::path gdsPath = fs::temp_directory_path() / "viz_test_gds_time.gds";
    NodeGeometry geom;
    geom.w = {1.f};
    geom.h = {1.f};
    geom.flags = {0};
    geom.kind = {static_cast<uint8_t>(Kind::Cell)};
    const float px[1] = {0.f};
    const float py[1] = {0.f};
    Rect emptyCore;
    std::string err;
    const std::string written = writeGdsFile(gdsPath.string(), "t1", geom,
                                             px, py, emptyCore, &err);
    CHECK_TRUE(!written.empty());

    std::ifstream in(gdsPath, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    CHECK_TRUE(buf.size() > 0);

    auto be16 = [](const uint8_t* p) {
        return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
    };
    bool bgnlib = false, bgnstr = false;
    for (size_t off = 0; off + 4 <= buf.size();) {
        const uint16_t len = be16(buf.data() + off);
        const uint8_t type = buf[off + 2];
        if (len < 4 || off + len > buf.size()) break;
        const size_t payload = len - 4;
        if (type == 0x01) {  // BGNLIB
            bgnlib = true;
            CHECK_EQ(len, 28);
            CHECK_EQ(payload, 24u);
        } else if (type == 0x05) {  // BGNSTR（标准记录代码）
            bgnstr = true;
            CHECK_EQ(len, 28);
            CHECK_EQ(payload, 24u);
        }
        off += len;
    }
    CHECK_TRUE(bgnlib);
    CHECK_TRUE(bgnstr);
    // Assert the full standard sequence, not a table copied from the writer.
    std::vector<uint8_t> actual;
    for (size_t off = 0; off + 4 <= buf.size();) {
        actual.push_back(buf[off + 2]);
        off += be16(buf.data() + off);
    }
    const std::vector<uint8_t> expected = {0x00,0x01,0x02,0x03,0x05,0x06,
        0x08,0x0d,0x0e,0x10,0x11,0x07,0x04};
    CHECK_TRUE(actual == expected);
    GdsInfo info;
    CHECK_TRUE(readGdsFile(gdsPath.string(), info, err));
    CHECK_EQ(info.layerCounts[1], 1u);
    CHECK_TRUE(info.library == "t1" && info.structure == "t1");
    CHECK_NEAR(info.userUnitMeters, 1e-6, 1e-15);
    CHECK_NEAR(info.databaseUnitMeters, 1e-9, 1e-18);
    CHECK_NEAR(info.lx, -0.5, 1e-6);
    auto badFile = [&](const std::vector<uint8_t>& bad) {
        { std::ofstream out(gdsPath, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(bad.data()), bad.size()); }
        CHECK_TRUE(!readGdsFile(gdsPath.string(), info, err));
        CHECK_TRUE(err.find("字节偏移") != std::string::npos);
    };
    auto truncated = buf; truncated.pop_back(); badFile(truncated);
    auto missingEnd = buf; missingEnd.resize(missingEnd.size()-8); badFile(missingEnd);
    auto badLength = buf; badLength[0] = 0xff; badLength[1] = 0xff; badFile(badLength);
    auto unknown = buf; unknown[2] = 0x77; badFile(unknown);
    auto oldCodes = buf;
    for (size_t off = 0; off + 4 <= oldCodes.size();) {
        if (oldCodes[off+2] == 0x05) oldCodes[off+2] = 0x06;
        off += be16(oldCodes.data()+off);
    }
    badFile(oldCodes);
    auto wrongType = buf; wrongType[3] = 6; badFile(wrongType);
    auto extra = buf; extra.insert(extra.end(), {0,4,4,0}); badFile(extra);
    auto badXy = buf;
    for (size_t off = 0; off + 4 <= badXy.size();) {
        if (badXy[off+2] == 0x10) { badXy[off+4+39] ^= 1; break; }
        off += be16(badXy.data()+off);
    }
    badFile(badXy);
    // All project layers, skipped filler/NI/zero-area nodes, negative coordinates.
    NodeGeometry layers;
    layers.w = {1, 2, 3, 1, 1, 0}; layers.h = {1, 2, 3, 1, 1, 0};
    layers.flags = {0, F_MACRO, F_FIXED, F_FILLER, F_NI, 0};
    for (auto flag : layers.flags) layers.kind.push_back(static_cast<uint8_t>(kindOf(flag)));
    const float lx[] = {-1, 2, 8, 20, 25, 30}, ly[] = {-2, 3, 9, 20, 25, 30};
    const Rect core = {-10, -10, 12, 12};
    CHECK_TRUE(!writeGdsFile(gdsPath.string(), "layers", layers, lx, ly, core, &err).empty());
    CHECK_TRUE(readGdsFile(gdsPath.string(), info, err));
    for (int layer : {1, 2, 3, 10}) CHECK_EQ(info.layerCounts[layer], 1u);
    CHECK_EQ(info.layerCounts.size(), 4u);
    CHECK_NEAR(info.ly, -10, 1e-6);
    CHECK_NEAR(info.hx, 12, 1e-6);
    const auto goodSize = fs::file_size(gdsPath);
    auto shortFlags = layers; shortFlags.flags.clear();
    CHECK_TRUE(writeGdsFile(gdsPath.string(), "bad", shortFlags, lx, ly, core, &err).empty());
    CHECK_EQ(fs::file_size(gdsPath), goodSize);
    float invalidX[] = {std::numeric_limits<float>::quiet_NaN(), 2,8,20,25,30};
    CHECK_TRUE(writeGdsFile(gdsPath.string(), "bad", layers, invalidX, ly, core, &err).empty());
    invalidX[0] = 1e20f;
    CHECK_TRUE(writeGdsFile(gdsPath.string(), "bad", layers, invalidX, ly, core, &err).empty());
    CHECK_EQ(fs::file_size(gdsPath), goodSize);
    CHECK_TRUE(writeGdsFile(gdsPath.string(), "", layers, lx, ly, core, &err).empty());
    CHECK_TRUE(writeGdsFile(gdsPath.string(), std::string(70000, 'x'), layers, lx, ly, core, &err).empty());
    CHECK_EQ(fs::file_size(gdsPath), goodSize);
    std::error_code rmEc;
    fs::remove(gdsPath, rmEc);
}
