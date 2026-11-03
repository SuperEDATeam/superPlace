#include "io/viz_writer.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "db/place_db.h"
#include "nlohmann/json.hpp"
#include "util/logger.h"

namespace fs = std::filesystem;

namespace sp {
namespace {

void writeBinary(const fs::path& p, const void* data, size_t bytes) {
    std::FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + p.string());
    const size_t ok = std::fwrite(data, 1, bytes, f);
    std::fclose(f);
    if (ok != bytes) throw std::runtime_error("short write: " + p.string());
}

}  // namespace

void VizBundleWriter::open(const std::string& dir, const std::string& benchmark,
                           const PlaceDB& db, int frameStride) {
    dir_ = dir;
    n_ = db.numNodes;
    stride_ = frameStride;
    fs::create_directories(fs::path(dir_) / "frames");

    // geom.bin 是交错的 (w, h)，与 make_viz.py 产出的布局一致
    std::vector<float> wh(static_cast<size_t>(n_) * 2);
    for (int i = 0; i < n_; ++i) {
        wh[static_cast<size_t>(i) * 2] = db.node_w[static_cast<size_t>(i)];
        wh[static_cast<size_t>(i) * 2 + 1] = db.node_h[static_cast<size_t>(i)];
    }
    writeBinary(fs::path(dir_) / "geom.bin", wh.data(), wh.size() * sizeof(float));
    writeBinary(fs::path(dir_) / "flags.bin", db.node_flags.data(), static_cast<size_t>(n_));

    {
        std::ofstream out(fs::path(dir_) / "names.txt");
        for (int i = 0; i < n_; ++i) out << db.node_name[static_cast<size_t>(i)] << '\n';
    }

    benchmark_ = benchmark;
    coreLx_ = db.coreRegion.lx;  coreLy_ = db.coreRegion.ly;
    coreHx_ = db.coreRegion.hx;  coreHy_ = db.coreRegion.hy;
    chipLx_ = db.chipRegion.lx;  chipLy_ = db.chipRegion.ly;
    chipHx_ = db.chipRegion.hx;  chipHy_ = db.chipRegion.hy;
    rowHeight_ = db.rowHeight;
    rows_ = static_cast<int>(db.rows.size());
    last_.clear();
    frames_.clear();
    skipped_ = 0;
    spentMs_ = 0.0;
}

void VizBundleWriter::append(const PlaceDB& db, const FrameMark& m) {
    if (!isOpen()) return;
    const auto t0 = std::chrono::steady_clock::now();
    struct Spent {
        double& acc;
        std::chrono::steady_clock::time_point t0;
        ~Spent() {
            acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                       .count();
        }
    } spent{spentMs_, t0};
    const size_t n = static_cast<size_t>(n_);

    // PlaceDB 是 SoA：node_x 与 node_y 各自连续，帧格式恰好是"先 x[N] 再 y[N]"，
    // 所以比较与写盘都直接在 db 的内存上做，不需要拼一份中间数组。
    if (last_.size() == 2 * n &&
        std::memcmp(last_.data(), db.node_x.data(), n * sizeof(float)) == 0 &&
        std::memcmp(last_.data() + n, db.node_y.data(), n * sizeof(float)) == 0) {
        ++skipped_;
        return;
    }

    char name[32];
    std::snprintf(name, sizeof(name), "frames/f%04d.bin", frameCount());
    std::FILE* f = std::fopen((fs::path(dir_) / name).c_str(), "wb");
    if (!f) throw std::runtime_error(std::string("cannot write ") + name);
    const bool ok = std::fwrite(db.node_x.data(), sizeof(float), n, f) == n &&
                    std::fwrite(db.node_y.data(), sizeof(float), n, f) == n;
    std::fclose(f);
    if (!ok) throw std::runtime_error(std::string("short write: ") + name);

    last_.resize(2 * n);
    std::memcpy(last_.data(), db.node_x.data(), n * sizeof(float));
    std::memcpy(last_.data() + n, db.node_y.data(), n * sizeof(float));

    frames_.push_back({name, m});
    writeScene();
}

void VizBundleWriter::finish(const std::string& resultDir) {
    if (!isOpen()) return;
    const fs::path src(resultDir), dst(dir_);
    std::error_code ec;
    if (fs::exists(src / "metrics.csv")) {
        fs::copy_file(src / "metrics.csv", dst / "metrics.csv",
                      fs::copy_options::overwrite_existing, ec);
        hasMetrics_ = !ec;
    }
    if (fs::exists(src / "summary.json")) {
        fs::copy_file(src / "summary.json", dst / "summary.json",
                      fs::copy_options::overwrite_existing, ec);
        hasSummary_ = !ec;
    }
    writeScene();
    SP_INFO("viz bundle: %d 帧（跳过重复 %d 帧），录制耗时 %.0f ms -> %s", frameCount(), skipped_,
            spentMs_, dir_.c_str());
}

void VizBundleWriter::writeScene() const {
    nlohmann::ordered_json j;
    j["format"] = "superplace-viz/1";
    j["producer"] = "superplace-cli";
    j["benchmark"] = benchmark_;
    j["nodeCount"] = n_;
    j["core"] = {{"lx", coreLx_}, {"ly", coreLy_}, {"hx", coreHx_}, {"hy", coreHy_}};
    j["chip"] = {{"lx", chipLx_}, {"ly", chipLy_}, {"hx", chipHx_}, {"hy", chipHy_}};
    j["rowHeight"] = rowHeight_;
    j["rows"] = rows_;
    j["flagBits"] = {{"macro", F_MACRO}, {"fixed", F_FIXED}, {"filler", F_FILLER}, {"ni", F_NI}};
    j["frameStride"] = stride_;
    j["hasNames"] = true;
    j["hasMetrics"] = hasMetrics_;
    j["hasSummary"] = hasSummary_;

    auto arr = nlohmann::ordered_json::array();
    for (const Entry& e : frames_) {
        arr.push_back({{"file", e.file},
                       {"stage", e.mark.stage},
                       {"iter", e.mark.iter},
                       {"t", e.mark.t},
                       {"breakBefore", e.mark.breakBefore},
                       {"label", e.mark.label}});
    }
    j["frames"] = std::move(arr);

    const fs::path tmp = fs::path(dir_) / "scene.json.tmp";
    {
        std::ofstream out(tmp);
        out << j.dump(2) << '\n';
    }
    fs::rename(tmp, fs::path(dir_) / "scene.json");
}

}  // namespace sp
