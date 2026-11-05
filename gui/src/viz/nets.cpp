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
    if (loadFailed_) return false;
    if (cachedFrame_ == f && !cachedOffset_.empty()) { out = cachedOffset_; return true; }
    // 首次：所有 update blob 读入并校验（一次），之后只在内存回放。
    if (!blobsLoaded_) {
        cachedBlobs_.assign(offsetUpdates.size(), {});
        for (size_t i = 0; i < offsetUpdates.size(); ++i) {
            auto buf = readAll(bundleDir + "/" + offsetUpdates[i].file);
            size_t expect = static_cast<size_t>(offsetUpdates[i].count) * 12;
            if (buf.size() != expect) { loadFailed_ = true; return false; }
            cachedBlobs_[i] = std::move(buf);
        }
        blobsLoaded_ = true;
    }
    // 从基准重放到 f：随机 seek 后退/前进都正确（不依赖上次帧号）。
    cachedOffset_ = pinOffset;
    if (cachedOffset_.size() != static_cast<size_t>(pinCount) * 2) { loadFailed_ = true; return false; }
    for (size_t i = 0; i < offsetUpdates.size(); ++i) {
        if (offsetUpdates[i].frame > f) break;
        const auto& buf = cachedBlobs_[i];
        for (size_t j = 0; j < static_cast<size_t>(offsetUpdates[i].count); ++j) {
            int pin = 0; float dx = 0, dy = 0;
            std::memcpy(&pin, buf.data() + j * 12 + 0, 4);
            std::memcpy(&dx, buf.data() + j * 12 + 4, 4);
            std::memcpy(&dy, buf.data() + j * 12 + 8, 4);
            if (pin < 0 || pin >= pinCount) { loadFailed_ = true; return false; }
            cachedOffset_[pin * 2 + 0] = dx;
            cachedOffset_[pin * 2 + 1] = dy;
        }
    }
    cachedFrame_ = f;
    out = cachedOffset_;
    return true;
}

}  // namespace viz
