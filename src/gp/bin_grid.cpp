#include "gp/bin_grid.h"

#include <algorithm>
#include <cmath>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "db/place_db.h"
#include "util/logger.h"

namespace sp {
namespace {

constexpr int kMaxBinDim = 1024;

int numThreads() {
#ifdef _OPENMP
    return omp_get_max_threads();
#else
    return 1;
#endif
}

}  // namespace

BinGrid::Footprint BinGrid::footprintOf(const PlaceDB& db, int i) const {
    Footprint f;
    float lx = db.llx(i), hx = db.urx(i);
    float ly = db.lly(i), hy = db.ury(i);
    float sx = 1.f, sy = 1.f;

    // local smoothing（05 §5.5.4）：比一个 bin 还小的单元虚拟放大到 bin 尺寸，
    // 同时记录缩放比，累加时乘回去使总面积守恒。
    //
    // 不做的话密度场会是分段常数函数——单元跨越 bin 边界时密度发生跳变，
    // 梯度随之不连续，优化器会在边界附近震荡甚至卡住。
    const float w = db.node_w[i], h = db.node_h[i];
    if (w < stepX_) {
        sx = w / stepX_;
        const float cx = db.node_x[i];
        lx = cx - 0.5f * stepX_;
        hx = cx + 0.5f * stepX_;
    }
    if (h < stepY_) {
        sy = h / stepY_;
        const float cy = db.node_y[i];
        ly = cy - 0.5f * stepY_;
        hy = cy + 0.5f * stepY_;
    }

    f.lx = lx; f.hx = hx; f.ly = ly; f.hy = hy;
    f.scale = sx * sy;
    f.bx0 = std::clamp(static_cast<int>((lx - originX_) / stepX_), 0, dim_ - 1);
    f.bx1 = std::clamp(static_cast<int>((hx - originX_) / stepX_), 0, dim_ - 1);
    f.by0 = std::clamp(static_cast<int>((ly - originY_) / stepY_), 0, dim_ - 1);
    f.by1 = std::clamp(static_cast<int>((hy - originY_) / stepY_), 0, dim_ - 1);
    return f;
}

float BinGrid::chargeWeight(const PlaceDB& db, int i, float footprintScale) const {
    // 密度缩放规则（05 §5.5.4）：乘 targetDensity 的只有【宏单元、固定终端、暗节点】。
    // 标准单元与 filler 不乘——filler 的总面积本身就是按目标密度设计的。
    if (db.isFiller(i)) return footprintScale;
    if (db.isMacro(i))  return footprintScale * targetDensity_;
    return footprintScale;
}

void BinGrid::initialize(const PlaceDB& db, float targetDensity, int binDimOverride) {
    targetDensity_ = targetDensity;
    originX_ = db.coreRegion.lx;
    originY_ = db.coreRegion.ly;

    // ---- 网格维度：取 2 的幂，使 (2^i)² <= idealCount < (2^(i+1))²
    // 一律按【标志】而非下标区间判定可移动性。mLG 之后宏会被置 F_FIXED，但它仍
    // 留在 [0, numMovable) 分段内（分段是位置约定，重排代价太大）。若这里按区间
    // 判定，宏就会同时被算进 τ 的分母、又被 accumulate 按标志跳过，两侧分叉。
    double cellArea = 0.0, macroArea = 0.0;
    for (int i = 0; i < db.numMovable; ++i) {
        if (db.isFixed(i) || db.isNI(i)) continue;
        if (db.isMacro(i)) macroArea += db.area(i);
        else cellArea += db.area(i);
    }
    movableAreaScaled_ = cellArea + macroArea * static_cast<double>(targetDensity);

    if (binDimOverride > 0) {
        dim_ = std::min(binDimOverride, kMaxBinDim);
    } else {
        const double avgArea =
            (db.numMovable > 0) ? (cellArea + macroArea) / db.numMovable : 1.0;
        const double idealBinArea = std::max(avgArea / targetDensity, 1e-9);
        const double coreArea = static_cast<double>(db.coreRegion.width()) *
                                static_cast<double>(db.coreRegion.height());
        const double idealCount = coreArea / idealBinArea;
        dim_ = 4;
        while (dim_ * 2 <= kMaxBinDim &&
               static_cast<double>(dim_ * 2) * (dim_ * 2) <= idealCount) {
            dim_ *= 2;
        }
    }

    stepX_ = db.coreRegion.width() / static_cast<float>(dim_);
    stepY_ = db.coreRegion.height() / static_cast<float>(dim_);

    const size_t n = static_cast<size_t>(dim_) * static_cast<size_t>(dim_);
    terminalDensity_.assign(n, 0.f);
    baseDensity_.assign(n, 0.f);
    nodeDensity_.assign(n, 0.f);
    fillerDensity_.assign(n, 0.f);
    rho_.assign(n, 0.f);
    phi_.assign(n, 0.f);
    fieldX_.assign(n, 0.f);
    fieldY_.assign(n, 0.f);

    const int nt = numThreads();
    localNode_.assign(static_cast<size_t>(nt), std::vector<float>(n, 0.f));
    localFiller_.assign(static_cast<size_t>(nt), std::vector<float>(n, 0.f));

    // ---- terminalDensity：固定终端的阻塞（密度缩放施于终端）
    //
    // 遍历全部真实节点并按 isFixed 判定，与 accumulate 的跳过条件严格互补：
    // 每个节点要么进 terminalDensity、要么进 nodeDensity，不重不漏。
    // mLG 把宏置为 F_FIXED 后重建网格，宏就自动从后者转入前者。
    for (int i = 0; i < db.numNodes; ++i) {
        if (!db.isFixed(i)) continue;
        if (db.isNI(i)) continue;                 // 零面积 IO 引脚不计
        const Rect box = db.box(i);
        const int bx0 = std::clamp(static_cast<int>((box.lx - originX_) / stepX_), 0, dim_ - 1);
        const int bx1 = std::clamp(static_cast<int>((box.hx - originX_) / stepX_), 0, dim_ - 1);
        const int by0 = std::clamp(static_cast<int>((box.ly - originY_) / stepY_), 0, dim_ - 1);
        const int by1 = std::clamp(static_cast<int>((box.hy - originY_) / stepY_), 0, dim_ - 1);
        for (int bx = bx0; bx <= bx1; ++bx) {
            const float blx = originX_ + bx * stepX_;
            for (int by = by0; by <= by1; ++by) {
                const float bly = originY_ + by * stepY_;
                const Rect bin{blx, bly, blx + stepX_, bly + stepY_};
                terminalDensity_[static_cast<size_t>(bx) * dim_ + by] +=
                    targetDensity_ * overlapArea(bin, box);
            }
        }
    }

    // ---- baseDensity：bin 内不可放置区域的等效密度
    //
    // 朴素实现是 bins × rows 双重循环——adaptec1 上 512×512×890 ≈ 2.3 亿次求交。
    // 这里先按 y 把 rows 排序并建前缀索引，每个 bin 只遍历与其 y 区间相交的行，
    // 复杂度降到近线性。
    std::vector<int> rowOrder(db.rows.size());
    for (size_t r = 0; r < db.rows.size(); ++r) rowOrder[r] = static_cast<int>(r);
    std::sort(rowOrder.begin(), rowOrder.end(), [&](int a, int b) {
        return db.rows[static_cast<size_t>(a)].ly < db.rows[static_cast<size_t>(b)].ly;
    });

#pragma omp parallel for schedule(static)
    for (int by = 0; by < dim_; ++by) {
        const float bly = originY_ + by * stepY_;
        const float bhy = bly + stepY_;
        // 二分定位第一个 hy() > bly 的行
        size_t lo = 0, hi = rowOrder.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (db.rows[static_cast<size_t>(rowOrder[mid])].hy() <= bly) lo = mid + 1;
            else hi = mid;
        }
        for (int bx = 0; bx < dim_; ++bx) {
            const float blx = originX_ + bx * stepX_;
            const Rect bin{blx, bly, blx + stepX_, bhy};
            float covered = 0.f;
            for (size_t t = lo; t < rowOrder.size(); ++t) {
                const PlaceDB::SiteRow& row = db.rows[static_cast<size_t>(rowOrder[t])];
                if (row.ly >= bhy) break;          // 已越过本 bin 的 y 区间
                covered += overlapArea(bin, row.rect());
            }
            const float freeArea = std::max(0.f, bin.area() - covered);
            baseDensity_[static_cast<size_t>(bx) * dim_ + by] = targetDensity_ * freeArea;
        }
    }

