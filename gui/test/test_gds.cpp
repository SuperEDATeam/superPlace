// T34：GDSII excess-64 8 字节实数编码逐字节对照标准编码器（gdspy 同算法）。
#include "test_util.h"

#include <array>

#include "viz/gds_writer.h"

using namespace viz;

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
}
