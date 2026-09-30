#include "lg/macro_sa.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "db/geometry.h"
#include "db/hpwl.h"
#include "db/place_db.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/metrics.h"
#include "util/rng.h"
#include "util/timer.h"

namespace sp {
namespace {

/// 一个宏在某条 net 上的【局部引脚包围盒】：相对宏中心的偏移范围。
/// 宏做刚体平移时这个盒子不变，绝对包围盒 = 宏中心 + 局部盒，这是整个增量
/// 计算的支点。
struct LocalBox {
    int   net;
    float lx, ly, hx, hy;
};

/// 增量线长模型。
///
/// mLG 期间标准单元与 filler 全部冻结，只有宏在动。因此每条 net 的包围盒可以拆成
/// 两部分：非宏引脚贡献的【静态盒】（一次算好、永不变），加上各宏的局部盒平移后的
/// 并集。移动一个宏时，只需重算它所在 net 的并集，成本是 O(该 net 上的宏数)——
/// 实测 MMS adaptec1 上约 1.5，而朴素地按引脚重算每次要访问 36751 个引脚，差约 150 倍。
class IncrementalWirelength {
public:
    void build(const PlaceDB& db, const std::vector<int>& macros) {
        const int numNets = db.numNets;
        isMacroPin_.assign(static_cast<size_t>(db.numPins), 0);
        macroSlot_.assign(static_cast<size_t>(db.totalNodes()), -1);
        for (size_t s = 0; s < macros.size(); ++s) macroSlot_[static_cast<size_t>(macros[s])] = static_cast<int>(s);

        for (int m : macros)
            for (int k = db.node2pin_start[m]; k < db.node2pin_start[m + 1]; ++k)
                isMacroPin_[static_cast<size_t>(db.flat_node2pin[k])] = 1;

        // ---- 静态盒：只由非宏引脚决定
        constexpr float kInf = std::numeric_limits<float>::infinity();
        staticBox_.assign(static_cast<size_t>(numNets), Rect{kInf, kInf, -kInf, -kInf});
        touched_.assign(static_cast<size_t>(numNets), 0);
        for (int n = 0; n < numNets; ++n) {
            Rect& b = staticBox_[static_cast<size_t>(n)];
            bool anyMacro = false;
            for (int k = db.net2pin_start[n]; k < db.net2pin_start[n + 1]; ++k) {
                const int p = db.flat_net2pin[k];
                if (isMacroPin_[static_cast<size_t>(p)]) {
                    anyMacro = true;
                    continue;
                }
                const float x = db.pinX(p), y = db.pinY(p);
                b.lx = std::min(b.lx, x);
                b.hx = std::max(b.hx, x);
                b.ly = std::min(b.ly, y);
                b.hy = std::max(b.hy, y);
            }
            touched_[static_cast<size_t>(n)] = anyMacro ? 1 : 0;
        }

        // ---- 每个宏在每条 net 上的局部盒（CSR：macro -> LocalBox 列表）
        macroBoxStart_.assign(macros.size() + 1, 0);
        std::vector<LocalBox> flat;
        std::vector<int> netSeen(static_cast<size_t>(numNets), -1);
        std::vector<int> netLocal(static_cast<size_t>(numNets), -1);
        for (size_t s = 0; s < macros.size(); ++s) {
            const int m = macros[s];
            const size_t begin = flat.size();
            for (int k = db.node2pin_start[m]; k < db.node2pin_start[m + 1]; ++k) {
                const int p = db.flat_node2pin[k];
                const int n = db.pin2net[static_cast<size_t>(p)];
                const float ox = db.pin_offset_x[static_cast<size_t>(p)];
                const float oy = db.pin_offset_y[static_cast<size_t>(p)];
                if (netSeen[static_cast<size_t>(n)] != static_cast<int>(s)) {
                    netSeen[static_cast<size_t>(n)] = static_cast<int>(s);
                    netLocal[static_cast<size_t>(n)] = static_cast<int>(flat.size());
                    flat.push_back(LocalBox{n, ox, oy, ox, oy});
                } else {
                    LocalBox& lb = flat[static_cast<size_t>(netLocal[static_cast<size_t>(n)])];
                    lb.lx = std::min(lb.lx, ox);
                    lb.hx = std::max(lb.hx, ox);
                    lb.ly = std::min(lb.ly, oy);
                    lb.hy = std::max(lb.hy, oy);
                }
            }
            macroBoxStart_[s + 1] = static_cast<int>(flat.size());
            (void)begin;
        }
        macroBox_ = std::move(flat);

        // ---- 反向索引：net -> 该 net 上的宏（增量重算时要遍历）
        std::vector<int> cnt(static_cast<size_t>(numNets) + 1, 0);
        for (size_t s = 0; s < macros.size(); ++s)
            for (int j = macroBoxStart_[s]; j < macroBoxStart_[s + 1]; ++j)
                ++cnt[static_cast<size_t>(macroBox_[static_cast<size_t>(j)].net) + 1];
        netMacroStart_.assign(static_cast<size_t>(numNets) + 1, 0);
        for (int n = 0; n < numNets; ++n)
            netMacroStart_[static_cast<size_t>(n) + 1] =
                netMacroStart_[static_cast<size_t>(n)] + cnt[static_cast<size_t>(n) + 1];
        netMacroBox_.assign(static_cast<size_t>(netMacroStart_.back()), 0);
        netMacroSlot_.assign(static_cast<size_t>(netMacroStart_.back()), 0);
        std::vector<int> fill(netMacroStart_.begin(), netMacroStart_.end() - 1);
        for (size_t s = 0; s < macros.size(); ++s) {
            for (int j = macroBoxStart_[s]; j < macroBoxStart_[s + 1]; ++j) {
                const int n = macroBox_[static_cast<size_t>(j)].net;
                const int slot = fill[static_cast<size_t>(n)]++;
                netMacroBox_[static_cast<size_t>(slot)] = j;
                netMacroSlot_[static_cast<size_t>(slot)] = static_cast<int>(s);
            }
        }

        weight_.assign(static_cast<size_t>(numNets), 1.f);
        for (int n = 0; n < numNets; ++n) weight_[static_cast<size_t>(n)] = db.net_weight[static_cast<size_t>(n)];
        cache_.assign(static_cast<size_t>(numNets), 0.0);
    }

