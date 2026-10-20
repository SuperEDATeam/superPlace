// T13：FrameMap——同阶段按 iter 最近邻映射，帧↔行双向一致；未知阶段比例兜底。
#include "test_util.h"

#include <vector>

#include <cmath>
#include <limits>

#include "viz/frame_map.h"

using namespace viz;

namespace {

FrameView mk(const char* stage, const char* label) {
    FrameView f;
    f.stage = stage;
    f.label = label;
    return f;
}

}  // namespace

void testFrameMap() {
    MetricsTable m;
    m.columns = {"stage", "iter", "hpwl", "overflow"};
    m.values.resize(4);
    for (int r = 0; r < 40; ++r) {
        const bool cgp = r >= 30;
        m.stage.push_back(cgp ? "cGP" : "mGP");
        m.values[0].push_back(0.f);
        m.values[1].push_back(static_cast<float>(cgp ? r - 30 : r));
        m.values[2].push_back(1000.f - r);
        m.values[3].push_back(0.5f);
    }
    m.rowCount = 40;

    std::vector<FrameView> frames = {
        mk("mGP", "mGP iter 0"),
        mk("mGP", "mGP iter 10"),
        mk("mGP", "mGP iter 20"),
        mk("cGP", "cGP iter 0"),
        mk("cGP", "cGP iter 9"),
    };

    FrameMap fm;
    fm.build(m, frames);
    CHECK_TRUE(fm.valid());

    // 帧1 (mGP iter10) -> 第 10 行；反向回到帧1
    CHECK_EQ(fm.frameToRow(1), 10);
    CHECK_EQ(fm.rowToFrame(10), 1);
    // 帧0 -> 第 0 行
    CHECK_EQ(fm.frameToRow(0), 0);
    // cGP iter0 帧 -> 第 30 行（cGP 起点）
    CHECK_EQ(fm.frameToRow(3), 30);
    CHECK_EQ(fm.rowToFrame(30), 3);
    // cGP iter9 帧 -> 第 39 行
    CHECK_EQ(fm.frameToRow(4), 39);
    CHECK_EQ(fm.rowToFrame(39), 4);

    // 越界返回 -1
    CHECK_EQ(fm.frameToRow(99), -1);
    CHECK_EQ(fm.rowToFrame(99), -1);

    // 质量分级：精确命中为 Exact
    MapQuality q = MapQuality::Unavailable;
    CHECK_EQ(fm.frameToRow(1, &q), 10);
    CHECK_TRUE(q == MapQuality::Exact);
    CHECK_EQ(fm.rowToFrame(10, &q), 1);
    CHECK_TRUE(q == MapQuality::Exact);
}

void testFrameMapHonesty() {
    MetricsTable m;
    m.columns = {"stage", "iter", "hpwl"};
    m.values.resize(3);
    for (int r = 0; r < 30; ++r) {
        m.stage.push_back("mGP");
        m.values[0].push_back(0.f);
        m.values[1].push_back(static_cast<float>(r));
        m.values[2].push_back(100.f - r);
    }
    m.rowCount = 30;

    std::vector<FrameView> frames = {
        mk("mGP", "gp (newblue1.gp.pl)"),   // 文件名里的 1 不得被当 iter
        mk("mGP", "mGP iter 7"),
        mk("FILLERONLY", "FILLERONLY pass"), // metrics 里无此阶段
    };
    FrameMap fm;
    fm.build(m, frames);

    MapQuality q = MapQuality::Exact;
    // FM-A：无 iter 词元，文件名数字不取；同阶段比例 Approx，不冒充精确
    CHECK_TRUE(fm.frameToRow(0, &q) >= 0);
    CHECK_TRUE(q == MapQuality::Approx);
    // iter7 精确
    CHECK_EQ(fm.frameToRow(1, &q), 7);
    CHECK_TRUE(q == MapQuality::Exact);
    // FM-B：未知阶段 FILLERONLY → Unavailable，禁止全局比例跨阶段跳转
    CHECK_EQ(fm.frameToRow(2, &q), -1);
    CHECK_TRUE(q == MapQuality::Unavailable);
    // FM-C：metrics 里 FILLERONLY 行不存在；反方向 mGP 行可映射，未知阶段行 -1
    CHECK_EQ(fm.rowToFrame(35, &q), -1);
    CHECK_TRUE(q == MapQuality::Unavailable);

    // 无 iter 帧的同阶段比例：帧0 落在 mGP 区间内（0..29），不越界
    const int r0 = fm.frameToRow(0);
    CHECK_TRUE(r0 >= 0 && r0 < 30);
}

