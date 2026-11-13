#include "viz/gds_writer.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace viz {
namespace {

void appendBe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(v & 0xff));
}

void appendBe32(std::vector<uint8_t>& out, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    out.push_back(static_cast<uint8_t>((u >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((u >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((u >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(u & 0xff));
}

// 记录头：[2 字节总长][1 字节类型][1 字节数据类型][数据]，总长含头 4 字节。
void writeRecord(std::vector<uint8_t>& out, uint8_t type, uint8_t dt,
                 const void* data, size_t len) {
    appendBe16(out, static_cast<uint16_t>(4 + len));
    out.push_back(type);
    out.push_back(dt);
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}

void writeRecord(std::vector<uint8_t>& out, uint8_t type, uint8_t dt) {
    writeRecord(out, type, dt, nullptr, 0);
}

// 字符串记录：补齐到偶数长度（NUL 填充）。
void writeStringRecord(std::vector<uint8_t>& out, uint8_t type, const std::string& s) {
    std::string padded = s;
    if (padded.size() % 2 != 0) padded.push_back('\0');
    writeRecord(out, type, 0x06, padded.data(), padded.size());
}

int32_t dbu(double v) {
    // 1 坐标单位 = 1 µm、DBU = 1 nm：round(值 × 1000)。
    return static_cast<int32_t>(std::lround(v * 1000.0));
}

}  // namespace

std::array<uint8_t, 8> encodeGdsReal(double v) {
    std::array<uint8_t, 8> b{};
    if (v == 0.0) return b;
    const bool neg = v < 0.0;
    double x = std::fabs(v);

    const double fexp = std::log2(x) / 4.0;   // = log16(x)
    int exponent = static_cast<int>(std::ceil(fexp));
    if (fexp == static_cast<double>(exponent)) ++exponent;

    const double scale = std::pow(16.0, 14 - exponent);
    uint64_t mantissa = static_cast<uint64_t>(x * scale);
    // 56 位小数，超出截断与参考实现一致。
    mantissa &= (1ULL << 56) - 1;

    b[0] = static_cast<uint8_t>((neg ? 0x80 : 0x00) |
                                static_cast<uint8_t>(exponent + 64));
    b[1] = static_cast<uint8_t>((mantissa >> 48) & 0xff);
    b[2] = static_cast<uint8_t>((mantissa >> 40) & 0xff);
    b[3] = static_cast<uint8_t>((mantissa >> 32) & 0xff);
    b[4] = static_cast<uint8_t>((mantissa >> 24) & 0xff);
    b[5] = static_cast<uint8_t>((mantissa >> 16) & 0xff);
    b[6] = static_cast<uint8_t>((mantissa >> 8) & 0xff);
    b[7] = static_cast<uint8_t>(mantissa & 0xff);
    return b;
}

int gdsLayerForKind(uint8_t kind, uint8_t flags) {
    if (flags & F_NI) return 0;                 // 零面积固定终端跳过
    switch (static_cast<Kind>(kind)) {
        case Kind::Macro:  return 2;
        case Kind::Cell:   return 1;
        case Kind::Fixed:  return 3;
        case Kind::Filler: return 0;
        case Kind::Count:  break;
    }
    return 0;
}

std::string writeGdsFile(const std::string& outPath, const std::string& designName,
                         const NodeGeometry& geom, const float* px, const float* py,
                         const Rect& core, std::string* err) {
    if (!px || !py) {
        if (err) *err = "没有可导出的位置";
        return {};
    }
    if (geom.w.size() != geom.h.size() || geom.kind.size() != geom.w.size()) {
        if (err) *err = "几何数据不一致";
        return {};
    }
    const size_t n = geom.w.size();

    std::vector<uint8_t> out;
    out.reserve(n * 64 + 256);

    // HEADER：版本号 600（与常见 GDS 写出器一致），int16 大端。
    std::vector<uint8_t> ver;
    appendBe16(ver, 600);
    writeRecord(out, 0x00, 0x02, ver.data(), ver.size());

    // BGNLIB：最后修改 / 访问时间各 6 个 int16（年,月,日,时,分,秒），写零。
    const uint8_t time12[12] = {0};
    writeRecord(out, 0x01, 0x02, time12, sizeof(time12));

    writeStringRecord(out, 0x02, designName);   // LIBNAME

    // UNITS：(用户单位/DBU, DBU/米) = (1e-3, 1e-9)。
    const auto u1 = encodeGdsReal(1e-3);
    const auto u2 = encodeGdsReal(1e-9);
    uint8_t units[16];
    std::memcpy(units, u1.data(), 8);
    std::memcpy(units + 8, u2.data(), 8);
    writeRecord(out, 0x03, 0x05, units, sizeof(units));

    writeRecord(out, 0x06, 0x02, time12, sizeof(time12));   // BGNSTR
    writeStringRecord(out, 0x07, designName);               // STRNAME

    auto writeRect = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1, int layer) {
        writeRecord(out, 0x08, 0x00);                       // BOUNDARY
        std::vector<uint8_t> lb;
        appendBe16(lb, static_cast<uint16_t>(layer));
        writeRecord(out, 0x0D, 0x02, lb.data(), lb.size()); // LAYER
        const uint16_t dt = 0;
        std::vector<uint8_t> db;
        appendBe16(db, dt);
        writeRecord(out, 0x0E, 0x02, db.data(), db.size()); // DATATYPE
        // XY：5 点闭合矩形，坐标 int32 大端，逐点写入。
        const int32_t pts[10] = {x0, y0, x1, y0, x1, y1, x0, y1, x0, y0};
        std::vector<uint8_t> xy;
        xy.reserve(sizeof(pts));
        for (int32_t v : pts) appendBe32(xy, v);
        writeRecord(out, 0x10, 0x03, xy.data(), xy.size());  // XY（5 点闭合）
        writeRecord(out, 0x11, 0x00);                       // ENDEL
    };

    for (size_t i = 0; i < n; ++i) {
        const int layer = gdsLayerForKind(geom.kind[i], geom.flags[i]);
        if (layer == 0) continue;
        const float w = geom.w[i], h = geom.h[i];
        if (w <= 0.f || h <= 0.f) continue;
        const float cx = px[i], cy = py[i];
        const int32_t x0 = dbu(cx - w * 0.5f);
        const int32_t y0 = dbu(cy - h * 0.5f);
        const int32_t x1 = dbu(cx + w * 0.5f);
        const int32_t y1 = dbu(cy + h * 0.5f);
        writeRect(x0, y0, x1, y1, layer);
    }

    // Core 边界：图层 10。
    if (core.width() > 0.f && core.height() > 0.f) {
        writeRect(dbu(core.lx), dbu(core.ly), dbu(core.hx), dbu(core.hy), 10);
    }

    writeRecord(out, 0x12, 0x00);                       // ENDSTR
    writeRecord(out, 0x13, 0x00);                       // ENDLIB

    std::ofstream file(outPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (err) *err = "打不开 " + outPath;
        return {};
    }
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    if (!file) {
        if (err) *err = "写文件失败 " + outPath;
        return {};
    }
    return outPath;
}

}  // namespace viz
