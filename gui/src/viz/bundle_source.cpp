// BundleSource：从 viz bundle 目录读数据。
//
// 这是独立模式下唯一的数据源，也是本工程里唯一需要理解文件格式的地方。
// 格式规范见 work_report/06-前端开发方案与接口契约.md §3。
// 它只用到标准库 + nlohmann/json，不碰算法侧任何头文件。
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "viz/source.h"

namespace fs = std::filesystem;

namespace viz {
namespace {

constexpr const char* kFormatPrefix = "superplace-viz/";

bool readBinary(const fs::path& p, void* dst, size_t bytes, std::string* err) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        *err = "打不开 " + p.string();
        return false;
    }
    const auto actual = static_cast<size_t>(in.tellg());
    if (actual != bytes) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s 大小应为 %zu 字节，实得 %zu",
                      p.filename().c_str(), bytes, actual);
        *err = buf;
        return false;
    }
    in.seekg(0);
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(bytes));
    return static_cast<bool>(in);
}

Rect readRect(const nlohmann::json& j) {
    Rect r;
    r.lx = j.value("lx", 0.0f);
    r.ly = j.value("ly", 0.0f);
    r.hx = j.value("hx", 0.0f);
    r.hy = j.value("hy", 0.0f);
    return r;
}

class BundleSource final : public IPlacementSource {
public:
    const SceneInfo&    scene()    const override { return scene_; }
    const NodeGeometry& geometry() const override { return geom_; }
    const MetricsTable* metrics()  const override { return hasMetrics_ ? &metrics_ : nullptr; }
    int  frameCount() const override { return static_cast<int>(frames_.size()); }
    const std::string& sourceName() const override { return name_; }

    bool frame(int index, FrameView& out) override {
        if (index < 0 || index >= frameCount()) return false;
        FrameEntry& fe = frames_[static_cast<size_t>(index)];
        if (fe.data.empty() && !loadFrame(fe)) return false;
        touch(index);
        out.x = fe.data.data();
        out.y = fe.data.data() + scene_.nodeCount;
        out.count = scene_.nodeCount;
        out.index = index;
        out.stage = fe.stage;
        out.label = fe.label;
        return true;
    }

    bool open(const fs::path& dir, std::string* err) {
        dir_ = dir;
        name_ = dir.filename().string();

        nlohmann::json j;
        {
            std::ifstream in(dir / "scene.json");
            if (!in) {
                *err = "找不到 " + (dir / "scene.json").string();
                return false;
            }
            try {
                in >> j;
            } catch (const std::exception& e) {
                *err = std::string("scene.json 解析失败：") + e.what();
                return false;
            }
        }

        const std::string format = j.value("format", "");
        if (format.rfind(kFormatPrefix, 0) != 0) {
            *err = "不是 viz bundle（format=" + format + "）";
            return false;
        }

        scene_.benchmark = j.value("benchmark", name_);
        scene_.nodeCount = j.value("nodeCount", 0);
        scene_.rowHeight = j.value("rowHeight", 0.0f);
        scene_.rows = j.value("rows", 0);
        if (j.contains("core")) scene_.core = readRect(j["core"]);
        scene_.chip = j.contains("chip") ? readRect(j["chip"]) : scene_.core;
        if (scene_.nodeCount <= 0) {
            *err = "nodeCount 非法";
            return false;
        }
        if (scene_.chip.width() <= 0.f || scene_.chip.height() <= 0.f) {
            *err = "chip 区域退化";
            return false;
        }

        const size_t n = static_cast<size_t>(scene_.nodeCount);
        std::vector<float> wh(n * 2);
        geom_.flags.resize(n);
        if (!readBinary(dir / "geom.bin", wh.data(), n * 2 * sizeof(float), err)) return false;
        if (!readBinary(dir / "flags.bin", geom_.flags.data(), n, err)) return false;

        // geom.bin 是交错存的 (w,h)，拆成两列 SoA——渲染时要按列上传
        geom_.w.resize(n);
        geom_.h.resize(n);
        geom_.kind.resize(n);
        for (size_t i = 0; i < n; ++i) {
            geom_.w[i] = wh[i * 2];
            geom_.h[i] = wh[i * 2 + 1];
            const Kind k = kindOf(geom_.flags[i]);
            geom_.kind[i] = static_cast<uint8_t>(k);
            scene_.countByKind[static_cast<int>(k)]++;
        }

        // names.txt 是可选的，没有就不开检视面板的名字列
        if (fs::exists(dir / "names.txt")) {
            std::ifstream in(dir / "names.txt");
            geom_.name.reserve(n);
            for (std::string line; std::getline(in, line);) geom_.name.push_back(line);
            if (geom_.name.size() != n) geom_.name.clear();   // 对不上就当没有
        }

        for (const auto& f : j.value("frames", nlohmann::json::array())) {
            FrameEntry fe;
            fe.file = f.value("file", "");
            fe.stage = f.value("stage", "");
            fe.label = f.value("label", fe.stage);
            if (!fe.file.empty()) frames_.push_back(std::move(fe));
        }
        if (frames_.empty()) {
            *err = "bundle 里一帧都没有";
            return false;
        }

        hasMetrics_ = j.value("hasMetrics", false) && fs::exists(dir / "metrics.csv");
        if (hasMetrics_) loadMetrics(dir / "metrics.csv");
        return true;
    }

private:
    struct FrameEntry {
        std::string file, stage, label;
        std::vector<float> data;   ///< 2N：前半 x、后半 y。空表示还没加载
    };