    /// 含宏的那部分线长（不含宏的 net 在 mLG 期间恒定，不必纳入）。
    double total(const std::vector<float>& cx, const std::vector<float>& cy,
                 const std::vector<uint8_t>& flipped) {
        double sum = 0.0;
        for (size_t n = 0; n < touched_.size(); ++n) {
            if (!touched_[n]) continue;
            cache_[n] = netHpwl(static_cast<int>(n), cx, cy, flipped);
            sum += cache_[n];
        }
        return sum;
    }

    /// 只重算受某个宏影响的 net，返回线长增量。不改 cache_（试探用）。
    double deltaForMacro(int slot, const std::vector<float>& cx, const std::vector<float>& cy,
                         const std::vector<uint8_t>& flipped, std::vector<int>& dirtyNets,
                         std::vector<double>& newVals) const {
        dirtyNets.clear();
        newVals.clear();
        double d = 0.0;
        for (int j = macroBoxStart_[static_cast<size_t>(slot)];
             j < macroBoxStart_[static_cast<size_t>(slot) + 1]; ++j) {
            const int n = macroBox_[static_cast<size_t>(j)].net;
            const double v = netHpwl(n, cx, cy, flipped);
            dirtyNets.push_back(n);
            newVals.push_back(v);
            d += v - cache_[static_cast<size_t>(n)];
        }
        return d;
    }

    void commit(const std::vector<int>& dirtyNets, const std::vector<double>& newVals) {
        for (size_t i = 0; i < dirtyNets.size(); ++i)
            cache_[static_cast<size_t>(dirtyNets[i])] = newVals[i];
    }

    int netCount() const { return static_cast<int>(touched_.size()); }

private:
    double netHpwl(int n, const std::vector<float>& cx, const std::vector<float>& cy,
                   const std::vector<uint8_t>& flipped) const {
        Rect b = staticBox_[static_cast<size_t>(n)];
        for (int t = netMacroStart_[static_cast<size_t>(n)];
             t < netMacroStart_[static_cast<size_t>(n) + 1]; ++t) {
            const LocalBox& lb = macroBox_[static_cast<size_t>(netMacroBox_[static_cast<size_t>(t)])];
            const int s = netMacroSlot_[static_cast<size_t>(t)];
            const float x = cx[static_cast<size_t>(s)], y = cy[static_cast<size_t>(s)];
            // 水平翻转把偏移区间 [lx,hx] 镜像成 [−hx,−lx]
            const bool f = flipped[static_cast<size_t>(s)] != 0;
            const float ox0 = f ? -lb.hx : lb.lx;
            const float ox1 = f ? -lb.lx : lb.hx;
            b.lx = std::min(b.lx, x + ox0);
            b.hx = std::max(b.hx, x + ox1);
            b.ly = std::min(b.ly, y + lb.ly);
            b.hy = std::max(b.hy, y + lb.hy);
        }
        if (!(b.hx >= b.lx) || !(b.hy >= b.ly)) return 0.0;
        return static_cast<double>(weight_[static_cast<size_t>(n)]) *
               (static_cast<double>(b.hx - b.lx) + static_cast<double>(b.hy - b.ly));
    }

