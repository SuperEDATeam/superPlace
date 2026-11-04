#include "viz/nets.h"

#include <cstring>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace viz {

namespace {
std::vector<char> readAll(const std::string& path) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (ec) return {};
    std::ifstream in(path, std::ios::binary);
    std::vector<char> buf(sz);
    in.read(buf.data(), sz);
    return buf;
}
}  // namespace

bool NetList::pinOffsetForFrame(int f, std::vector<float>& out,
                                const std::string& bundleDir) const {
    out = pinOffset;  // 基准
    if (out.size() != static_cast<size_t>(pinCount) * 2) return false;
    // 累积所有 frame<=f 的更新
    for (const auto& u : offsetUpdates) {
        if (u.frame > f) break;
        auto buf = readAll(bundleDir + "/" + u.file);
        size_t expect = static_cast<size_t>(u.count) * 12;
        if (buf.size() != expect) return false;
        for (size_t i = 0; i < static_cast<size_t>(u.count); ++i) {
            int pin = 0;
            float dx = 0, dy = 0;
            std::memcpy(&pin, buf.data() + i * 12 + 0, 4);
            std::memcpy(&dx, buf.data() + i * 12 + 4, 4);
            std::memcpy(&dy, buf.data() + i * 12 + 8, 4);
            if (pin < 0 || pin >= pinCount) return false;
            out[pin * 2 + 0] = dx;
            out[pin * 2 + 1] = dy;
        }
    }
    return true;
}

}  // namespace viz
