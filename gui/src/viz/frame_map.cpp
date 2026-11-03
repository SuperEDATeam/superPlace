#include "viz/frame_map.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace viz {

const char* mapQualityLabel(MapQuality q) {
    switch (q) {
        case MapQuality::Exact:      return "精确";
        case MapQuality::Nearest:   return "最近邻";
        case MapQuality::Approx:    return "近似(比例)";
        case MapQuality::Unavailable:
        default:                     return "无映射";
    }
}

namespace {

/// 只接受 "iter N" 词元后的整数（N 可带正负）；其余数字（文件名、benchmark 编号）
/// 一律不取。长数字饱和到 [0, INT_MAX]，避免连乘溢出 UB。解析不到返回 -1。
int parseIter(const std::string& s) {
    const size_t n = s.size();
    for (size_t i = 0; i + 4 <= n; ++i) {
        // 词元边界：前一个字符非字母数字，避免 "filter" 之类误命中。
        const bool boundary = (i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1])));
        if (!boundary) continue;
        if (std::tolower(static_cast<unsigned char>(s[i])) != 'i' ||
            std::tolower(static_cast<unsigned char>(s[i + 1])) != 't' ||
            std::tolower(static_cast<unsigned char>(s[i + 2])) != 'e' ||
            std::tolower(static_cast<unsigned char>(s[i + 3])) != 'r')
            continue;
        size_t j = i + 4;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
        errno = 0;
        char* end = nullptr;
        long v = std::strtol(s.c_str() + j, &end, 10);
        if (end == s.c_str() + j) return -1;          // iter 后没有数字
        if (v < 0) return -1;
        if (v > std::numeric_limits<int>::max())
            return std::numeric_limits<int>::max();   // 饱和，不溢出
        return static_cast<int>(v);
    }
    return -1;
}

}  // namespace

void FrameMap::build(const MetricsTable& m, const std::vector<FrameView>& frames) {
    frames_.clear();
    stages_.clear();
    rowCount_ = m.rowCount;
    iterCol_ = m.columnIndex("iter");
    m_ = &m;

    // 切分阶段行区间：stage 列变化处即边界（非连续同名也各自成段，不合并）。
    for (int r = 0; r < rowCount_; ++r) {
        const std::string& st = m.stage[static_cast<size_t>(r)];
        if (stages_.empty() || stages_.back().name != st)
            stages_.push_back(StageRange{st, r, r + 1});
        else
            stages_.back().end = r + 1;
    }

    frames_.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        FrameInfo fi;
        fi.index = static_cast<int>(i);
        fi.stage = frames[i].stage;
        fi.iter = parseIter(frames[i].label);
        frames_.push_back(fi);
    }
}

std::vector<size_t> FrameMap::rangesOf(const std::string& name) const {
    std::vector<size_t> out;
    for (size_t i = 0; i < stages_.size(); ++i)
        if (stages_[i].name == name) out.push_back(i);
    return out;
}

FrameMap::IterVal FrameMap::rowIterVal(int row) const {
    if (!m_ || row < 0 || row >= rowCount_) return {-1, false, false};
    if (iterCol_ < 0) return {-1, false, false};  // 无 iter 列 = 缺 metadata
    const auto& col = (*m_).values[static_cast<size_t>(iterCol_)];
    if (static_cast<int>(col.size()) <= row) return {-1, false, false};  // 缺行

    const double v = static_cast<double>(col[static_cast<size_t>(row)]);
    // 明确非法：非有限、负数、非整数、或超过 INT_MAX（用 double 比较，不用 float(INT_MAX)
    // 其上界本身被舍入为 2147483648）。非法 ≠ 缺 metadata，禁止按比例兜底。
    if (!std::isfinite(v) || v < 0.0) return {-1, true, false};
    if (v != std::trunc(v)) return {-1, true, false};
    if (v > static_cast<double>(std::numeric_limits<int>::max()))
        return {-1, true, false};
    return {static_cast<int>(v), true, true};
}

int FrameMap::rowIter(int row) const {
    const IterVal iv = rowIterVal(row);
    return iv.valid ? iv.v : -1;
}

bool FrameMap::rowIterIsIllegal(int row) const {
    const IterVal iv = rowIterVal(row);
    return iv.present && !iv.valid;
}

int FrameMap::framesOfStage(const std::string& stage, std::vector<int>& out) const {
    out.clear();
    for (size_t f = 0; f < frames_.size(); ++f)
        if (frames_[f].stage == stage) out.push_back(static_cast<int>(f));
    return static_cast<int>(out.size());
}