    std::vector<uint8_t>  isMacroPin_;
    std::vector<int>      macroSlot_;
    std::vector<Rect>     staticBox_;
    std::vector<uint8_t>  touched_;
    std::vector<LocalBox> macroBox_;
    std::vector<int>      macroBoxStart_;
    std::vector<int>      netMacroStart_, netMacroBox_, netMacroSlot_;
    std::vector<float>    weight_;
    std::vector<double>   cache_;
};

/// 重叠模型：宏-宏两两，加上宏与固定阻挡（非零面积的固定节点）。
///
/// **带均匀网格索引。** 朴素实现每次查询都要扫过全部 M 个宏，而 SA 的移动次数
/// 又正比于 M，总复杂度是 O(M²)：实测 63 个宏时单次移动 2.03 µs、1329 个宏时
/// 5.32 µs，外推到 newblue7 量级（约 2.5 万个宏）要跑两个多小时，M6 直接不可行。
/// 加索引后单次查询只看落在该宏包围盒所覆盖网格里的少数候选。
///
/// 两条设计约束：
///   * **坐标与索引由本类统一持有**，只经 setPos() 修改。若让调用方另存一份坐标，
///     二者一旦失步就会漏掉重叠，而这种错误不会崩、只会悄悄输出非法布局。
///   * **候选要按下标排序后再累加**。网格桶里的次序取决于插入删除历史，
///     不排序的话同一份布局两次运行可能得到最后几位不同的重叠面积（铁律 7）。
///
/// 非线程安全（内部有查询用的临时缓冲）；SA 本身是串行的。
class OverlapModel {
public:
    void build(const PlaceDB& db, const std::vector<int>& macros, const std::vector<float>& x0,
               const std::vector<float>& y0) {
        const size_t n = macros.size();
        w_.resize(n);
        h_.resize(n);
        cx_.assign(x0.begin(), x0.end());
        cy_.assign(y0.begin(), y0.end());
        for (size_t s = 0; s < n; ++s) {
            w_[s] = db.node_w[static_cast<size_t>(macros[s])];
            h_[s] = db.node_h[static_cast<size_t>(macros[s])];
        }

        // 零面积的 terminal_NI（MMS 把 I/O pad 全部置零）不构成阻挡，必须排除，
        // 否则会引入零面积矩形并让阻挡列表凭空膨胀几百项。
        for (int i = db.numMovable; i < db.numNodes; ++i) {
            if (db.isNI(i)) continue;
            if (db.node_w[static_cast<size_t>(i)] <= 0.f || db.node_h[static_cast<size_t>(i)] <= 0.f)
                continue;
            blockages_.push_back(db.box(i));
        }

        // 网格维度取 ~sqrt(2M)，即平均每格半个宏。上限 128 以免格子数失控；
        // 个别超大宏会跨很多格，这是可接受的——它们数量少。
        const double target = std::sqrt(2.0 * static_cast<double>(std::max<size_t>(n, 1)));
        dim_ = std::min(128, std::max(4, static_cast<int>(target + 0.5)));
        ox_ = db.coreRegion.lx;
        oy_ = db.coreRegion.ly;
        cellW_ = std::max(1e-3f, db.coreRegion.width() / static_cast<float>(dim_));
        cellH_ = std::max(1e-3f, db.coreRegion.height() / static_cast<float>(dim_));

        cells_.assign(static_cast<size_t>(dim_) * static_cast<size_t>(dim_), {});
        blockCells_.assign(static_cast<size_t>(dim_) * static_cast<size_t>(dim_), {});
        for (size_t s = 0; s < n; ++s) insert(static_cast<int>(s));
        for (size_t b = 0; b < blockages_.size(); ++b) {
            int x0i, x1i, y0i, y1i;
            cellRange(blockages_[b], x0i, x1i, y0i, y1i);
            for (int gx = x0i; gx <= x1i; ++gx)
                for (int gy = y0i; gy <= y1i; ++gy)
                    blockCells_[cellIdx(gx, gy)].push_back(static_cast<int>(b));
        }
        stamp_.assign(n, -1);
        blockStamp_.assign(blockages_.size(), -1);
    }

    float x(int s) const { return cx_[static_cast<size_t>(s)]; }
    float y(int s) const { return cy_[static_cast<size_t>(s)]; }
    const std::vector<float>& xs() const { return cx_; }
    const std::vector<float>& ys() const { return cy_; }

    /// 位置与索引的唯一写入口。
    void setPos(int s, float nx, float ny) {
        erase(s);
        cx_[static_cast<size_t>(s)] = nx;
        cy_[static_cast<size_t>(s)] = ny;
        insert(s);
    }