    bool loadFrame(FrameEntry& fe) {
        const size_t n = static_cast<size_t>(scene_.nodeCount);
        fe.data.resize(n * 2);
        std::string err;
        if (!readBinary(dir_ / fe.file, fe.data.data(), n * 2 * sizeof(float), &err)) {
            fe.data.clear();
            std::fprintf(stderr, "[viz] %s\n", err.c_str());
            return false;
        }
        return true;
    }

    /// 帧是动画载荷，330k 节点每帧 2.6 MB，全缓存会吃掉几百 MB。
    /// 只留最近 kCacheFrames 帧，其余按最久未用淘汰。
    void touch(int index) {
        lru_.erase(std::remove(lru_.begin(), lru_.end(), index), lru_.end());
        lru_.push_back(index);
        while (static_cast<int>(lru_.size()) > kCacheFrames) {
            frames_[static_cast<size_t>(lru_.front())].data = {};
            frames_[static_cast<size_t>(lru_.front())].data.shrink_to_fit();
            lru_.erase(lru_.begin());
        }
    }

    void loadMetrics(const fs::path& p) {
        std::ifstream in(p);
        std::string line;
        if (!std::getline(in, line)) return;
        {
            std::stringstream ss(line);
            for (std::string c; std::getline(ss, c, ',');) metrics_.columns.push_back(c);
        }
        metrics_.values.resize(metrics_.columns.size());
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            size_t col = 0;
            for (std::string cell; std::getline(ss, cell, ',') && col < metrics_.columns.size();
                 ++col) {
                if (col == 0) {
                    metrics_.stage.push_back(cell);          // 第 0 列是阶段名
                    metrics_.values[0].push_back(0.f);
                } else {
                    float v = 0.f;
                    try { v = std::stof(cell); } catch (...) { v = 0.f; }
                    metrics_.values[col].push_back(v);
                }
            }
            ++metrics_.rowCount;
        }
    }

    static constexpr int kCacheFrames = 24;

    fs::path     dir_;
    std::string  name_;
    SceneInfo    scene_;
    NodeGeometry geom_;
    MetricsTable metrics_;
    bool         hasMetrics_ = false;
    std::vector<FrameEntry> frames_;
    std::vector<int>        lru_;
};

}  // namespace

std::unique_ptr<IPlacementSource> makeBundleSource(const std::string& dir, std::string* error) {
    std::string local;
    std::string* err = error ? error : &local;
    auto src = std::make_unique<BundleSource>();
    if (!src->open(dir, err)) return nullptr;
    return src;
}

}  // namespace viz
