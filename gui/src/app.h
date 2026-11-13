// 查看器主程序。
//
// App 不知道数据从哪来——它只持有一个 IPlacementSource。
// App —— 前端总装。注入 BundleSource，UI 代码与数据源解耦。
#pragma once

#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <sys/types.h>

#include "render/canvas.h"
#include "viz/density.h"
#include "viz/density_cache.h"
#include "viz/frame_map.h"
#include "viz/playback.h"
#include "viz/spatial_index.h"
#include "viz/source.h"

struct GLFWwindow;

namespace viz {

struct AppOptions {
    std::string initialBundle;     ///< 启动时加载哪个 bundle（可空）
    int  width = 1600, height = 980;
    double uiScale = 1.5;          ///< T36 界面缩放系数 S（--ui-scale 可覆盖）
    bool vsync = true;
    /// >0 时进入基准模式：渲染这么多帧、报告耗时、退出，不进交互循环。
    /// 既用于量化回归，也让"33 万单元够不够快"这种问题有实测答案而不是猜。
    int  benchFrames = 0;
    /// >0 时进入真实 UI 端到端基准：跑完整 drawUi（含 ImGui/LOD/热图）这么多帧，
    /// 报告每帧总耗时 p50/p95/fps 与 LOD 状态——区别于 runBench 的纯矩形微基准。
    int  benchUiFrames = 0;
    std::string benchCsvPath;
    double zoomFactor = 1.0;       ///< 性能测试：fit 后再乘
    double centerX = 1e30, centerY = 1e30;  ///< 性能测试：指定相机中心
    bool renderCull = true;        ///< 性能测试：视口剔除开关（对照 fastpath）
    bool paused = false;           ///< 性能测试：静态 UI（不推进时间）
    std::string screenshot;        ///< 非空则把最后一帧存成 PPM
    /// >0 时：正常跑交互循环，到点自动截图并退出。
    /// 实时模式下用来出图与做自动化验证——bench 模式会立刻退出，等不到引擎产出帧。
    double autoShotAfterSec = 0.0;
    int  startFrame = -999;         ///< 启动跳帧；-999 未设置，-1 最后一帧
    /// 自检用：启动即开启两帧对比（A=0, B=最后一帧），便于无交互截图验证 T8。
    bool compareOn = false;
};

class App {
public:
    explicit App(AppOptions opt);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// 注入数据源（BundleSource）。
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
    void drawTerminalPanel();
    void drawComparePanel();

    // T32：按固定选项打开 bundle；dir 为空时清空回空白状态。
    void openBundle(const std::string& dir);
    // T33：现场布局子进程生命周期。
    void startPlace();
    void stopPlace(bool report);
    void pollPlace();
    void handleTerminalCommand(const std::string& cmd);
    // T34：把当前显示帧导出 GDSII 到固定路径。
    void exportGds();
    // 仓库根目录：可执行文件在 <ROOT>/gui/build/ 下，向上两级推出。
    static std::string repoRoot();
    void gotoFrame(int index);
    void gotoKeyframe(int index);
    void syncPlayback();
    void uploadGeometry();
    int  runBench();
    void saveScreenshot(const std::string& path, int w, int h);

    // ---- T4：懒重建空间索引（切帧后第一次点选时才建）
    void rebuildIndexIfDirty();
    void rebuildIndexAtCurrent();   ///< T24：点击时用当前插值位置建索引
    // ---- T6：懒重建密度场
    void rebuildDensityIfDirty();
    void startPreDensity();          ///< setSource 后起后台线程预计算各关键帧密度
    bool statsSource(const float** sx, const float** sy);
    // ---- T7/T8：按当前着色模式算连续属性值并上传
    void updateContinuousValues();

    AppOptions opt_;
    GLFWwindow* window_ = nullptr;
    Canvas canvas_;
    Camera cam_;
    std::unique_ptr<IPlacementSource> src_;

    int    startKeyframe_ = 0;     ///< 启动时跳到的关键帧（--frame）
    bool   needFit_ = true;
    bool   glReady_ = false;      ///< GL 上下文是否已就绪
    bool   geomUploaded_ = false;
    std::string status_, bundlePath_;
    char pathBuf_[512] = {0};

    // ---- T14：连续回放（控制器 + 插值缓冲 + 持帧）
    PlaybackController playback_;
    std::vector<FrameMeta> metas_;
    std::vector<float> interpX_, interpY_;   ///< CPU 线性插值位置（百万节点 < 1ms）
    FrameView fvLo_, fvHi_;                  ///< 持住插值两端关键帧（shared_ptr 保活）
    FrameView statsFv_;                      ///< 懒统计当前取的关键帧
    int    statsKeyFrame_ = -1;              ///< 播放/拖动时懒统计锚定的关键帧
    bool   statsFromInterp_ = false;         ///< 已停：统计按插值位置
    double heatmapScale_ = 1.0;              ///< 热力图固定标尺（目标密度 1.0）
    double lastFrameTime_ = 0.0;
    bool   scrubbingSlider_ = false;
    bool   loopOn_ = false;
    int    prefetchDir_ = 1;          ///< T17：最近一次播放/拖动方向（停下保持）
    double lastPlayhead_ = 0.0;
    std::vector<double> benchUiTimes_;  ///< T14 端到端 UI 基准每帧耗时
    std::vector<double> benchUiRender_; ///< 渲染段（不含交换）
    std::vector<double> benchUiSwap_;   ///< 交换段
    int    benchUiLodFrames_ = 0;
    int    benchUiPlaying_ = 0, benchUiInterp_ = 0;
    int    benchUiLoops_ = 0;
    double benchUiTmin_ = 1e18, benchUiTmax_ = -1e18;
    double benchUiPrevVt_ = -1.0;
    std::string benchCsv_;
    std::string benchCsvPath_;          ///< 非空则把每帧 t/ms/lo/hi/alpha/playing 写 CSV

