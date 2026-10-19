// LiveSource：把正在运行的布局器包装成 IPlacementSource。
//
// **这是整个工程里唯一 include 算法侧头文件的文件**，也是唯一会随 PlaceDB 变动
// 而需要维护的文件。UI 与渲染层一行都不用改——它们只认 IPlacementSource。
// 这正是 §2 那套解耦设计要兑现的东西：低耦合不等于放弃实时模式。
//
// 只在给出 -DSUPERPLACE_ROOT 时才编译进 superplace-gui；独立版 superplace-viz
// 根本不编译它，因此不沾 FFTW / Eigen / libpng / OpenMP。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "gp/eplace.h"
#include "gp/filler.h"
#include "init/initial_placer.h"
#include "io/bookshelf_reader.h"
#include "util/config.h"
#include "util/metrics.h"
#include "viz/live_source.h"

namespace viz {
namespace {

class LiveSource final : public IPlacementSource {
public:
    ~LiveSource() override {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    const SceneInfo&    scene()    const override { return scene_; }
    const NodeGeometry& geometry() const override { return geom_; }
    const MetricsTable* metrics()  const override { return &metrics_; }
    bool isLive() const override { return true; }
    const std::string& sourceName() const override { return name_; }

    int frameCount() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<int>(frames_.size());
    }

    bool frame(int index, FrameView& out) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (index < 0 || index >= static_cast<int>(frames_.size())) return false;
        const Snapshot& s = frames_[static_cast<size_t>(index)];
        out.x = s.pos.data();
        out.y = s.pos.data() + scene_.nodeCount;
        out.count = scene_.nodeCount;
        out.index = index;
        out.stage = s.stage;
        out.label = s.label;
        return true;
    }

    bool poll() override { return dirty_.exchange(false); }

    bool start(const std::string& auxPath, int strideIters, std::string* err) {
        name_ = auxPath;
        try {
            sp::BookshelfReader reader;
            reader.read(auxPath, db_);
        } catch (const std::exception& e) {
            *err = std::string("解析失败：") + e.what();
            return false;
        }

        cfg_.aux_path = auxPath;
        cfg_.random_seed = 1002;
        // filler 必须在建立几何快照之前插入——节点总数会变，而几何是整场不变量
        sp::initializeFillers(db_, cfg_);
        buildStatic();
        stride_ = strideIters > 0 ? strideIters : 1;

        worker_ = std::thread([this] { runEngine(); });
        return true;
    }

private:
    struct Snapshot {
        std::vector<float> pos;   ///< 2N：前半 x、后半 y
        std::string stage, label;
    };

    /// 几何与元信息只在开跑前算一次。
    void buildStatic() {
        const int n = db_.totalNodes();
        scene_.benchmark = cfg_.benchmark_name.empty() ? name_ : cfg_.benchmark_name;
        scene_.nodeCount = n;
        scene_.core = {db_.coreRegion.lx, db_.coreRegion.ly, db_.coreRegion.hx, db_.coreRegion.hy};
        scene_.chip = {db_.chipRegion.lx, db_.chipRegion.ly, db_.chipRegion.hx, db_.chipRegion.hy};
        scene_.rowHeight = db_.rowHeight;
        scene_.rows = static_cast<int>(db_.rows.size());

        geom_.w.resize(static_cast<size_t>(n));
        geom_.h.resize(static_cast<size_t>(n));
        geom_.flags.resize(static_cast<size_t>(n));
        geom_.kind.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            geom_.w[static_cast<size_t>(i)] = db_.node_w[static_cast<size_t>(i)];
            geom_.h[static_cast<size_t>(i)] = db_.node_h[static_cast<size_t>(i)];
            // 算法侧的标志位与 viz::NodeFlag 数值一致（见 scene.h 的注释），直接透传
            geom_.flags[static_cast<size_t>(i)] = db_.node_flags[static_cast<size_t>(i)];
            const Kind k = kindOf(geom_.flags[static_cast<size_t>(i)]);
            geom_.kind[static_cast<size_t>(i)] = static_cast<uint8_t>(k);
            scene_.countByKind[static_cast<int>(k)]++;
        }
        metrics_.columns = {"stage", "iter", "hpwl", "overflow"};
        metrics_.values.resize(4);
    }

    /// 从 db 当前坐标抓一帧。**必须在 worker 线程调用**，由 mutex 保护发布。
    void capture(const char* stage, int iter) {
        Snapshot s;
        s.pos.resize(static_cast<size_t>(scene_.nodeCount) * 2);
        const int n = scene_.nodeCount;
        for (int i = 0; i < n; ++i) {
            s.pos[static_cast<size_t>(i)] = db_.node_x[static_cast<size_t>(i)];
            s.pos[static_cast<size_t>(n + i)] = db_.node_y[static_cast<size_t>(i)];
        }
        s.stage = stage;
        s.label = std::string(stage) + " iter " + std::to_string(iter);
        {
            std::lock_guard<std::mutex> lk(mu_);
            frames_.push_back(std::move(s));
        }
        dirty_.store(true);
    }

    void runEngine() {
        sp::MetricsSink sink;
        if (auto placer = sp::makeInitialPlacer(cfg_.init_method)) {
            placer->place(db_, cfg_, sink);
        }
        capture("init", 0);

        sp::EPlace engine(db_, cfg_, sink);
        engine.setIterCallback([this](sp::GpStage stage, int iter, const sp::PlaceDB&) {
            // 回调在 worker 线程里，db_ 此刻是一致的；每 stride 轮抓一帧，
            // 否则 330k 节点下每轮 2.6 MB，几百轮就是几百 MB。
            if (iter % stride_ != 0) return;
            capture(sp::toString(stage), iter);
        });
        engine.run(sp::GpStage::kMGP);
        capture("done", 0);
        std::fprintf(stderr, "[live] 布局结束，HPWL = %.6g\n", sp::computeHPWL(db_));
    }

    sp::PlaceDB db_;
    sp::Config  cfg_;
    SceneInfo    scene_;
    NodeGeometry geom_;
    MetricsTable metrics_;
    std::string  name_;
    int          stride_ = 5;

    mutable std::mutex    mu_;
    std::vector<Snapshot> frames_;
    std::atomic<bool>     dirty_{false};
    std::atomic<bool>     stop_{false};
    std::thread           worker_;
};

}  // namespace

std::unique_ptr<IPlacementSource> makeLiveSource(const std::string& auxPath, int strideIters,
                                                 std::string* error) {
    std::string local;
    std::string* err = error ? error : &local;
    auto src = std::make_unique<LiveSource>();
    if (!src->start(auxPath, strideIters, err)) return nullptr;
    return src;
}

}  // namespace viz
