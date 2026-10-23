// T5：metrics.csv 行 ↔ bundle 抽样帧的诚实映射。
//
// 两个事实决定了这里不能伪造一一对应：
//   1. bundle 帧数远少于 metrics 行数（帧是抽样的）；
//   2. metrics 的 iter 列在每个阶段都从 0 重数，直接当横轴会折回去。
//
// 映射分四级质量，UI 必须如实标注，绝不把猜测画成精确游标：
//   Exact      同阶段且 iter 精确相等；
//   Nearest    同阶段、iter 最近邻（抽样帧与迭代号错开）；
//   Approx     同阶段内无可用 iter，按顺序比例兜底；
//   Unavailable 未知阶段 / 无本阶段帧 / 解析不到 → 返回 -1，不跨阶段猜测。
#pragma once

#include <string>
#include <vector>

#include "viz/scene.h"

namespace viz {

/// 映射质量等级（UI 据此标注，禁止把 Nearest/Approx/Unavailable 当精确）。
enum class MapQuality { Unavailable = 0, Exact = 1, Nearest = 2, Approx = 3 };

const char* mapQualityLabel(MapQuality q);

class FrameMap {
public:
    struct FrameInfo {
        int index = -1;
        std::string stage;
        int iter = -1;   // 仅当 label 含 "iter N" 词元时解析；否则 -1
    };

    void build(const MetricsTable& m, const std::vector<FrameView>& frames);

    /// 帧 -> metrics 行。无可靠映射返回 -1；质量经 q 带出（可为 nullptr）。
    int frameToRow(int frame, MapQuality* q = nullptr) const;
    /// metrics 行 -> 帧（点曲线跳转）。无可靠映射返回 -1；质量经 q 带出。
    int rowToFrame(int row, MapQuality* q = nullptr) const;

    const std::vector<FrameInfo>& frames() const { return frames_; }
    bool valid() const { return rowCount_ > 0; }

    /// iter 列存在且该行值为合法非负整数时返回该整数；否则 -1。
    /// 与“缺 metadata”区分：缺列/缺行返回 -1 且 rowIterIsIllegal 为 false。
    int rowIter(int row) const;
    /// 列存在但该行 iter 明确非法（超 INT_MAX / 负数 / 小数 / NaN / Inf）。
    bool rowIterIsIllegal(int row) const;

private:
    struct IterVal { int v = -1; bool present = false; bool valid = false; };
    /// 三态：present=false=缺 metadata（无列/缺行）；present=true/valid=false=明确非法；
    /// present=valid=true=合法非负整数。
    IterVal rowIterVal(int row) const;

    struct StageRange {
        std::string name;
        int start = 0, end = 0;   // [start, end)
    };

    std::vector<FrameInfo> frames_;
    std::vector<StageRange> stages_;
    int rowCount_ = 0;
    int iterCol_ = -1;
    const MetricsTable* m_ = nullptr;

    std::vector<size_t> rangesOf(const std::string& name) const;  // 同名非连续多段
    /// 收集某阶段的帧在 frames_ 里的下标；返回数量。
    int framesOfStage(const std::string& stage, std::vector<int>& out) const;
};

}  // namespace viz