    BackendConfig cfg;
    cfg.nx = dim_;
    cfg.ny = dim_;
    cfg.step_x = stepX_;
    cfg.step_y = stepY_;
    backend_ = makePoissonBackend(cfg);

    SP_INFO("bin grid: %dx%d  step (%.4f, %.4f)  backend %s", dim_, dim_, stepX_, stepY_,
            backend_->name());
}

void BinGrid::accumulate(const PlaceDB& db) {
    const size_t n = static_cast<size_t>(dim_) * static_cast<size_t>(dim_);
    const int nt = static_cast<int>(localNode_.size());

    for (int t = 0; t < nt; ++t) {
        std::fill(localNode_[static_cast<size_t>(t)].begin(),
                  localNode_[static_cast<size_t>(t)].end(), 0.f);
        std::fill(localFiller_[static_cast<size_t>(t)].begin(),
                  localFiller_[static_cast<size_t>(t)].end(), 0.f);
    }

    const int total = db.totalNodes();

    // 铁律 7：per-thread 局部网格 + 固定顺序规约。
    // 禁止原子加——它既慢（bin 冲突频繁），又让浮点累加顺序不确定，
    // 破坏逐位可复现。
#pragma omp parallel
    {
#ifdef _OPENMP
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        float* __restrict ln = localNode_[static_cast<size_t>(tid)].data();
        float* __restrict lf = localFiller_[static_cast<size_t>(tid)].data();

#pragma omp for schedule(static)
        for (int i = 0; i < total; ++i) {
            if (db.isFixed(i) || db.isNI(i)) continue;

            const Footprint f = footprintOf(db, i);
            const Rect rect{f.lx, f.ly, f.hx, f.hy};
            const bool filler = db.isFiller(i);
            const float w = chargeWeight(db, i, f.scale);

            for (int bx = f.bx0; bx <= f.bx1; ++bx) {
                const float blx = originX_ + bx * stepX_;
                for (int by = f.by0; by <= f.by1; ++by) {
                    const float bly = originY_ + by * stepY_;
                    const Rect bin{blx, bly, blx + stepX_, bly + stepY_};
                    const float ov = overlapArea(bin, rect) * w;
                    if (ov <= 0.f) continue;
                    const size_t idx = static_cast<size_t>(bx) * dim_ + by;
                    if (filler) lf[idx] += ov;
                    else        ln[idx] += ov;
                }
            }
        }
    }

    // 固定顺序规约：始终按线程号递增累加，保证逐位可复现
    std::fill(nodeDensity_.begin(), nodeDensity_.end(), 0.f);
    std::fill(fillerDensity_.begin(), fillerDensity_.end(), 0.f);
    for (int t = 0; t < nt; ++t) {
        const float* ln = localNode_[static_cast<size_t>(t)].data();
        const float* lf = localFiller_[static_cast<size_t>(t)].data();
#pragma omp parallel for schedule(static)
        for (size_t b = 0; b < n; ++b) {
            nodeDensity_[b] += ln[b];
            fillerDensity_[b] += lf[b];
        }
    }
}