void testFrameMapLongIter() {
    MetricsTable m;
    m.columns = {"stage", "iter", "hpwl"};
    m.values.resize(3);
    m.stage.push_back("mGP");
    m.values[0].push_back(0.f);
    m.values[1].push_back(0.f);
    m.values[2].push_back(1.f);
    m.rowCount = 1;
    std::vector<FrameView> frames = {
        mk("mGP", "mGP iter 99999999999999999999"),  // 长数字饱和，不溢出 UB
    };
    FrameMap fm;
    fm.build(m, frames);
    MapQuality q = MapQuality::Unavailable;
    // 饱和后最近邻到唯一行，不崩溃、不 UB
    CHECK_EQ(fm.frameToRow(0, &q), 0);
}

// R2-2：空阶段（未知）行/帧不参与映射，不被当作 mGP，也不继承。
void testFrameMapBlankStage() {
    MetricsTable m;
    m.columns = {"stage", "iter", "hpwl"};
    m.values.resize(3);
    // 行0 mGP iter0；行1 空阶段（未知）；行2 mGP iter2
    const char* st[3] = {"mGP", "", "mGP"};
    for (int r = 0; r < 3; ++r) {
        m.stage.push_back(st[r]);
        m.values[0].push_back(0.f);
        m.values[1].push_back(static_cast<float>(r));
        m.values[2].push_back(1.f);
    }
    m.rowCount = 3;
    std::vector<FrameView> frames = {mk("mGP", "mGP iter 0")};
    FrameMap fm;
    fm.build(m, frames);
    MapQuality q = MapQuality::Exact;
    CHECK_EQ(fm.rowToFrame(1, &q), -1);          // 空阶段行不映射
    CHECK_TRUE(q == MapQuality::Unavailable);
    CHECK_EQ(fm.frameToRow(0, &q), 0);           // mGP 帧仍精确到行0
    CHECK_TRUE(q == MapQuality::Exact);
    // 空阶段帧（bundle 未给 stage）→ Unavailable
    std::vector<FrameView> blank = {mk("", "frame x")};
    FrameMap fm2;
    fm2.build(m, blank);
    CHECK_EQ(fm2.frameToRow(0, &q), -1);
    CHECK_TRUE(q == MapQuality::Unavailable);
}

