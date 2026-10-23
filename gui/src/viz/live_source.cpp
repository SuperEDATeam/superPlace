// LiveSource：把正在运行的布局器包装成 IPlacementSource。
//
// **这是整个工程里唯一 include 算法侧头文件的文件**，也是唯一会随 PlaceDB 变动
// 而需要维护的文件。UI 与渲染层一行都不用改——它们只认 IPlacementSource。
// 这正是 §2 那套解耦设计要兑现的东西：低耦合不等于放弃实时模式。
//
// 只在给出 -DSUPERPLACE_ROOT 时才编译进 superplace-gui；独立版 superplace-viz
// 根本不编译它，因此不沾 FFTW / Eigen / libpng / OpenMP。
//
// T11：这里照搬 src/stages/gp_stage.cpp 的【权威四阶段序列】，不再只跑 mGP：
//   initializeFillers → mGP → legalizeMacros(一次跳变) → 重建 EPlace →
//   scatterFillers(seedOffset=1) → FILLERONLY → 重建 EPlace → cGP。
// 关键坑（06 §T11 / 05 §5.6.2）：mLG 后宏被置 F_FIXED，其面积从"可移动电荷"
// 变成"固定阻挡"，**每个 GP 阶段都各自新建 EPlace**（构造函数重算静态密度项），
// 不图省事复用一个。阶段语义以 gp_stage.cpp 为准，不在此处另立规矩。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "gp/eplace.h"
#include "gp/filler.h"
#include "init/initial_placer.h"
#include "io/bookshelf_reader.h"
#include "lg/macro_sa.h"
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
        // filler 必须在建立几何快照之前插入——节点总数会变，而几何是整场不变量。
        // 注意：filler 只按节点面积与行面积计数、用 Rng(seed) 随机撒点，与可移动节点
        // 的位置无关；而默认 init_method=quadratic 不消耗 Rng、只动 [0,numMovable)。
        // 故此处先插 filler、worker 里再跑初始布局，与 CLI"先 init 后 gp(内插 filler)"
        // 进入 mGP 时的 db 状态逐位一致（见 gp_stage.cpp 权威序列）。
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

    /// 把 db 当前坐标拷进一帧并发布。**必须在 worker 线程调用**，mutex 保护发布。
    void publish(const char* stage, std::string label) {
        Snapshot s;
        const int n = scene_.nodeCount;
        s.pos.resize(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n; ++i) {
            s.pos[static_cast<size_t>(i)] = db_.node_x[static_cast<size_t>(i)];
            s.pos[static_cast<size_t>(n + i)] = db_.node_y[static_cast<size_t>(i)];
        }
        s.stage = stage;
        s.label = std::move(label);
        {
            std::lock_guard<std::mutex> lk(mu_);
            frames_.push_back(std::move(s));
        }
        dirty_.store(true);
    }

    /// GP 逐迭代帧：label 带真实迭代号（来自 EPlace 回调，不伪造）。
    void captureIter(const char* stage, int iter) {
        publish(stage, std::string(stage) + " iter " + std::to_string(iter));
    }

    /// 里程碑帧（init / mLG / done）：不是 GP 迭代，label 直接给语义文案，
    /// **绝不写成 "xxx iter 0" 伪造一个迭代号**。
    void captureMilestone(const char* stage, const char* label) { publish(stage, label); }

    void runEngine() {
        sp::MetricsSink sink;
        if (auto placer = sp::makeInitialPlacer(cfg_.init_method)) {
            placer->place(db_, cfg_, sink);
        }
        captureMilestone("init", "初始布局");

        // 与 gp_stage.cpp 同口径：统计可移动宏，决定后三阶段是否执行。
        int movableMacros = 0;
        for (int i = 0; i < db_.numMovable; ++i)
            if (db_.isMacro(i) && !db_.isFixed(i)) ++movableMacros;

        // 逐迭代回调在 worker 线程里、db 此刻一致；每 stride 轮抓一帧。
        // stride 只决定抽样密度，不碰 db，故不改变任何阶段的优化轨迹。
        auto cb = [this](sp::GpStage stage, int iter, const sp::PlaceDB&) {
            if (iter % stride_ != 0) return;
            captureIter(sp::toString(stage), iter);
        };

        // ---- 阶段一：mGP（标准单元 + 可移动宏 + filler）
        {
            sp::EPlace engine(db_, cfg_, sink);
            engine.setIterCallback(cb);
            engine.run(sp::GpStage::kMGP);
        }

        if (movableMacros == 0) {
            // 无可移动宏：mLG / FILLERONLY / cGP 无事可做（与 gp_stage.cpp 同）。
        } else {
            // ---- 阶段二：mLG（宏模拟退火合法化，一次性跳变，结束后宏被钉死）
            // legalizeMacros 无逐轮回调，它一整次跑完才返回——这里补一帧，
            // 把"mGP 末帧 → mLG 后位置"这一次跳变明确表现出来，且 label 不假装是迭代。
            sp::legalizeMacros(db_, cfg_, sink);
            captureMilestone("mLG", "mLG 宏合法化（一次跳变，此后宏不再移动）");

            // ---- 阶段三：FILLERONLY（重撒 filler，只优化 filler，固定轮数）
            // 宏刚被挪动，旧 filler 分布围着旧宏位置已失效，重撒后跑几轮填匀。
            sp::scatterFillers(db_, cfg_, /*seedOffset=*/1);
            {
                sp::EPlace engine(db_, cfg_, sink);
                engine.setIterCallback(cb);
                engine.run(sp::GpStage::kFillerOnly);
            }

            // ---- 阶段四：cGP（标准单元全局布局，宏已固定不再参与）
            // 宏被钉死后面积从可移动电荷变固定阻挡，必须新建 EPlace 重算静态密度。
            {
                sp::EPlace engine(db_, cfg_, sink);
                engine.setIterCallback(cb);
                engine.run(sp::GpStage::kCGP);
            }
        }

        captureMilestone("done", "四阶段完成");
        // 全精度打印，供与 CLI summary 的 double 对拍（%.6g 只有 ~1e-6 相对精度，不够）。
        std::fprintf(stderr, "[live] 布局结束，HPWL = %.17g\n", sp::computeHPWL(db_));
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
