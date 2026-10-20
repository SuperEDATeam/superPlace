// BundleSource：从 viz bundle 目录读数据。
//
// 这是独立模式下唯一的数据源，也是本工程里唯一需要理解文件格式的地方。
// 格式规范见 work_report/06-前端开发方案与接口契约.md §3。
// 它只用到标准库 + nlohmann/json，不碰算法侧任何头文件。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

#include "viz/source.h"
#include "viz/summary.h"

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
    auto num = [&](const char* k, float def) -> float {
        auto it = j.find(k);
        return (it != j.end() && it->is_number()) ? it->get<float>() : def;
    };
    r.lx = num("lx", 0.0f);
    r.ly = num("ly", 0.0f);
    r.hx = num("hx", 0.0f);
    r.hy = num("hy", 0.0f);
    return r;
}

// 安全标量读取：类型不符（如 nodeCount 写成字符串）返回默认值而不是抛
// nlohmann::type_error 导致进程 Aborted(134)。
int safeInt(const nlohmann::json& j, const char* k, int def) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number_integer()) ? it->get<int>() :
           (it != j.end() && it->is_number_float()) ? static_cast<int>(it->get<float>()) : def;
}
float safeFloat(const nlohmann::json& j, const char* k, float def) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<float>() : def;
}
std::string safeString(const nlohmann::json& j, const char* k, const std::string& def) {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
bool safeBool(const nlohmann::json& j, const char* k, bool def) {
    auto it = j.find(k);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

// 硬上限：防止恶意 nodeCount 先分配后校验造成 bad_alloc/OOM。
constexpr size_t kMaxNodes = 64ull * 1024 * 1024;

class BundleSource final : public IPlacementSource {
public:
    const SceneInfo&    scene()    const override { return scene_; }
    const NodeGeometry& geometry() const override { return geom_; }
    const MetricsTable* metrics()  const override { return hasMetrics_ ? &metrics_ : nullptr; }
    const SummaryTable* summary() const override { return hasSummary_ ? &summary_ : nullptr; }
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

        scene_.benchmark = safeString(j, "benchmark", name_);
        scene_.nodeCount = safeInt(j, "nodeCount", 0);
        scene_.rowHeight = safeFloat(j, "rowHeight", 0.0f);
        scene_.rows = safeInt(j, "rows", 0);
        if (j.contains("core") && j["core"].is_object()) scene_.core = readRect(j["core"]);
        if (j.contains("chip") && j["chip"].is_object())
            scene_.chip = readRect(j["chip"]);
        else
            scene_.chip = scene_.core;
        if (scene_.nodeCount <= 0) {
            *err = "nodeCount 非法或类型错误（应为正整数）";
            return false;
        }
        if (scene_.chip.width() <= 0.f || scene_.chip.height() <= 0.f) {
            *err = "chip 区域退化";
            return false;
        }

        // R1-3：先按声明的 nodeCount 与 geom.bin/flags.bin 实际大小一致性校验，
        // 通过后才分配，避免 2e9 这类恶意声明先 vector 分配再 bad_alloc。
        const size_t n = static_cast<size_t>(scene_.nodeCount);
        if (n > kMaxNodes) {
            *err = "nodeCount 超过安全上限，拒绝分配";
            return false;
        }
        auto fileSize = [](const fs::path& p, size_t& out) -> bool {
            std::error_code ec;
            out = fs::file_size(p, ec);
            return !ec;
        };
        size_t geomBytes = 0, flagsBytes = 0;
        if (!fileSize(dir / "geom.bin", geomBytes)) { *err = "打不开 geom.bin"; return false; }
        if (!fileSize(dir / "flags.bin", flagsBytes)) { *err = "打不开 flags.bin"; return false; }
        const size_t expectGeom = n * 2 * sizeof(float);
        const size_t expectFlags = n;
        if (geomBytes != expectGeom) {
            *err = "geom.bin 大小与 nodeCount 不一致（声明 " + std::to_string(expectGeom) +
                   " 字节，实得 " + std::to_string(geomBytes) + " 字节）";
            return false;
        }
        if (flagsBytes != expectFlags) {
            *err = "flags.bin 大小与 nodeCount 不一致（声明 " + std::to_string(expectFlags) +
                   " 字节，实得 " + std::to_string(flagsBytes) + " 字节）";
            return false;
        }

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

        const auto fit = j.find("frames");
        if (fit != j.end() && fit->is_array()) {
            for (const auto& f : *fit) {
                if (!f.is_object()) continue;
                FrameEntry fe;
                fe.file = safeString(f, "file", "");
                fe.stage = safeString(f, "stage", "");
                fe.label = safeString(f, "label", fe.stage);
                if (!fe.file.empty()) frames_.push_back(std::move(fe));
            }
        }
        if (frames_.empty()) {
            *err = "bundle 里一帧都没有";
            return false;
        }

        hasMetrics_ = safeBool(j, "hasMetrics", false) && fs::exists(dir / "metrics.csv");
        if (hasMetrics_) loadMetrics(dir / "metrics.csv");

        // summary.json 可选（T1）。scene.json 的 hasSummary 只是提示，以文件是否
        // 存在为准；解析失败不致命——降级为"不含总览"，面板据此提示而不是崩溃。
        if (fs::exists(dir / "summary.json")) {
            std::string serr;
            if (loadSummaryFile((dir / "summary.json").string(), summary_, &serr)) {
                hasSummary_ = true;
            } else {
                std::fprintf(stderr, "[viz] summary.json 跳过：%s\n", serr.c_str());
            }
        }
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

    // R1-5：ragged 行【不补 0】（补 0 是造数）。缺列填 NaN，ImPlot 在 NaN 处断线，
    // 维度始终 = 列数 × rowCount，绘图/tooltip 不越界，也不会把缺失画成 0。
    //
    // R2-2：缺 stage 名【不沿用上一阶段】——缺名原本无法知道语义，继承是猜测。
    // 缺失/空白一律存空串（未知），FrameMap 对空阶段返回 Unavailable，不参与映射；
    // 旧源没有 stage 列时同样全部记为未知，不臆造分段。
    static std::string trimCell(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    void loadMetrics(const fs::path& p) {
        std::ifstream in(p);
        std::string line;
        if (!std::getline(in, line)) return;
        {
            std::stringstream ss(line);
            for (std::string c; std::getline(ss, c, ',');)
                metrics_.columns.push_back(trimCell(c));
        }
        const size_t ncol = metrics_.columns.size();
        metrics_.values.assign(ncol, {});
        const float nan = std::nanf("");

        // 定位阶段列：优先表头 "stage"；没有该列则视为无阶段信息（全未知），
        // 不再默认第 0 列就是阶段，避免把数值列误当阶段名。
        int stageCol = -1;
        for (size_t c = 0; c < ncol; ++c)
            if (metrics_.columns[c] == "stage" || metrics_.columns[c] == "阶段") {
                stageCol = static_cast<int>(c);
                break;
            }

        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::vector<std::string> cells(ncol);
            for (size_t c = 0; c < ncol; ++c) {
                if (!std::getline(ss, cells[c], ',')) cells[c].clear();
            }
            // 缺名/空白 = 未知（空串），绝不继承上一阶段。
            metrics_.stage.push_back(stageCol >= 0 ? trimCell(cells[static_cast<size_t>(stageCol)])
                                                   : std::string());
            for (size_t c = 0; c < ncol; ++c) {
                if (static_cast<int>(c) == stageCol) {
                    metrics_.values[c].push_back(0.f);
                    continue;
                }
                float v = nan;
                const std::string t = trimCell(cells[c]);
                if (!t.empty()) {
                    try { v = std::stof(t); } catch (...) { v = nan; }
                }
                metrics_.values[c].push_back(v);
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
    SummaryTable summary_;
    bool         hasMetrics_ = false;
    bool         hasSummary_ = false;
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