// rowIter 三态：合法非负整数 / 明确非法（超 INT_MAX、负、小数、NaN、Inf）/ 缺 metadata。
void testFrameMapRowIterBoundaries() {
    MetricsTable m;
    m.columns = {"stage", "iter", "hpwl"};
    m.values.resize(3);
    const float cases[9] = {
        0.f,            // 0 合法
        1000000.f,      // 可精确表示的大整数合法
        1e30f,          // 有限但远超 INT_MAX -> 非法
        static_cast<float>(std::numeric_limits<int>::max()),  // float(INT_MAX) 舍入为 2147483648 -> 非法
        -5.f,           // 负 -> 非法
        3.5f,           // 小数 -> 非法
        std::numeric_limits<float>::quiet_NaN(),   // NaN -> 非法
        std::numeric_limits<float>::infinity(),    // +Inf -> 非法
        -std::numeric_limits<float>::infinity(),   // -Inf -> 非法
    };
    for (int r = 0; r < 9; ++r) {
        m.stage.push_back("mGP");
        m.values[0].push_back(0.f);
        m.values[1].push_back(cases[r]);
        m.values[2].push_back(1.f);
    }
    m.rowCount = 9;
    FrameMap fm;
    fm.build(m, {});

    CHECK_EQ(fm.rowIter(0), 0);
    CHECK_TRUE(!fm.rowIterIsIllegal(0));
    CHECK_EQ(fm.rowIter(1), 1000000);
    CHECK_TRUE(!fm.rowIterIsIllegal(1));
    for (int r = 2; r < 9; ++r) {
        CHECK_EQ(fm.rowIter(r), -1);
        CHECK_TRUE(fm.rowIterIsIllegal(r));
    }
    // 越界行：缺 metadata 语义，非法标志为 false
    CHECK_EQ(fm.rowIter(99), -1);
    CHECK_TRUE(!fm.rowIterIsIllegal(99));

    // 缺 metadata：无 iter 列 -> rowIter -1 且非非法（可走顺序比例兜底）
    MetricsTable noCol;
    noCol.columns = {"stage", "hpwl"};
    noCol.values.resize(2);
    for (int r = 0; r < 3; ++r) {
        noCol.stage.push_back("mGP");
        noCol.values[0].push_back(0.f);
        noCol.values[1].push_back(1.f);
    }
    noCol.rowCount = 3;
    FrameMap fmNo;
    fmNo.build(noCol, {});
    CHECK_EQ(fmNo.rowIter(0), -1);
    CHECK_TRUE(!fmNo.rowIterIsIllegal(0));
}

// 明确非法 iter 不得被当“缺 metadata”按比例兜底；缺 metadata 才允许 Approx。
void testFrameMapIllegalNoFallback() {
    // 列存在，但本阶段所有行 iter 非法（1e30）；帧带合法 iter 5 -> Unavailable，不兜底。
    MetricsTable m;
    m.columns = {"stage", "iter", "hpwl"};
    m.values.resize(3);
    for (int r = 0; r < 3; ++r) {
        m.stage.push_back("mGP");
        m.values[0].push_back(0.f);
        m.values[1].push_back(1e30f);
        m.values[2].push_back(1.f);
    }
    m.rowCount = 3;
    std::vector<FrameView> frames = {mk("mGP", "mGP iter 5")};
    FrameMap fm;
    fm.build(m, frames);
    MapQuality q = MapQuality::Exact;
    CHECK_EQ(fm.frameToRow(0, &q), -1);
    CHECK_TRUE(q == MapQuality::Unavailable);
    // 非法行反向也 Unavailable，不 Approx 猜帧
    CHECK_EQ(fm.rowToFrame(0, &q), -1);
    CHECK_TRUE(q == MapQuality::Unavailable);

    // 缺 iter 列（缺 metadata）：帧无 iter 词元 -> 允许同阶段顺序 Approx 兜底。
    MetricsTable noCol;
    noCol.columns = {"stage", "hpwl"};
    noCol.values.resize(2);
    for (int r = 0; r < 3; ++r) {
        noCol.stage.push_back("mGP");
        noCol.values[0].push_back(0.f);
        noCol.values[1].push_back(1.f);
    }
    noCol.rowCount = 3;
    std::vector<FrameView> nf = {mk("mGP", "mGP pass")};
    FrameMap fmNo;
    fmNo.build(noCol, nf);
    CHECK_TRUE(fmNo.frameToRow(0, &q) >= 0);
    CHECK_TRUE(q == MapQuality::Approx);
    CHECK_TRUE(fmNo.rowToFrame(0, &q) >= 0);
    CHECK_TRUE(q == MapQuality::Approx);
}

