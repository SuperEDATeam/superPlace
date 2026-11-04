// viz bundle 写出（superplace-viz/1，格式见 work_report/06 §3）。
//
// 运行过程中把布局逐帧录下来，供前端拖动进度条回放从初始到收敛的全过程。
// 只读 PlaceDB，不改任何坐标——开不开录制，优化轨迹逐位相同。
//
// 节点集合与顺序：只录【真实节点】[0, numNodes)，不含 filler；顺序是 PlaceDB 的
// 内存顺序。注意它【不等于】.nodes 文件里的顺序——读入时 partitionNodes() 会把节点
// 重排成 [可移动 | 固定]，且不保留旧序映射。因此几何、标志、名字、每一帧都必须
// 由这里用同一套顺序一起写出，不能和 scripts/make_viz.py（按文件顺序）产的 bundle 混用。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/metrics.h"

namespace sp {

class PlaceDB;

class VizBundleWriter {
public:
    /// 建目录并写出整场不变的部分：geom.bin / flags.bin / names.txt，以及网表 nets/。
    /// 必须在全局布局之前调用——mLG 会把宏置为 F_FIXED，之后再取标志位，
    /// 宏在前端就会被当成固定终端着色。
    void open(const std::string& dir, const std::string& benchmark, const PlaceDB& db,
              int frameStride);

    /// 录一帧。与上一帧位置逐位相同、且引脚偏移也没变时跳过（FILLERONLY 阶段只有
    /// filler 在动，真实节点纹丝不动，这些帧录了也只是重复画面）。
    /// 引脚偏移变了（mLG 翻转宏）时写出稀疏更新，并把这一帧强制标为 breakBefore。
    void append(const PlaceDB& db, const FrameMark& m);

    /// 把 metrics.csv / summary.json 复制进 bundle，前端画曲线与总览要用。
    void finish(const std::string& resultDir);

    bool isOpen() const { return !dir_.empty(); }
    int  frameCount() const { return static_cast<int>(frames_.size()); }
    int  skippedDuplicates() const { return skipped_; }
    /// 最近一帧的全局进度；还没录过帧时为 0
    double lastT() const { return frames_.empty() ? 0.0 : frames_.back().mark.t; }

private:
    struct Entry {
        std::string file;
        FrameMark   mark;
    };

    /// 每录一帧就重写一次 scene.json（先写临时文件再 rename）。
    /// 好处是运行中途崩溃或被打断时，已录的部分仍是一个合法 bundle。
    void writeScene() const;

    /// nets/ 的静态部分（work_report/06 §3.9）。数据不完整时不写，前端按"无网表"处理。
    void writeNets(const PlaceDB& db);
    /// 与上次写出的引脚偏移比较；有变化就写出 nets/pin_offset_fNNNN.bin 并返回 true
    bool writeOffsetUpdate(const PlaceDB& db, int frameIndex);

    struct OffsetUpdate {
        int         frame = 0;
        std::string file;
        int         count = 0;
    };

    std::string        dir_;
    int                n_ = 0;
    int                stride_ = 0;
    std::vector<float> last_;   ///< 上一帧的 x[N] y[N]，用于去重
    std::vector<Entry> frames_;
    int                skipped_ = 0;
    double             spentMs_ = 0.0;   ///< 录制本身花掉的时间，用于确认开销可接受

    // scene.json 的静态部分，open 时取一次
    std::string benchmark_;
    float coreLx_ = 0, coreLy_ = 0, coreHx_ = 0, coreHy_ = 0;
    float chipLx_ = 0, chipLy_ = 0, chipHx_ = 0, chipHy_ = 0;
    float rowHeight_ = 0;
    int   rows_ = 0;
    bool  hasMetrics_ = false, hasSummary_ = false;

    bool  hasNets_ = false, hasNetNames_ = false, hasNetWeights_ = false;
    int   numNets_ = 0, numPins_ = 0;
    std::vector<float>        pinOffX_, pinOffY_;   ///< 上次写出的引脚偏移（引擎引脚顺序）
    std::vector<OffsetUpdate> offsetUpdates_;
};

}  // namespace sp