    /// 宏 s 若位于 (x,y)，与其他宏和固定阻挡的重叠面积之和。
    double at(int s, float px, float py) const {
        const Rect a = rectOf(s, px, py);
        int x0i, x1i, y0i, y1i;
        cellRange(a, x0i, x1i, y0i, y1i);

        cand_.clear();
        ++tick_;
        for (int gx = x0i; gx <= x1i; ++gx) {
            for (int gy = y0i; gy <= y1i; ++gy) {
                for (int t : cells_[cellIdx(gx, gy)]) {
                    if (t == s) continue;
                    if (stamp_[static_cast<size_t>(t)] == tick_) continue;   // 跨格去重
                    stamp_[static_cast<size_t>(t)] = tick_;
                    cand_.push_back(t);
                }
            }
        }
        // 桶内次序取决于插入删除历史，必须排序后再累加，否则浮点和不可复现
        std::sort(cand_.begin(), cand_.end());

        double sum = 0.0;
        for (int t : cand_) sum += overlapArea(a, rectOf(t, cx_[static_cast<size_t>(t)],
                                                         cy_[static_cast<size_t>(t)]));

        if (!blockages_.empty()) {
            bcand_.clear();
            for (int gx = x0i; gx <= x1i; ++gx) {
                for (int gy = y0i; gy <= y1i; ++gy) {
                    for (int b : blockCells_[cellIdx(gx, gy)]) {
                        if (blockStamp_[static_cast<size_t>(b)] == tick_) continue;
                        blockStamp_[static_cast<size_t>(b)] = tick_;
                        bcand_.push_back(b);
                    }
                }
            }
            std::sort(bcand_.begin(), bcand_.end());
            for (int b : bcand_) sum += overlapArea(a, blockages_[static_cast<size_t>(b)]);
        }
        return sum;
    }

    /// 全局重叠总量。按下标序两两求和，与索引无关，可作为 at() 的独立对照。
    double total() const {
        double sum = 0.0;
        for (size_t s = 0; s < w_.size(); ++s) {
            const Rect a = rectOf(static_cast<int>(s), cx_[s], cy_[s]);
            for (size_t t = s + 1; t < w_.size(); ++t)
                sum += overlapArea(a, rectOf(static_cast<int>(t), cx_[t], cy_[t]));
            for (const Rect& b : blockages_) sum += overlapArea(a, b);
        }
        return sum;
    }

    Rect rectOf(int s, float px, float py) const {
        const float hw = 0.5f * w_[static_cast<size_t>(s)];
        const float hh = 0.5f * h_[static_cast<size_t>(s)];
        return {px - hw, py - hh, px + hw, py + hh};
    }

    float width(int s) const { return w_[static_cast<size_t>(s)]; }
    float height(int s) const { return h_[static_cast<size_t>(s)]; }
    int   count() const { return static_cast<int>(w_.size()); }
    int   gridDim() const { return dim_; }

    /// 不变量自检：索引版 at() 必须与全扫一致。
    ///
    /// 索引失效是**静默**错误——漏掉候选只会让 at() 少算重叠，SA 与贪心修复
    /// 会据此把宏放到自以为干净的位置上，最后输出一份非法布局，不崩不报。
    /// 这里花一次 O(M²) 把它钉死（只在 mLG 开头做一次，相对整体耗时可忽略）。
    /// 返回最大绝对偏差。
    double selfCheck() const {
        double worst = 0.0;
        const int n = static_cast<int>(w_.size());
        for (int s = 0; s < n; ++s) {
            const Rect a = rectOf(s, cx_[static_cast<size_t>(s)], cy_[static_cast<size_t>(s)]);
            double brute = 0.0;
            for (int t = 0; t < n; ++t) {
                if (t == s) continue;
                brute += overlapArea(a, rectOf(t, cx_[static_cast<size_t>(t)],
                                               cy_[static_cast<size_t>(t)]));
            }
            for (const Rect& b : blockages_) brute += overlapArea(a, b);
            worst = std::max(worst, std::fabs(brute - at(s, cx_[static_cast<size_t>(s)],
                                                         cy_[static_cast<size_t>(s)])));
        }
        return worst;
    }

private:
    size_t cellIdx(int gx, int gy) const {
        return static_cast<size_t>(gx) * static_cast<size_t>(dim_) + static_cast<size_t>(gy);
    }
    void cellRange(const Rect& r, int& x0i, int& x1i, int& y0i, int& y1i) const {
        x0i = std::clamp(static_cast<int>((r.lx - ox_) / cellW_), 0, dim_ - 1);
        x1i = std::clamp(static_cast<int>((r.hx - ox_) / cellW_), 0, dim_ - 1);
        y0i = std::clamp(static_cast<int>((r.ly - oy_) / cellH_), 0, dim_ - 1);
        y1i = std::clamp(static_cast<int>((r.hy - oy_) / cellH_), 0, dim_ - 1);
    }
    void insert(int s) {
        int x0i, x1i, y0i, y1i;
        cellRange(rectOf(s, cx_[static_cast<size_t>(s)], cy_[static_cast<size_t>(s)]), x0i, x1i,
                  y0i, y1i);
        for (int gx = x0i; gx <= x1i; ++gx)
            for (int gy = y0i; gy <= y1i; ++gy) cells_[cellIdx(gx, gy)].push_back(s);
    }
    void erase(int s) {
        int x0i, x1i, y0i, y1i;
        cellRange(rectOf(s, cx_[static_cast<size_t>(s)], cy_[static_cast<size_t>(s)]), x0i, x1i,
                  y0i, y1i);
        for (int gx = x0i; gx <= x1i; ++gx) {
            for (int gy = y0i; gy <= y1i; ++gy) {
                std::vector<int>& v = cells_[cellIdx(gx, gy)];
                for (size_t k = 0; k < v.size(); ++k) {
                    if (v[k] != s) continue;
                    v[k] = v.back();   // 顺序被打乱，故 at() 必须排序后再累加
                    v.pop_back();
                    break;
                }
            }
        }
    }

