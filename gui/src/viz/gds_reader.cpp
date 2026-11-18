#include "viz/gds_reader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace viz {
namespace {
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
int32_t be32(const uint8_t* p) {
    const uint32_t value = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                           (uint32_t(p[2]) << 8) | p[3];
    return value <= uint32_t(INT32_MAX) ? static_cast<int32_t>(value)
        : static_cast<int32_t>(int64_t(value) - (int64_t(1) << 32));
}
double real8(const uint8_t* p) {
    uint64_t fraction = 0;
    for (int i = 1; i < 8; ++i) fraction = (fraction << 8) | p[i];
    const double sign = p[0] & 0x80 ? -1.0 : 1.0;
    return sign * std::ldexp(static_cast<double>(fraction), -56) *
           std::pow(16.0, int(p[0] & 0x7f) - 64);
}
std::string stringRecord(const uint8_t* p, std::size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
}  // namespace

bool readGdsFile(const std::string& path, GdsInfo& info, std::string& error) {
    info = {}; error.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { error = "打不开 " + path; return false; }
    const auto length = file.tellg();
    if (length < 0 || static_cast<uint64_t>(length) > 512ULL * 1024 * 1024) {
        error = "文件长度无效或超过 512 MiB"; return false;
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), length)) {
        error = "读取文件失败"; return false;
    }
    GdsInfo parsed;
    parsed.fileBytes = bytes.size();
    const std::array<uint8_t, 6> prefix{{0x00, 0x01, 0x02, 0x03, 0x05, 0x06}};
    std::size_t off = 0, count = 0;
    int elementStep = 0, layer = 0;
    bool endStructure = false, ended = false;
    auto fail = [&](const std::string& why, int type) {
        std::ostringstream out;
        out << "字节偏移 " << off << "，记录类型 ";
        if (type < 0) out << "缺失";
        else out << "0x" << std::hex << std::setw(2) << std::setfill('0') << type;
        out << "：" << why;
        error = out.str(); return false;
    };
    while (off < bytes.size()) {
        if (ended) return fail("ENDLIB 后有额外数据", bytes.size() - off >= 3 ? bytes[off + 2] : -1);
        if (bytes.size() - off < 4) return fail("记录头截断", -1);
        const auto* p = bytes.data() + off;
        const uint16_t len = be16(p);
        const uint8_t type = p[2], dt = p[3];
        if (len < 4 || len % 2 || len > bytes.size() - off) return fail("记录长度无效或越界", type);
        const std::size_t n = len - 4;
        const uint8_t* payload = p + 4;
        if (count < prefix.size()) {
            if (type != prefix[count]) return fail("库/结构记录序列不完整", type);
            switch (type) {
                case 0x00:
                    if (dt != 2 || n != 2) return fail("HEADER 类型或长度错误", type);
                    break;
                case 0x01: case 0x05:
                    if (dt != 2 || n != 24) return fail("时间戳应为24字节", type);
                    break;
                case 0x02: case 0x06:
                    if (dt != 6 || n == 0) return fail("名称类型或长度错误", type);
                    if (type == 0x02) parsed.library = stringRecord(payload, n);
                    else parsed.structure = stringRecord(payload, n);
                    break;
                case 0x03: {
                    if (dt != 5 || n != 16) return fail("UNITS 类型或长度错误", type);
                    const double ratio = real8(payload);
                    parsed.databaseUnitMeters = real8(payload + 8);
                    if (!(ratio > 0.0) || !(parsed.databaseUnitMeters > 0.0) ||
                        !std::isfinite(ratio) || !std::isfinite(parsed.databaseUnitMeters))
                        return fail("UNITS 必须为有限正数", type);
                    parsed.userUnitMeters = parsed.databaseUnitMeters / ratio;
                    if (!std::isfinite(parsed.userUnitMeters)) return fail("UNITS 比率无效", type);
                    break;
                }
            }
        } else if (endStructure) {
            if (type != 0x04 || dt != 0 || n != 0) return fail("缺少 ENDLIB", type);
            ended = true;
        } else if (elementStep == 0 && type == 0x07) {
            if (dt != 0 || n != 0) return fail("ENDSTR 类型或长度错误", type);
            endStructure = true;
        } else {
            static constexpr uint8_t element[] = {0x08, 0x0d, 0x0e, 0x10, 0x11};
            if (type != element[elementStep]) return fail("未知记录或 BOUNDARY 结构错误", type);
            if (elementStep == 0 || elementStep == 4) {
                if (dt != 0 || n != 0) return fail("无载荷记录类型错误", type);
            } else if (elementStep == 1 || elementStep == 2) {
                if (dt != 2 || n != 2) return fail("图层/数据类型记录错误", type);
                if (elementStep == 1) layer = be16(payload);
                else if (be16(payload) != 0) return fail("DATATYPE 只支持0", type);
            } else {
                if (dt != 3 || n != 40) return fail("XY 必须是五点 int32 矩形", type);
                int32_t x[5], y[5];
                for (int i = 0; i < 5; ++i) { x[i] = be32(payload + i * 8); y[i] = be32(payload + i * 8 + 4); }
                if (x[0] != x[4] || y[0] != y[4] || x[0] != x[3] || x[1] != x[2] ||
                    y[0] != y[1] || y[2] != y[3] || x[1] <= x[0] || y[2] <= y[0])
                    return fail("XY 不是正面积闭合矩形", type);
                const double factor = parsed.databaseUnitMeters * 1e6;
                const double lx = x[0] * factor, ly = y[0] * factor;
                const double hx = x[1] * factor, hy = y[2] * factor;
                if (!parsed.hasBounds) {
                    parsed.lx = lx; parsed.ly = ly; parsed.hx = hx; parsed.hy = hy; parsed.hasBounds = true;
                } else {
                    parsed.lx = std::min(parsed.lx, lx); parsed.ly = std::min(parsed.ly, ly);
                    parsed.hx = std::max(parsed.hx, hx); parsed.hy = std::max(parsed.hy, hy);
                }
                ++parsed.layerCounts[layer];
            }
            elementStep = (elementStep + 1) % 5;
        }
        ++count; off += len;
    }
    if (!ended || !endStructure || count < 8 || elementStep != 0) return fail("缺少 ENDSTR / ENDLIB 或文件截断", -1);
    parsed.records = count;
    info = std::move(parsed);
    return true;
}

}  // namespace viz