int FrameMap::frameToRow(int frame, MapQuality* q) const {
    const auto fail = [&]() {
        if (q) *q = MapQuality::Unavailable;
        return -1;
    };
    if (!valid() || frame < 0 || frame >= static_cast<int>(frames_.size())) return fail();
    const FrameInfo& fi = frames_[static_cast<size_t>(frame)];

    // R2-2：空阶段 = 未知，不参与映射（不做内部近似配对）。
    if (fi.stage.empty()) return fail();

    const auto ranges = rangesOf(fi.stage);
    if (ranges.empty()) return fail();   // 未知阶段：禁止跨阶段全局比例猜测

    // 1) 帧自身带合法 iter 且 iter 列存在：仅在“合法行”里精确 / 最近邻；
    //    列存在但无任何合法行（全非法/缺行）→ Unavailable，禁止按比例兜底选非法行。
    if (fi.iter >= 0 && iterCol_ >= 0) {
        int best = -1, bd = -1;
        for (const size_t si : ranges)
            for (int r = stages_[si].start; r < stages_[si].end; ++r) {
                const IterVal iv = rowIterVal(r);
                if (!iv.valid) continue;
                const int d = std::abs(iv.v - fi.iter);
                if (bd < 0 || d < bd) { bd = d; best = r; }
            }
        if (best >= 0) {
            if (q) *q = (bd == 0) ? MapQuality::Exact : MapQuality::Nearest;
            return best;
        }
        return fail();
    }

    // 2) 同名阶段内按顺序比例兜底（Approx）：只在本阶段各段的行并集里取，
    //    不跳到别的阶段；非连续段按原始顺序拼接。明确非法 iter 的行一律剔除，
    //    混合时只在合法/缺 metadata 行里选；全非法则无候选，Unavailable。
    std::vector<int> same;
    const int cnt = framesOfStage(fi.stage, same);
    if (cnt > 0) {
        std::vector<int> flat;
        for (const size_t si : ranges)
            for (int r = stages_[si].start; r < stages_[si].end; ++r)
                if (!rowIterIsIllegal(r)) flat.push_back(r);
        if (!flat.empty()) {
            int ord = 0;
            for (int k = 0; k < cnt; ++k)
                if (same[k] == frame) { ord = k; break; }
            const float pos = cnt > 1
                                  ? static_cast<float>(ord) / static_cast<float>(cnt - 1)
                                  : 0.f;
            const int idx = static_cast<int>(pos * static_cast<float>(flat.size() - 1) + 0.5f);
            if (q) *q = MapQuality::Approx;
            return flat[std::clamp(idx, 0, static_cast<int>(flat.size()) - 1)];
        }
    }
    return fail();
}

int FrameMap::rowToFrame(int row, MapQuality* q) const {
    const auto fail = [&]() {
        if (q) *q = MapQuality::Unavailable;
        return -1;
    };
    if (!valid() || row < 0 || row >= rowCount_) return fail();

    // 定位本行所属段（非连续同名也能找到自己这一段）。
    int si = -1;
    for (size_t i = 0; i < stages_.size(); ++i)
        if (row >= stages_[i].start && row < stages_[i].end) { si = static_cast<int>(i); break; }
    if (si < 0) return fail();
    const StageRange& rg = stages_[static_cast<size_t>(si)];
    // R2-2：空阶段行 = 未知，不映射到任何帧。
    if (rg.name.empty()) return fail();

    const IterVal iv = rowIterVal(row);
    // 列存在但本行 iter 明确非法（超界/负/小数/NaN/Inf）→ Unavailable，
    // 不得当作“缺 metadata”按比例猜测。
    if (iv.present && !iv.valid) return fail();

    // 1) 合法 iter：同名阶段帧精确 / 最近邻；帧侧无可比 iter 时落到下方顺序 Approx。
    if (iv.valid && iterCol_ >= 0) {
        int best = -1, bd = -1;
        for (size_t f = 0; f < frames_.size(); ++f) {
            if (frames_[f].stage != rg.name) continue;
            if (frames_[f].iter < 0) continue;
            const int d = std::abs(frames_[f].iter - iv.v);
            if (bd < 0 || d < bd) { bd = d; best = static_cast<int>(f); }
        }
        if (best >= 0) {
            if (q) *q = (bd == 0) ? MapQuality::Exact : MapQuality::Nearest;
            return best;
        }
    }

    // 2) 仅当缺 metadata（无 iter 列/缺行）才在同名阶段按顺序比例兜底（Approx）；
    //    明确非法行从候选中剔除；无本阶段帧/无合法候选则 Unavailable，不跳到别的阶段。
    std::vector<int> same;
    const int cnt = framesOfStage(rg.name, same);
    if (cnt > 0) {
        const auto ranges = rangesOf(rg.name);
        std::vector<int> flat;
        for (const size_t ri : ranges)
            for (int r = stages_[ri].start; r < stages_[ri].end; ++r)
                if (!rowIterIsIllegal(r)) flat.push_back(r);
        if (!flat.empty()) {
            int local = 0;
            for (int k = 0; k < static_cast<int>(flat.size()); ++k)
                if (flat[k] == row) { local = k; break; }
            const float pos = flat.size() > 1
                                  ? static_cast<float>(local) / static_cast<float>(flat.size() - 1)
                                  : 0.f;
            const int idx = static_cast<int>(pos * static_cast<float>(cnt - 1) + 0.5f);
            if (q) *q = MapQuality::Approx;
            return same[std::clamp(idx, 0, cnt - 1)];
        }
    }
    return fail();
}

}  // namespace viz
