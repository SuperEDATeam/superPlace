// 布局画布：GPU 实例化渲染 + 相机。
//
// 整个查看器好不好用，基本只取决于这一个文件（见 work_report/01 §7.4）。
// 要点有三个，都不是可选优化：
//   1. 绝不一个单元一个对象——全部打成实例，一次 draw call 画完
//   2. 亚像素保护——全芯片视角下标准单元不足 1px，直接画会碎成摩尔纹
//   3. 位置更新走 glBufferSubData，不要重建缓冲
#pragma once

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

class Canvas {
public:
    bool init(std::string* error);
    void shutdown();

    /// 上传整场静态数据（尺寸 + 类别）。换 bundle 时调一次。
    void setGeometry(const NodeGeometry& geom);
    /// 每帧更新位置。x/y 是两段连续数组（与 FrameView 一致）。
    void setPositions(const float* x, const float* y, int count);

    /// 画到当前 framebuffer 的 (0,0,viewW,viewH) 区域。
    void draw(const Camera& cam, int viewW, int viewH);

    bool  layerVisible[static_cast<int>(Kind::Count)] = {false, true, true, true};
    float minPixelSize = 1.5f;   ///< 亚像素保护阈值
    float outlineOnly = 0.f;     ///< 预留：>0 时只画边框（近景检视用）

    int   instanceCount() const { return count_; }
    double lastUploadMs() const { return uploadMs_; }

private:
    unsigned prog_ = 0, vao_ = 0, quad_ = 0, bufCenter_ = 0, bufSize_ = 0, bufKind_ = 0;
    int uView_ = -1, uColors_ = -1, uMinPx_ = -1, uViewport_ = -1;
    int count_ = 0;
    double uploadMs_ = 0.0;
    std::vector<float> interleaved_;   ///< 位置上传前的交错缓冲，避免每帧分配
};

}  // namespace viz