void BinGrid::solveField() {
    const size_t n = rho_.size();
    const float invArea = 1.0f / binArea();
#pragma omp parallel for schedule(static)
    for (size_t b = 0; b < n; ++b) {
        rho_[b] = (nodeDensity_[b] + fillerDensity_[b] + terminalDensity_[b] + baseDensity_[b]) *
                  invArea;
    }
    backend_->solve({rho_.data(), dim_, dim_}, {phi_.data(), dim_, dim_},
                    {fieldX_.data(), dim_, dim_}, {fieldY_.data(), dim_, dim_});
}

float BinGrid::overflow() const {
    if (movableAreaScaled_ <= 0.0) return 0.f;
    const double area = static_cast<double>(binArea());
    const double target = static_cast<double>(targetDensity_);

    double over = 0.0;
    const size_t n = nodeDensity_.size();
#pragma omp parallel for schedule(static) reduction(+ : over)
    for (size_t b = 0; b < n; ++b) {
        // ρ' 只含可移动单元 + 终端 + 不可放置区域，**不含 filler**——
        // filler 是为平滑密度场人为插入的，把它计入会让 τ 永远接近目标值。
        const double rho =
            (static_cast<double>(nodeDensity_[b]) + terminalDensity_[b] + baseDensity_[b]) / area;
        // 必须取正部：否则稀疏 bin 的负溢出会抵消拥挤 bin 的正溢出，
        // 而总面积守恒会让 τ 近似为常数，完全失去停止判据的意义。
        const double d = rho - target;
        if (d > 0.0) over += d * area;
    }
    return static_cast<float>(over / movableAreaScaled_);
}

