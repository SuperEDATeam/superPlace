// 查看器主程序。
//
// App 不知道数据从哪来——它只持有一个 IPlacementSource。
// 独立版注入 BundleSource，集成版注入 LiveSource，UI 代码完全相同。
#pragma once

#include <memory>
#include <vector>
#include <string>

#include "render/canvas.h"
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
    void gotoFrame(int index);
    void uploadGeometry();
    int  runBench();
    void saveScreenshot(const std::string& path, int w, int h);

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
    std::vector<float> xs_;      ///< 曲线横轴（行序号），按需重建
    bool   layoutBuilt_ = false;
};

}  // namespace viz
