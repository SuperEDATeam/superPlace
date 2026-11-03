// 查看器主程序。
//
// App 不知道数据从哪来——它只持有一个 IPlacementSource。
// 独立版注入 BundleSource，集成版注入 LiveSource，UI 代码完全相同。
#pragma once

#include <memory>
#include <vector>
#include <string>

#include "render/canvas.h"
#include "viz/density.h"
#include "viz/frame_map.h"
#include "viz/spatial_index.h"
#include "viz/source.h"

struct GLFWwindow;

namespace viz {

struct AppOptions {
    std::string initialBundle;     ///< 启动时加载哪个 bundle（可空）
    int  width = 1600, height = 980;
    bool vsync = true;
    /// >0 时进入基准模式：渲染这么多帧、报告耗时、退出，不进交互循环。
    /// 既用于量化回归，也让"33 万单元够不够快"这种问题有实测答案而不是猜。
    int  benchFrames = 0;
    std::string screenshot;        ///< 非空则把最后一帧存成 PPM
    /// >0 时：正常跑交互循环，到点自动截图并退出。
    /// 实时模式下用来出图与做自动化验证——bench 模式会立刻退出，等不到引擎产出帧。
    double autoShotAfterSec = 0.0;
    int  startFrame = 0;           ///< 启动时跳到第几帧，-1 表示最后一帧
    /// 自检用：启动即开启两帧对比（A=0, B=最后一帧），便于无交互截图验证 T8。
    bool compareOn = false;
};

class App {
public:
    explicit App(AppOptions opt);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// 注入数据源。集成版在这里塞 LiveSource。
    void setSource(std::unique_ptr<IPlacementSource> src);

    /// 跑主循环直到窗口关闭。返回进程退出码。
    int run();

private:
    bool initWindow();
    void drawUi();
    void drawCanvasPanel(unsigned dockId);
    void drawSidebar();
    void drawMetricsPanel();
    void drawInspectPanel();
    void drawComparePanel();
    void gotoFrame(int index);
    void uploadGeometry();
    int  runBench();
    void saveScreenshot(const std::string& path, int w, int h);

    // ---- T4：懒重建空间索引（切帧后第一次点选时才建）
    void rebuildIndexIfDirty();
    // ---- T6：懒重建密度场
    void rebuildDensityIfDirty();
    // ---- T7/T8：按当前着色模式算连续属性值并上传
    void updateContinuousValues();

    AppOptions opt_;
    GLFWwindow* window_ = nullptr;
    Canvas canvas_;
    Camera cam_;
    std::unique_ptr<IPlacementSource> src_;

    int    frameIndex_ = 0;
    bool   playing_ = false;
    float  playFps_ = 12.f;
    double lastAdvance_ = 0.0;
    bool   needFit_ = true;
    bool   glReady_ = false;      ///< GL 上下文是否已就绪
    bool   geomUploaded_ = false;
    std::string status_, bundlePath_;
    char pathBuf_[512] = {0};
    FrameView cur_;

    // 画布面板在窗口里的位置与尺寸（像素），交互与投影都要用
    float canvasX_ = 0, canvasY_ = 0, canvasW_ = 1, canvasH_ = 1;
    double drawMs_ = 0.0;
    std::vector<float> xs_;      ///< 曲线横轴（全局行序号），按需重建
    bool   layoutBuilt_ = false;

    // ---- T2：resize 保持视野（记住上一帧画布尺寸）
    float prevCanvasW_ = -1.f, prevCanvasH_ = -1.f;

    // ---- T4：空间索引 + 选中项
    SpatialIndex pickIndex_;
    bool indexDirty_ = true;
    int  selected_ = -1;
    float downX_ = 0.f, downY_ = 0.f;   ///< 按下位置，区分单击与拖拽

    // ---- T5：metrics 行 ↔ 抽样帧映射
    FrameMap frameMap_;
    bool mapBuilt_ = false;

    // ---- T6：密度场 / 热力图 / LOD
    DensityField density_;
    bool densityDirty_ = true;
    unsigned long lastHmGen_ = 0;  ///< 已上传纹理对应的密度场版本，避免每帧重复上传
    bool heatmapOverlay_ = true;   ///< 近景是否叠加半透明热力图
    bool lodAuto_ = true;          ///< 远景自动切热力图
    bool lodActive_ = false;       ///< 本帧是否处于 LOD（由阈值算出）

    // ---- T7：着色模式（net 数因 T10 无 nets.bin 而禁用）
    ColorMode colorMode_ = ColorMode::Kind;
    float lastValMin_ = 0.f, lastValMax_ = 1.f;   ///< 连续色表当前范围（图例用）

    // ---- T8：两帧对比与位移向量
    bool compareOn_ = false;
    int  frameA_ = 0, frameB_ = 1;
    bool showVectors_ = true;
    std::vector<float> dispValues_;
    std::vector<float> densityScratch_;   ///< T6 着色复用缓冲，避免每帧堆分配
};

}  // namespace viz
