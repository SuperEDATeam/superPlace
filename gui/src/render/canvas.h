// 布局画布：GPU 实例化渲染 + 相机。
//
// 整个查看器好不好用，基本只取决于这一个文件（见 work_report/01 §7.4）。
// 要点有三个，都不是可选优化：
//   1. 绝不一个单元一个对象——全部打成实例，一次 draw call 画完
//   2. 亚像素保护——全芯片视角下标准单元不足 1px，直接画会碎成摩尔纹
//   3. 位置更新走 glBufferSubData，不要重建缓冲
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "render/gl_loader.h"
#include "viz/scene.h"

namespace viz {

/// 设计坐标 <-> 屏幕坐标。Y 轴向上为正，翻转只发生在投影矩阵里。
struct Camera {
    float cx = 0.f, cy = 0.f;   ///< 视野中心（设计坐标）
    float scale = 1.f;          ///< 每个设计单位对应多少像素

    void fit(const Rect& r, float viewW, float viewH, float margin = 0.04f) {
        cx = r.cx();
        cy = r.cy();
        const float sx = viewW / (r.width() * (1.f + margin * 2.f));
        const float sy = viewH / (r.height() * (1.f + margin * 2.f));
        scale = (sx < sy) ? sx : sy;
    }

    void screenToWorld(float px, float py, float viewW, float viewH, float* wx, float* wy) const {
        *wx = cx + (px - viewW * 0.5f) / scale;
        *wy = cy - (py - viewH * 0.5f) / scale;   // 屏幕 y 向下
    }

    /// screenToWorld 的逆变换（T4 高亮框、T8 位移箭头都要用屏幕坐标）。
    void worldToScreen(float wx, float wy, float viewW, float viewH, float* px, float* py) const {
        *px = viewW * 0.5f + (wx - cx) * scale;
        *py = viewH * 0.5f - (wy - cy) * scale;
    }

    /// T2：面板比例或窗口大小变化后保持视野。
    ///
    /// ⚠️ 不能重新 fit()——那会丢掉用户已经缩放到的局部。这里中心 cx/cy 不动，
    /// scale 按【短边】比例缩放：新旧视野重叠区保持不变、不变形，长边方向
    /// 多出/少掉的边缘自然露出/裁掉。
    void rescaleOnResize(float oldW, float oldH, float newW, float newH) {
        if (oldW <= 0.f || oldH <= 0.f || newW <= 0.f || newH <= 0.f || scale <= 0.f) return;
        const float r = std::min(newW / oldW, newH / oldH);
        scale *= r;
    }

    /// 以某个屏幕点为锚缩放：缩放前后该点对应的设计坐标保持不变。
    /// 不这么做的话滚轮缩放会往视野中心漂，手感会立刻垮掉。
    void zoomAt(float px, float py, float viewW, float viewH, float factor) {
        float wx, wy;
        screenToWorld(px, py, viewW, viewH, &wx, &wy);
        scale *= factor;
        float nx, ny;
        screenToWorld(px, py, viewW, viewH, &nx, &ny);
        cx += wx - nx;
        cy += wy - ny;
    }
};

/// 着色/着色模式（T7）。Kind 是默认按类别；其余两种走连续色表。
enum class ColorMode : int { Kind = 0, Density = 1, Displacement = 2 };

class Canvas {
public:
    bool init(std::string* error);
    void shutdown();

    /// 上传整场静态数据（尺寸 + 类别）。换 bundle 时调一次。
    void setGeometry(const NodeGeometry& geom);
    /// 每帧更新位置。x/y 是两段连续数组（与 FrameView 一致）。
    void setPositions(const float* x, const float* y, int count);

    /// 连续属性值（T7 局部密度 / T8 位移量）。count 须与节点数一致；
    /// 仅在连续着色模式下上传；类别(Kind)模式下调用直接跳过，避免无效 n 浮点上传。
    void setContinuousValues(const float* values, int count);
    void setColorMode(ColorMode mode) {
        colorMode_ = mode;
        valueActive_ = (mode != ColorMode::Kind);
    }
    void setValueRange(float mn, float mx) { valMin_ = mn; valMax_ = mx; }

    /// T6 热力图：上传归一化密度场（norm/maxNorm 已由调用方算好）。
    /// cols/rows 为 bin 网格；maxNorm 用于色表归一化。
    void setHeatmap(const float* norm, int cols, int rows, float maxNorm);

    /// 画实例到当前 framebuffer 的 (0,0,viewW,viewH) 区域。
    void draw(const Camera& cam, int viewW, int viewH);
    /// T6：在实例之下画热力图叠加（alpha 0..1）。无热力图时 no-op。
    void drawHeatmapOverlay(const Camera& cam, int viewW, int viewH, const Rect& chip, float alpha);

    bool  layerVisible[static_cast<int>(Kind::Count)] = {false, true, true, true};
    float minPixelSize = 1.5f;   ///< 亚像素保护阈值
    float outlineOnly = 0.f;     ///< 预留：>0 时只画边框（近景检视用）

    int   instanceCount() const { return count_; }
    double lastUploadMs() const { return uploadMs_; }

private:
    // 实例程序
    unsigned prog_ = 0, vao_ = 0, quad_ = 0;
    unsigned bufCenter_ = 0, bufSize_ = 0, bufKind_ = 0, bufValue_ = 0;
    int uView_ = -1, uColors_ = -1, uMinPx_ = -1, uViewport_ = -1;
    int uColorMode_ = -1, uValueMin_ = -1, uValueMax_ = -1;
    // 热力图程序
    unsigned hmProg_ = 0, hmVao_ = 0, hmTex_ = 0;
    int hmUView_ = -1, hmUViewport_ = -1, hmUTex_ = -1, hmUChip_ = -1, hmUAlpha_ = -1;
    int hmCols_ = 0, hmRows_ = 0;

    int count_ = 0;
    double uploadMs_ = 0.0;
    std::vector<float> interleaved_;   ///< 位置上传前的交错缓冲，避免每帧分配
    std::vector<float> values_;        ///< 连续属性值缓存

    ColorMode colorMode_ = ColorMode::Kind;
    bool valueActive_ = false;   ///< 连续着色模式才上传 bufValue_
    float valMin_ = 0.f, valMax_ = 1.f;
};

}  // namespace viz