    std::vector<float> w_, h_, cx_, cy_;
    std::vector<Rect>  blockages_;

    int   dim_ = 1;
    float ox_ = 0.f, oy_ = 0.f, cellW_ = 1.f, cellH_ = 1.f;
    std::vector<std::vector<int>> cells_, blockCells_;

    mutable std::vector<int> stamp_, blockStamp_, cand_, bcand_;
    mutable int tick_ = 0;
};

/// 把宏的中心坐标对齐到 site / row 栅格，并夹回 core。
///
/// 在【提议阶段】就对齐，而不是 SA 结束后统一对齐：事后对齐会把已经消除的重叠
/// 重新引入（每个宏最多平移半个行高），那时再修就没有代价函数指引了。
struct Snapper {
    float coreLx, coreLy, coreHx, coreHy, site, row;

    void operator()(const OverlapModel& om, int s, float& x, float& y) const {
        const float hw = 0.5f * om.width(s), hh = 0.5f * om.height(s);
        float lx = x - hw, ly = y - hh;
        lx = coreLx + std::round((lx - coreLx) / site) * site;
        ly = coreLy + std::round((ly - coreLy) / row) * row;
        lx = std::min(std::max(lx, coreLx), std::max(coreLx, coreHx - 2.f * hw));
        ly = std::min(std::max(ly, coreLy), std::max(coreLy, coreHy - 2.f * hh));
        x = lx + hw;
        y = ly + hh;
    }
};

}  // namespace

MacroSaResult legalizeMacros(PlaceDB& db, const Config& cfg, MetricsSink& sink) {
    MacroSaResult r;

    std::vector<int> macros;
    for (int i = 0; i < db.numMovable; ++i)
        if (db.isMacro(i) && !db.isFixed(i)) macros.push_back(i);
    r.numMacros = static_cast<int>(macros.size());
    if (macros.empty()) {
        SP_INFO("mLG: 无可移动宏，跳过（ISPD2005/2006 原版数据上这是正常情况）");
        return r;
    }

    const size_t nM = macros.size();
    std::vector<float>   x0(nM), y0(nM);
    std::vector<uint8_t> flipped(nM, 0);
    for (size_t s = 0; s < nM; ++s) {
        x0[s] = db.node_x[static_cast<size_t>(macros[s])];
        y0[s] = db.node_y[static_cast<size_t>(macros[s])];
    }

    OverlapModel om;
    om.build(db, macros, x0, y0);
    IncrementalWirelength iwl;
    iwl.build(db, macros);

    Snapper snap{db.coreRegion.lx, db.coreRegion.ly, db.coreRegion.hx, db.coreRegion.hy,
                 db.rows.empty() ? 1.f : db.rows[0].step,
                 db.rowHeight > 0.f ? db.rowHeight : 1.f};
    for (size_t s = 0; s < nM; ++s) {
        float px = om.x(static_cast<int>(s)), py = om.y(static_cast<int>(s));
        snap(om, static_cast<int>(s), px, py);
        om.setPos(static_cast<int>(s), px, py);
    }
    // cx / cy 一律经由 om 读取：坐标与网格索引只有 setPos 一个写入口，
    // 二者失步会漏掉重叠，而那是一种不会崩、只会静默输出非法布局的错误。
    const std::vector<float>& cx = om.xs();
    const std::vector<float>& cy = om.ys();

    // 索引自检。M 很大时 O(M²) 也会变贵，超过 8000 个宏就跳过——
    // 那个规模下真要排查，用小用例复现即可。
    if (nM <= 8000) {
        const double dev = om.selfCheck();
        // 面积量级可达 1e8，float 求和的相对误差约 1e-7，放到 1e-3 的绝对容差
        if (dev > 1e-3)
            SP_ERROR("mLG: 空间索引与全扫不一致（最大偏差 %.6g）——重叠会被低估，"
                     "输出布局可能非法", dev);
        else
            SP_INFO("mLG: 空间索引自检通过（%dx%d 网格，最大偏差 %.3g）", om.gridDim(),
                    om.gridDim(), dev);
    }

    r.hpwlBefore = computeHPWL(db);
    r.overlapBefore = om.total();

    double macroArea = 0.0;
    for (int m : macros) macroArea += db.area(m);
    double wl = iwl.total(cx, cy, flipped);

    // 两项量纲不同（面积 vs 长度），各自除以自身参考量归一后再加权，
    // 这样 α/β 才是可解释的"相对重要性"而不是需要反复试的魔数。
    const double refOverlap = std::max(macroArea, 1.0);
    const double refWl = std::max(wl, 1.0);
    constexpr double kAlpha = 8.0;   // 重叠权重远大于线长：合法性是硬约束
    constexpr double kBeta = 1.0;

    double overlap = r.overlapBefore;
    auto cost = [&](double ov, double w) {
        return kAlpha * ov / refOverlap + kBeta * w / refWl;
    };

    const int moves = (cfg.macro_sa_moves > 0)
                          ? cfg.macro_sa_moves
                          : std::max(20000, static_cast<int>(nM) * 3000);
    Rng rng(cfg.random_seed ^ 0x9E3779B97F4A7C15ull);

    // 初温按"接受一次典型恶化的概率约为 init_accept"来定，避免拍脑袋给常数
    const double t0 = -1.0 / std::log(std::max(1e-6f, cfg.macro_sa_init_accept));
    const double tempRatio = std::max(1e-12f, cfg.macro_sa_temp_ratio);
    const float coreW = db.coreRegion.width(), coreH = db.coreRegion.height();

    // SA 是随机过程，最后一个被接受的解不一定是见过的最好解——末期仍会接受恶化。
    // 必须单独记住最优解并在结束时回到它，否则无重叠的输入进来反而会被搞坏。
    std::vector<float>   bestX = cx, bestY = cy;
    std::vector<uint8_t> bestFlip = flipped;
    double bestCost = cost(overlap, wl);

    std::vector<int>    dirty;
    std::vector<double> newVals;
    Timer timer;

    for (int it = 0; it < moves; ++it) {
        const double progress = static_cast<double>(it) / static_cast<double>(moves);
        const double temp = t0 * std::pow(tempRatio, progress);
        // 步长随退火收缩：前期全局重排，后期精细微调
        const float span = static_cast<float>(0.5 * (1.0 - progress) + 0.02);

        const int s = rng.uniformInt(0, static_cast<int>(nM) - 1);
        const int op = rng.uniformInt(0, 9);   // 0-6 平移，7-8 交换，9 翻转

        const float oldX = cx[static_cast<size_t>(s)], oldY = cy[static_cast<size_t>(s)];
        const uint8_t oldFlip = flipped[static_cast<size_t>(s)];
        int s2 = -1;
        float oldX2 = 0.f, oldY2 = 0.f;

        double dOverlap = 0.0;

        if (op <= 6) {
            float nx = oldX + rng.uniform(-span, span) * coreW;
            float ny = oldY + rng.uniform(-span, span) * coreH;
            snap(om, s, nx, ny);
            dOverlap = om.at(s, nx, ny) - om.at(s, oldX, oldY);
            om.setPos(s, nx, ny);
        } else if (op <= 8 && nM >= 2) {
            do {
                s2 = rng.uniformInt(0, static_cast<int>(nM) - 1);
            } while (s2 == s);
            oldX2 = cx[static_cast<size_t>(s2)];
            oldY2 = cy[static_cast<size_t>(s2)];
            // at(s) + at(s2) 会把 (s,s2) 这一对算两遍，而 overlap 记的是每对只算
            // 一次的总量。必须把这一对减掉，否则增量与真实总量长期漂移。
            const double before = om.at(s, oldX, oldY) + om.at(s2, oldX2, oldY2) -
                                  overlapArea(om.rectOf(s, oldX, oldY),
                                              om.rectOf(s2, oldX2, oldY2));
            float ax = oldX2, ay = oldY2, bx = oldX, by = oldY;
            snap(om, s, ax, ay);
            snap(om, s2, bx, by);
            om.setPos(s, ax, ay);
            om.setPos(s2, bx, by);
            const double after = om.at(s, ax, ay) + om.at(s2, bx, by) -
                                 overlapArea(om.rectOf(s, ax, ay), om.rectOf(s2, bx, by));
            dOverlap = after - before;
        } else {
            // 翻转只改引脚位置，不改外形，因此重叠不变
            flipped[static_cast<size_t>(s)] = static_cast<uint8_t>(1 - oldFlip);
        }

        double dWl = iwl.deltaForMacro(s, cx, cy, flipped, dirty, newVals);
        std::vector<int>    dirty2;
        std::vector<double> newVals2;
        if (s2 >= 0) dWl += iwl.deltaForMacro(s2, cx, cy, flipped, dirty2, newVals2);

        const double dCost = cost(overlap + dOverlap, wl + dWl) - cost(overlap, wl);
        const bool accept =
            (dCost <= 0.0) || (temp > 0.0 && rng.uniform(0.f, 1.f) < std::exp(-dCost / temp));

        if (accept) {
            overlap += dOverlap;
            wl += dWl;
            iwl.commit(dirty, newVals);
            if (s2 >= 0) iwl.commit(dirty2, newVals2);
            ++r.accepted;
            const double c = cost(overlap, wl);
            if (c < bestCost) {
                bestCost = c;
                bestX = cx;
                bestY = cy;
                bestFlip = flipped;
            }
        } else {
            om.setPos(s, oldX, oldY);
            flipped[static_cast<size_t>(s)] = oldFlip;
            if (s2 >= 0) om.setPos(s2, oldX2, oldY2);
        }
    }
    r.moves = moves;

    for (size_t s = 0; s < nM; ++s) om.setPos(static_cast<int>(s), bestX[s], bestY[s]);
    flipped = bestFlip;
    wl = iwl.total(cx, cy, flipped);
    overlap = om.total();

    // ---- 贪心修复：SA 是概率算法，不保证重叠恰好为 0，而"零重叠"是硬验收项。
    //
    // 对仍有重叠的宏，在其周围搜索落脚点。两个要点：
    //   * 优先挑【零重叠且位移最小】的位置；若窗口内根本不存在零重叠位置，
    //     退而取【重叠最小】的位置。只接受零重叠解的话，密排设计（MMS adaptec4
    //     的宏占 core 面积 48.6%）会在第一轮就全体卡死，后续轮次毫无进展。
    //   * 每轮按当前重叠量【从大到小】处理，先给最堵的宏挪窝，收敛快得多。
    //     排序键取自本轮开始时的快照并以下标破平，保证顺序可复现。
    std::vector<size_t> order(nM);
    for (size_t s = 0; s < nM; ++s) order[s] = s;

    for (int pass = 0; pass < 12 && overlap > 0.0; ++pass) {
        std::vector<double> ov(nM);
        for (size_t s = 0; s < nM; ++s) ov[s] = om.at(static_cast<int>(s), cx[s], cy[s]);
        std::stable_sort(order.begin(), order.end(),
                         [&ov](size_t a, size_t b) { return ov[a] > ov[b]; });

        for (size_t s : order) {
            const double cur = om.at(static_cast<int>(s), cx[s], cy[s]);
            if (cur <= 0.0) continue;

            // 位移是这里的第一等公民：宏刚从 mGP 的线长最优位置上下来，挪得越远
            // 线长损失越大。实测把"取重叠最小位置"放任不管（不限位移）能把残留
            // 重叠再压低一个数量级，代价却是 mLG 的 HPWL 增量从 +16% 飙到 +35%，
            // 最终 HPWL 劣化 17%——这笔交易不划算。故按下面的优先级挑落脚点：
            //
            //   1. 零重叠位置中【位移最小】的；
            //   2. 窗口内没有零重叠位置时，才退而取重叠更小的，且【位移受限】。
            float  zeroX = 0.f, zeroY = 0.f;
            double zeroDist = std::numeric_limits<double>::infinity();
            bool   haveZero = false;

            float  fbX = 0.f, fbY = 0.f;
            double fbOv = cur;
            bool   haveFallback = false;

            const float stepX = std::max(om.width(static_cast<int>(s)) * 0.25f, snap.site);
            const float stepY = std::max(om.height(static_cast<int>(s)) * 0.25f, snap.row);
            const int   span = 12 + 2 * pass;
            const double maxDisp =
                static_cast<double>(2 + pass) *
                std::max(om.width(static_cast<int>(s)), om.height(static_cast<int>(s)));

            for (int ry = -span; ry <= span; ++ry) {
                for (int rx = -span; rx <= span; ++rx) {
                    float nx = cx[s] + static_cast<float>(rx) * stepX;
                    float ny = cy[s] + static_cast<float>(ry) * stepY;
                    snap(om, static_cast<int>(s), nx, ny);
                    const double d = std::hypot(static_cast<double>(nx - cx[s]),
                                                static_cast<double>(ny - cy[s]));
                    const double o = om.at(static_cast<int>(s), nx, ny);
                    if (o <= 0.0) {
                        if (d < zeroDist) {
                            zeroDist = d;
                            zeroX = nx;
                            zeroY = ny;
                            haveZero = true;
                        }
                    } else if (!haveZero && d <= maxDisp && o < fbOv - 1e-9) {
                        fbOv = o;
                        fbX = nx;
                        fbY = ny;
                        haveFallback = true;
                    }
                }
            }

            if (haveZero) {
                om.setPos(static_cast<int>(s), zeroX, zeroY);
                ++r.repaired;
            } else if (haveFallback) {
                om.setPos(static_cast<int>(s), fbX, fbY);
                ++r.repaired;
            }
        }
        const double after = om.total();
        if (after >= overlap) {   // 本轮毫无进展，换下面的分离平移去啃
            overlap = after;
            break;
        }
        overlap = after;
    }

    // ---- 最小分离平移：网格搜索啃不动的零头交给它。
    //
    // 网格搜索是在固定步长的候选点里挑，一个只差几个单位就能分开的宏，
    // 可能在整张候选网格上都找不到零重叠点。这里改为**直接算出让它脱离
    // 当前最大重叠邻居所需的最小平移**（左/右/下/上四选一），一次到位。
    // 可能引入新的重叠，故迭代若干轮并只接受总量下降的结果。
    for (int pass = 0; pass < 30 && overlap > 0.0; ++pass) {
        bool moved = false;
        for (size_t s = 0; s < nM; ++s) {
            const int si = static_cast<int>(s);
            if (om.at(si, cx[s], cy[s]) <= 0.0) continue;
            const Rect a = om.rectOf(si, cx[s], cy[s]);

            // 找重叠最大的那个邻居
            int worst = -1;
            double worstOv = 0.0;
            for (size_t t = 0; t < nM; ++t) {
                if (t == s) continue;
                const double o = overlapArea(a, om.rectOf(static_cast<int>(t), cx[t], cy[t]));
                if (o > worstOv) { worstOv = o; worst = static_cast<int>(t); }
            }
            if (worst < 0) continue;
            const Rect b = om.rectOf(worst, cx[static_cast<size_t>(worst)],
                                     cy[static_cast<size_t>(worst)]);

            const float dxL = b.lx - a.hx, dxR = b.hx - a.lx;   // 推到左侧 / 右侧
            const float dyD = b.ly - a.hy, dyU = b.hy - a.ly;   // 推到下方 / 上方
            const float cand[4][2] = {{dxL, 0.f}, {dxR, 0.f}, {0.f, dyD}, {0.f, dyU}};

            double bestOv = om.at(si, cx[s], cy[s]);
            float bestPx = cx[s], bestPy = cy[s];
            for (const auto& d : cand) {
                float nx = cx[s] + d[0], ny = cy[s] + d[1];
                snap(om, si, nx, ny);
                const double o = om.at(si, nx, ny);
                if (o < bestOv - 1e-9) { bestOv = o; bestPx = nx; bestPy = ny; }
            }
            if (bestPx != cx[s] || bestPy != cy[s]) {
                om.setPos(si, bestPx, bestPy);
                ++r.repaired;
                moved = true;
            }
        }
        const double after = om.total();
        if (!moved || after >= overlap) { overlap = after; break; }
        overlap = after;
    }

    // ---- 写回 db，并把宏钉死，后续 cGP 不再移动它们
    for (size_t s = 0; s < nM; ++s) {
        const int m = macros[s];
        db.node_x[static_cast<size_t>(m)] = cx[s];
        db.node_y[static_cast<size_t>(m)] = cy[s];
        if (flipped[s]) {
            for (int k = db.node2pin_start[m]; k < db.node2pin_start[m + 1]; ++k) {
                const int p = db.flat_node2pin[k];
                db.pin_offset_x[static_cast<size_t>(p)] = -db.pin_offset_x[static_cast<size_t>(p)];
            }
            // BookShelf 方向：水平镜像即 N<->FN(0<->4)、S<->FS(2<->6)
            uint8_t& o = db.node_orient[static_cast<size_t>(m)];
            if (o == 0) o = 4;
            else if (o == 4) o = 0;
            else if (o == 2) o = 6;
            else if (o == 6) o = 2;
        }
        db.node_flags[static_cast<size_t>(m)] |= F_FIXED;
    }

    r.overlapAfter = om.total();
    r.hpwlAfter = computeHPWL(db);

    const double ms = timer.elapsedMs();
    sink.recordDuration("mLG", ms);
    SP_INFO("mLG: %d 个宏，%d 次移动接受 %d（%.1f%%），贪心修复 %d 次",
            r.numMacros, r.moves, r.accepted,
            100.0 * r.accepted / std::max(1, r.moves), r.repaired);
    SP_INFO("mLG: 重叠 %.6g -> %.6g（占宏总面积 %.4f%%），HPWL %.6g -> %.6g (%+.2f%%)，%.1f ms",
            r.overlapBefore, r.overlapAfter, 100.0 * r.overlapAfter / macroArea, r.hpwlBefore,
            r.hpwlAfter, 100.0 * (r.hpwlAfter - r.hpwlBefore) / std::max(1.0, r.hpwlBefore), ms);
    return r;
}

}  // namespace sp
