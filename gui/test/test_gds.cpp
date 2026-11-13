// T34：GDSII excess-64 8 字节实数编码逐字节对照标准编码器（gdspy 同算法）。
#include "test_util.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "viz/gds_writer.h"

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
        } else if (type == 0x06) {  // BGNSTR
            bgnstr = true;
            CHECK_EQ(len, 28);
            CHECK_EQ(payload, 24u);
        }
        off += len;
    }
    CHECK_TRUE(bgnlib);
    CHECK_TRUE(bgnstr);
    std::error_code rmEc;
    fs::remove(gdsPath, rmEc);
}