    // 画布面板在窗口里的位置与尺寸（像素），交互与投影都要用
    float canvasX_ = 0, canvasY_ = 0, canvasW_ = 1, canvasH_ = 1;
    double drawMs_ = 0.0;
    std::vector<float> xs_;      ///< 曲线横轴（全局行序号），按需重建
    bool   layoutBuilt_ = false;
    unsigned int dockId_ = 0;   ///< 宿主 dockspace id（T18 放行判定）

    // ---- T2：resize 保持视野（记住上一帧画布尺寸）
    float prevCanvasW_ = -1.f, prevCanvasH_ = -1.f;

    // ---- T4：空间索引 + 选中项
    SpatialIndex pickIndex_;
    bool indexDirty_ = true;
    int  selected_ = -1;
    int  selectedNet_ = -1;   ///< T10：点检视面板 net 名后只高亮该 net
    std::string selectedNameBuf_;       ///< T24：左上角读数显示选中名
    float downX_ = 0.f, downY_ = 0.f;   ///< 按下位置，区分单击与拖拽

    // ---- T5：metrics 行 ↔ 抽样帧映射
    FrameMap frameMap_;
    bool mapBuilt_ = false;

    // ---- T6：密度场 / 热力图 / LOD
    DensityField density_;
    bool densityDirty_ = true;
    int  densityCacheKey_ = -1;
    bool densityCacheOk_ = false;
    std::vector<float> densityCacheField_;
    int  densityCacheCols_ = 0, densityCacheRows_ = 0;
    float densityCacheMax_ = 0.f;
    int  lastSyncLo_ = -1, lastSyncHi_ = -1;
    double lastSyncAlpha_ = -1.0;
    // 性能剖析（ms/帧）：密度构建/插值/上传/索引/画布绘制
    double profDensity_ = 0.0, profInterp_ = 0.0, profUpload_ = 0.0;
    double profIndex_ = 0.0, profDraw_ = 0.0;
    double profSubmitMs_ = 0.0, profCompletionMs_ = 0.0;
    double profFrame_ = 0.0, profLerp_ = 0.0;
    // 关键帧密度后台预计算（128x128 ≈64KB/帧）：消除播放中每帧 O(n) 重建。
    viz::DensityCache preDensityCache_;
    // 同步时跳过 GL 上传（LOD 且播放）置位；近景绘制前若为脏则补传当前位置。
    bool gpuPositionsDirty_ = false;
    bool positionsValid_ = false;   ///< 本帧 frame() 成功 materialize；false 时近景不画旧 buffer
    const float* lastPx_ = nullptr;
    const float* lastPy_ = nullptr;
    int lastN_ = 0;
    unsigned long lastHmGen_ = 0;  ///< 已上传纹理对应的密度场版本，避免每帧重复上传
    bool heatmapOverlay_ = false;  ///< T20：热力图叠加图层（默认关）
    float heatmapAlpha_ = 0.35f;   ///< 热力图叠加透明度
    bool lodActive_ = false;       ///< 本帧是否处于自动降级热力图（节点>100万且行高<1px）

    // ---- T7：着色模式（net 数因 T10 无 nets.bin 而禁用）
    ColorMode colorMode_ = ColorMode::Kind;
    int  dispRefMode_ = 0;
    float dispRange_ = -1.f;
    bool  dispRangeDirty_ = true;
    int  netDegThreshold_ = 100;  ///< T10 高亮跳过度数大于该值的 net
    float lastValMin_ = 0.f, lastValMax_ = 1.f;   ///< 连续色表当前范围（图例用）

    // ---- T8：两帧对比与位移向量
    bool compareOn_ = false;
    bool showVectors_ = true;
    std::vector<float> dispValues_;
    std::vector<float> netCountValues_;
    std::vector<float> densityScratch_;   ///< T6 着色复用缓冲，避免每帧堆分配

    // ---- T36：界面缩放（画布文字仍固定 17px，见 drawCanvasPanel）
    float uiScale_ = 1.f;

    // ---- T32：写死的数据源选项
    struct DataSourceEntry {
        std::string label;
        std::string dir;
        bool canPlace = false;
        bool exists = false;
    };
    std::vector<DataSourceEntry> dataSources_;
    int dataSourceIndex_ = -1;

    // ---- T30：底部标签页切换（place 开始切终端，完成切回收敛曲线）
    // 首帧强制激活「收敛曲线」；无 ini 全新启动时默认页不再是终端。
    std::string focusTab_ = "收敛曲线";

    // ---- T33：终端与现场布局子进程
    char terminalInput_[256] = {0};
    std::vector<std::string> terminalLines_;
    pid_t placePid_ = -1;
    bool placeRunning_ = false;
    bool placeStopping_ = false;        ///< 已发 SIGTERM，等待子进程退出/兜底升级
    double placeStopSec_ = 0.0;         ///< 发 SIGTERM 的时刻（用于 3s 后升级 SIGKILL）
    std::string placeStage_;          ///< 上次上报的阶段（仅切换时打一行）
    int placeIter_ = -1;              ///< 常驻状态行显示的阶段内轮号
    double placeStartSec_ = 0.0;       ///< 布局启动时刻（glfwGetTime）
    double placeLastPoll_ = 0.0;       ///< 上次轮询 scene.json 的时刻
    std::string placeTargetDir_;       ///< 完成后自动加载的 bundle 目录
};

}  // namespace viz
