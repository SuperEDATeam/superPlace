// BundleSource：从 viz bundle 目录读数据。
//
// 这是独立模式下唯一的数据源，也是本工程里唯一需要理解文件格式的地方。
// 格式规范见 work_report/06-前端开发方案与接口契约.md §3。
// 它只用到标准库 + nlohmann/json，不碰算法侧任何头文件。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

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
    ~BundleSource() override {
        stop_ = true;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    const SceneInfo&    scene()    const override { return scene_; }
    const NodeGeometry& geometry() const override { return geom_; }
    const MetricsTable* metrics()  const override { return hasMetrics_ ? &metrics_ : nullptr; }
    const SummaryTable* summary() const override { return hasSummary_ ? &summary_ : nullptr; }
    int  frameCount() const override { return static_cast<int>(frames_.size()); }
    const std::string& sourceName() const override { return name_; }

    void prefetch(int center, int direction) override {
        if (frames_.empty()) return;
        wantCenter_ = center;
        wantDir_ = direction;
        cv_.notify_all();
        ensureWorker();
    }

    bool frame(int index, FrameView& out) override {
        if (index < 0 || index >= frameCount()) return false;
        std::shared_ptr<std::vector<float>> fresh;
        std::string file;
        {
            std::lock_guard<std::mutex> lk(mu_);
            FrameEntry& fe = frames_[static_cast<size_t>(index)];
            if (fe.data) {                  // 已在缓存：直接给，只做 touch
                installView(out, fe, index);
                touch(index);
                return true;
            }
            file = fe.file;                 // 元数据不可变，拷出文件名后锁外读
        }
        fresh = readPositions(file);        // 锁外读盘，UI 不被磁盘 IO 阻塞
        if (!fresh) return false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            FrameEntry& fe = frames_[static_cast<size_t>(index)];
            if (!fe.data) fe.data = std::move(fresh);   // 双检：worker 可能已装好
            installView(out, fe, index);
            touch(index);
        }
        return true;
    }

    FrameMeta frameInfo(int index) const override {
        std::lock_guard<std::mutex> lk(mu_);
        FrameMeta fm;
        if (index < 0 || index >= frameCount()) return fm;
        const FrameEntry& fe = frames_[static_cast<size_t>(index)];
        fm.index = index;
        fm.stage = fe.stage;
        fm.label = fe.label;
        fm.iter = fe.iter;
        fm.t = fe.t;
        fm.breakBefore = fe.breakBefore;
        return fm;
    }

    bool hasGlobalT() const override { return hadAnyT_; }

    int cachedFrameCount() const override {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (const FrameEntry& fe : frames_) if (fe.data) ++n;
        return n;
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
                fe.iter = safeInt(f, "iter", -2);
                fe.breakBefore = safeBool(f, "breakBefore", false);
                // t 是可选字段；任一带了就视为本 bundle 带真实全局 t。
                if (f.contains("t") && f["t"].is_number()) {
                    fe.t = f["t"].get<double>();
                    hadAnyT_ = true;
                } else {
                    fe.t = 0.0;
                }
                if (!fe.file.empty()) frames_.push_back(std::move(fe));
            }
        }
        if (frames_.empty()) {
            *err = "bundle 里一帧都没有";
            return false;
        }
        // 不带 t 的 bundle（合成样本 / make_viz.py 转换）：退化为按帧序号均匀分布。
        if (!hadAnyT_) {
            for (size_t i = 0; i < frames_.size(); ++i)
                frames_[i].t = static_cast<double>(i);
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
        int         iter = -1;
        double      t = 0.0;
        bool        breakBefore = false;
        /// 2N：前半 x、后半 y。shared_ptr 共享所有权——LRU 淘汰只放缓存这份引用，
        /// 任何持有 FrameView 的调用方数据都活着（T15）。空表示尚未加载。
        std::shared_ptr<std::vector<float>> data;
    };

    /// 锁外读一个位置文件到独立 shared_ptr（磁盘 IO 不持 mu_）。失败返回 nullptr。
    /// 一次只缓冲一帧，worker 串行读，避免一次读 3 帧占大块内存。
    std::shared_ptr<std::vector<float>> readPositions(const std::string& file) {
        const size_t n = static_cast<size_t>(scene_.nodeCount);
        auto buf = std::make_shared<std::vector<float>>(n * 2);
        std::string err;
        if (!readBinary(dir_ / file, buf->data(), n * 2 * sizeof(float), &err)) {
            std::fprintf(stderr, "[viz] %s\n", err.c_str());
            return nullptr;
        }
        return buf;
    }

    void installView(FrameView& out, const FrameEntry& fe, int index) {
        const size_t n = static_cast<size_t>(scene_.nodeCount);
        out.data = fe.data;                 // 共享所有权：缓存淘汰不影响持有者
        out.x = fe.data->data();
        out.y = fe.data->data() + n;
        out.count = scene_.nodeCount;
        out.index = index;
        out.stage = fe.stage;
        out.label = fe.label;
    }

    /// 帧是动画载荷，330k 节点每帧 2.6 MB，全缓存会吃掉几百 MB。
    /// 只留最近 kCacheFrames 帧，其余按最久未用淘汰（放掉缓存自己那份引用）。
    void touch(int index) {
        lru_.erase(std::remove(lru_.begin(), lru_.end(), index), lru_.end());
        lru_.push_back(index);
        while (static_cast<int>(lru_.size()) > kCacheFrames) {
            frames_[static_cast<size_t>(lru_.front())].data.reset();
            lru_.erase(lru_.begin());
        }
    }

    // T17：按需启动后台预读线程。只往缓存里放数据，不碰 UI 正持有的帧
    // （shared_ptr 保活，淘汰只放缓存自己那份引用）。
    void ensureWorker() {
        if (workerStarted_.test_and_set()) return;
        worker_ = std::thread([this] { workerLoop(); });
    }

    void workerLoop() {
        for (;;) {
            int center = 0, dir = 1;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, std::chrono::milliseconds(120),
                             [&] { return stop_.load(); });
                if (stop_.load()) return;       // 可靠关闭：唤醒后立即退出并 join
                center = wantCenter_;
                dir = wantDir_;
            }
            // 逐帧处理：每帧在锁内检查是否已加载、拷出文件名，锁外读盘，
            // 再锁内双检安装并 touch 进 LRU（有界，避免无界增长）。
            for (int k = 1; k <= 3; ++k) {
                if (stop_.load()) return;
                const int idx = center + dir * k;
                if (idx < 0 || idx >= frameCount()) continue;
                std::string file;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (frames_[static_cast<size_t>(idx)].data) continue;
                    file = frames_[static_cast<size_t>(idx)].file;
                }
                auto buf = readPositions(file);     // 锁外 IO，不阻塞 UI
                if (!buf) continue;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (!frames_[static_cast<size_t>(idx)].data) {
                        frames_[static_cast<size_t>(idx)].data = std::move(buf);
                        touch(idx);                 // 进 LRU，受 kCacheFrames 约束
                    }
                }
            }
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
    bool         hadAnyT_ = false;
    std::vector<FrameEntry> frames_;
    std::vector<int>        lru_;

    // T17 预读线程：缓存访问统一走 mu_；可靠关闭靠 stop_ + join。
    mutable std::mutex            mu_;
    std::condition_variable       cv_;
    std::thread                   worker_;
    std::atomic<bool>             stop_{false};
    std::atomic<int>             wantCenter_{0};
    std::atomic<int>             wantDir_{1};
    std::atomic_flag             workerStarted_ = ATOMIC_FLAG_INIT;
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