// FM-G5：无 iter 词元的帧走 Approx 时，flat 必须剔除明确非法行；全非法 -> Unavailable，
// 混合时只在合法/缺 metadata 行里选，不得返回非法行。
void testFrameMapApproxSkipsIllegal() {
    // 全非法（2147483648 / 2147483649 均超 INT_MAX），帧 label='mGP' 无 iter 词元。
    MetricsTable allBad;
    allBad.columns = {"stage", "iter", "hpwl"};
    allBad.values.resize(3);
    const float bad[2] = {2147483648.f, 2147483649.f};
    for (int r = 0; r < 2; ++r) {
        allBad.stage.push_back("mGP");
        allBad.values[0].push_back(0.f);
        allBad.values[1].push_back(bad[r]);
        allBad.values[2].push_back(1.f);
    }
    allBad.rowCount = 2;
    FrameMap fmBad;
    fmBad.build(allBad, {mk("mGP", "mGP")});
    MapQuality q = MapQuality::Exact;
    CHECK_EQ(fmBad.frameToRow(0, &q), -1);
    CHECK_TRUE(q == MapQuality::Unavailable);
    CHECK_EQ(fmBad.rowToFrame(0, &q), -1);
    CHECK_TRUE(q == MapQuality::Unavailable);

    // 混合：r0 非法、r1 合法(5)、r2 非法；无 label 帧 Approx 只能落到合法 r1。
    MetricsTable mix;
    mix.columns = {"stage", "iter", "hpwl"};
    mix.values.resize(3);
    const float vals[3] = {2147483648.f, 5.f, 2147483649.f};
    for (int r = 0; r < 3; ++r) {
        mix.stage.push_back("mGP");
        mix.values[0].push_back(0.f);
        mix.values[1].push_back(vals[r]);
        mix.values[2].push_back(1.f);
    }
    mix.rowCount = 3;
    FrameMap fmMix;
    fmMix.build(mix, {mk("mGP", "mGP")});
    CHECK_EQ(fmMix.frameToRow(0, &q), 1);
    CHECK_TRUE(q == MapQuality::Approx);
    // 反向：合法行 r1 -> 该帧；非法 r0/r2 直接 Unavailable。
    CHECK_EQ(fmMix.rowToFrame(1, &q), 0);
    CHECK_EQ(fmMix.rowToFrame(0, &q), -1);
    CHECK_TRUE(q == MapQuality::Unavailable);
    CHECK_EQ(fmMix.rowToFrame(2, &q), -1);
    CHECK_TRUE(q == MapQuality::Unavailable);
}

void testFrameMapNonContiguous() {
    // mGP 0..9, cGP 10..19, mGP 20..29（同名非连续，中间夹 cGP）。
    MetricsTable m;
    m.columns = {"stage", "iter", "hpwl"};
    m.values.resize(3);
    for (int r = 0; r < 30; ++r) {
        const char* st = (r < 10) ? "mGP" : (r < 20 ? "cGP" : "mGP");
        m.stage.push_back(st);
        m.values[0].push_back(0.f);
        m.values[1].push_back(static_cast<float>(r));
        m.values[2].push_back(1.f);
    }
    m.rowCount = 30;
    std::vector<FrameView> frames = {
        mk("mGP", "mGP iter 5"),    // 落在首段
        mk("mGP", "mGP iter 25"),   // 落在次段，不得被首段/ cGP 截胡
        mk("cGP", "cGP iter 15"),
    };
    FrameMap fm;
    fm.build(m, frames);
    MapQuality q = MapQuality::Exact;
    CHECK_EQ(fm.frameToRow(0, &q), 5);   CHECK_TRUE(q == MapQuality::Exact);
    CHECK_EQ(fm.frameToRow(1, &q), 25); CHECK_TRUE(q == MapQuality::Exact);
    CHECK_EQ(fm.frameToRow(2, &q), 15); CHECK_TRUE(q == MapQuality::Exact);
    // 反向：次段 mGP 行25 -> 帧1，不跳 cGP
    CHECK_EQ(fm.rowToFrame(25, &q), 1);
    CHECK_EQ(fm.rowToFrame(15, &q), 2);
}