void BinGrid::gatherForce(const PlaceDB& db, float* force) const {
    const int total = db.totalNodes();
    std::fill(force, force + 2 * static_cast<size_t>(total), 0.f);

#pragma omp parallel for schedule(static)
    for (int i = 0; i < total; ++i) {
        if (db.isFixed(i) || db.isNI(i)) continue;

        const Footprint f = footprintOf(db, i);
        const Rect rect{f.lx, f.ly, f.hx, f.hy};
        // 电荷量 q 的缩放必须与 accumulate 完全一致，否则场与力不自洽
        //（easyPlace 正是在这里只缩放了场、没缩放力，见 02 §2.3）。
        // 走同一个 chargeWeight() 从结构上杜绝分叉。
        const float w = chargeWeight(db, i, f.scale);

        float fx = 0.f, fy = 0.f;
        for (int bx = f.bx0; bx <= f.bx1; ++bx) {
            const float blx = originX_ + bx * stepX_;
            for (int by = f.by0; by <= f.by1; ++by) {
                const float bly = originY_ + by * stepY_;
                const Rect bin{blx, bly, blx + stepX_, bly + stepY_};
                const float q = overlapArea(bin, rect) * w;
                if (q <= 0.f) continue;
                const size_t idx = static_cast<size_t>(bx) * dim_ + by;
                fx += q * fieldX_[idx];
                fy += q * fieldY_[idx];
            }
        }
        force[i] = fx;
        force[total + i] = fy;
    }
}

double BinGrid::totalCharge(const PlaceDB& db) const {
    const int total = db.totalNodes();
    double sum = 0.0;
    for (int i = 0; i < total; ++i) {
        if (db.isFixed(i) || db.isNI(i)) continue;
        const Footprint f = footprintOf(db, i);
        const Rect rect{f.lx, f.ly, f.hx, f.hy};
        const float w = chargeWeight(db, i, f.scale);
        for (int bx = f.bx0; bx <= f.bx1; ++bx) {
            const float blx = originX_ + bx * stepX_;
            for (int by = f.by0; by <= f.by1; ++by) {
                const float bly = originY_ + by * stepY_;
                sum += static_cast<double>(
                    overlapArea(Rect{blx, bly, blx + stepX_, bly + stepY_}, rect) * w);
            }
        }
    }
    return sum;
}

}  // namespace sp
