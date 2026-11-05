#include "app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>   // DockBuilder：搭默认停靠布局
#include <implot.h>

#include "render/scale_tick.h"
#include "viz/summary.h"
#include "viz/nets.h"
#include "viz/displacement.h"

namespace viz {
namespace {

const char* kKindName[] = {"Filler", "固定终端", "标准单元", "宏单元"};
const ImVec4 kKindColor[] = {
    {0.31f, 0.75f, 0.35f, 1.f}, {0.24f, 0.43f, 0.86f, 1.f},
    {0.86f, 0.24f, 0.24f, 1.f}, {0.96f, 0.59f, 0.16f, 1.f},
};

// T5：各阶段曲线配色。未知阶段落到灰色。
ImVec4 stageColor(const std::string& s) {
    if (s == "mGP")        return {0.86f, 0.24f, 0.24f, 1.f};
    if (s == "FILLERONLY") return {0.96f, 0.59f, 0.16f, 1.f};
    if (s == "cGP")        return {0.24f, 0.43f, 0.86f, 1.f};
    if (s == "mLG")        return {0.62f, 0.40f, 0.86f, 1.f};
    return {0.50f, 0.50f, 0.50f, 1.f};
}

void glfwErrorCb(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] %d: %s\n", code, desc);
}

bool fileExists(const char* p) {
    if (!p || !*p) return false;
    FILE* f = std::fopen(p, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

/// 问 fontconfig 要一个真正带中文字体的字体。
///
/// 只靠硬编码路径不够：各发行版的字体目录各不相同，而 fontconfig 在任何
/// 桌面 Linux 上都有。
///
/// ⚠️ 这里**必须用 `fc-list` 而不是 `fc-match`**。`fc-match` 永远会返回一个
/// "最佳匹配"，哪怕那个字体一个汉字都没有——本机实测它对 `:lang=zh` 返回
/// DejaVuSans，结果既渲染不出中文，又盖掉了后面本来能命中的候选路径。
/// `fc-list :lang=zh` 只列出真正声明了中文覆盖的字体，没有就返回空。
std::string queryFontconfig() {
    FILE* pipe = popen("fc-list :lang=zh --format='%{file}\\n' 2>/dev/null", "r");
    if (!pipe) return {};
    char line[1024];
    std::string found;
    while (std::fgets(line, sizeof(line), pipe)) {
        std::string p(line);
        while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
        if (fileExists(p.c_str())) {
            found = p;
            break;
        }
    }
    pclose(pipe);
    return found;
}

/// 按"环境变量 → fontconfig → 常见路径"的顺序找中文字体。
void loadCjkFont(ImGuiIO& io) {
    std::string path;
    const char* env = std::getenv("SUPERPLACE_FONT");
    if (fileExists(env)) {
        path = env;
    } else {
        if (env && *env)
            std::fprintf(stderr, "[font] SUPERPLACE_FONT 指向的文件不存在：%s\n", env);
        path = queryFontconfig();
    }
    if (path.empty()) {
        // fontconfig 不可用时的兜底。覆盖 Debian/Ubuntu、Fedora、Arch、openSUSE
        // 以及 WSL 借用 Windows 字体这几种情况。
        static const char* kCandidates[] = {
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",   // Debian/Ubuntu
            "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
            "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", // Fedora
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",        // Arch
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc",
            "/usr/share/fonts/truetype/arphic/uming.ttc",
            "/mnt/c/Windows/Fonts/msyh.ttc",                            // WSL 借 Windows
        };
        for (const char* c : kCandidates) {
            if (fileExists(c)) {
                path = c;
                break;
            }
        }
    }

    if (path.empty()) {
        std::fprintf(stderr,
                     "[font] 未找到中文字体，界面中文会显示为方块。装一个即可：\n"
                     "       Debian/Ubuntu: sudo apt install fonts-noto-cjk\n"
                     "       Fedora:        sudo dnf install google-noto-sans-cjk-fonts\n"
                     "       Arch:          sudo pacman -S noto-fonts-cjk\n"
                     "       或用 SUPERPLACE_FONT=/path/to/font.ttc 指定\n");
        return;
    }
    io.Fonts->AddFontFromFileTTF(path.c_str(), 17.0f, nullptr,
                                 io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    std::fprintf(stderr, "[font] %s\n", path.c_str());
}

std::string human(long long v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

}  // namespace

App::App(AppOptions opt) : opt_(std::move(opt)) {
    benchCsvPath_ = opt_.benchCsvPath;
}

App::~App() {
    preDensityCache_.stop();
    preDensityCache_.join();
    if (window_) {
        canvas_.shutdown();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

void App::setSource(std::unique_ptr<IPlacementSource> src) {
    // 先停旧预计算 worker 并 join，再销毁/替换旧 src_，杜绝 worker 读已析构源的 UAF。
    preDensityCache_.stop();
    preDensityCache_.join();
    gpuPositionsDirty_ = true;
    positionsValid_ = false;
    lastPx_ = nullptr; lastPy_ = nullptr; lastN_ = 0;
    src_ = std::move(src);
    needFit_ = true;
    geomUploaded_ = false;
    fvLo_ = fvHi_ = FrameView();
    statsFv_ = FrameView();
    statsKeyFrame_ = -1;
    statsFromInterp_ = false;
    dispRangeDirty_ = true;
    netCountValues_.clear();
    selectedNet_ = -1;
    // 重置全部派生状态
    selected_ = -1;
    indexDirty_ = true;
    densityDirty_ = true;
    // 显式清空密度缓存，不依赖首次 build 覆盖——否则未 build 新源前 play 会误命中旧源场。
    densityCacheKey_ = -1;
    densityCacheOk_ = false;
    densityCacheField_.clear();
    lastSyncLo_ = lastSyncHi_ = -1;
    lastSyncAlpha_ = -1.0;
    lastHmGen_ = 0;
    mapBuilt_ = false;
    prevCanvasW_ = prevCanvasH_ = -1.f;
    colorMode_ = ColorMode::Kind;
    compareOn_ = false;

    // T14：从 frameInfo 建回放时间轴（只读元数据，不触发位置 I/O）。
    metas_.clear();
    if (src_) {
        metas_.reserve(static_cast<size_t>(src_->frameCount()));
        for (int i = 0; i < src_->frameCount(); ++i)
            metas_.push_back(src_->frameInfo(i));
        playback_.setFrames(metas_);
        playback_.setSpeed(60.0);   // 默认 60 轮/秒
        playback_.setLoop(false);
        playback_.pause();
        const size_t n = static_cast<size_t>(src_->scene().nodeCount);
        interpX_.assign(n, 0.f);
        interpY_.assign(n, 0.f);
        status_ = src_->scene().benchmark + " 已加载";
    }
    // 真正的上传推迟到 uploadGeometry()，由主循环在上下文就绪后触发。
    uploadGeometry();
    startPreDensity();
}

void App::startPreDensity() {
    if (!src_) return;
    const int nf = src_->frameCount();
    // 预算：每关键帧场 128x128x4B=64KB，最多 1024 帧（≈64MiB）；超出不预计算，播放退化当场 build。
    constexpr int kMaxPreDensityFrames = 1024;
    // 捕获启动时稳定的源指针：setSource 已先 join 旧 worker 再换 src_，源生命周期覆盖本线程。
    IPlacementSource* s = src_.get();
    viz::DensityField df;
    preDensityCache_.start(nf, kMaxPreDensityFrames, [s, df](int i) mutable {
        FrameView fv;
        viz::DensityFieldData out;
        if (!s->frame(i, fv)) return out; // 失败帧 ok=false，调用方退化当场 build
        df.build(s->scene().chip, fv.x, fv.y, s->geometry(), 128, 128);
        out.data = df.field();
        out.maxNorm = df.maxNorm();
        out.ok = true;
        return out;
    });
}

void App::uploadGeometry() {
    if (!glReady_ || !src_ || geomUploaded_) return;
    canvas_.setGeometry(src_->geometry());
    geomUploaded_ = true;
    syncPlayback();
}

bool App::initWindow() {
    glfwSetErrorCallback(glfwErrorCb);
    if (!glfwInit()) return false;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    window_ = glfwCreateWindow(opt_.width, opt_.height, "superPlace Viewer", nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr,
                     "创建窗口失败。请确认已装 GLFW/Mesa 开发包且具备可用显示（DISPLAY）。\n");
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(opt_.vsync ? 1 : 0);

    if (!glLoad()) {
        std::fprintf(stderr, "OpenGL 入口加载失败，驱动可能过旧\n");
        return false;
    }
    std::fprintf(stderr, "[gl] renderer = %s\n", glGetString(GL_RENDERER));
    std::fprintf(stderr, "[gl] version  = %s\n", glGetString(GL_VERSION));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // 多面板停靠
    io.IniFilename = "superplace-gui.ini";              // 记住用户的面板布局
    ImGui::StyleColorsLight();
    loadCjkFont(io);

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::string err;
    if (!canvas_.init(&err)) {
        std::fprintf(stderr, "画布初始化失败：%s\n", err.c_str());
        return false;
    }
    glReady_ = true;
    return true;
}

void App::gotoKeyframe(int index) {
    if (!src_ || metas_.empty()) return;
    // 用显式 pin 按帧 index 精确定位（同 arrival 多帧也能逐个到达，不再二分歧义）。
    playback_.pinIndex(index);
    syncPlayback();
}

void App::gotoFrame(int index) { gotoKeyframe(index); }

/// 懒统计（空间索引/密度/连续着色）使用的位置来源。
/// 播放/拖动时锚定最近关键帧 statsKeyFrame_；停止时用当前显示位置（插值或单帧）。
bool App::statsSource(const float** sx, const float** sy) {
    if (!src_ || metas_.empty()) return false;
    if (statsKeyFrame_ >= 0) {
        if (!src_->frame(statsKeyFrame_, statsFv_)) return false;
        *sx = statsFv_.x;
        *sy = statsFv_.y;
        return true;
    }
    // 停止：用 fvLo_（单帧）或插值缓冲。
    const PlaybackController::DisplayState ds = playback_.state();
    if (ds.interpolating) {
        *sx = interpX_.data();
        *sy = interpY_.data();
        return true;
    }
    if (fvLo_.valid()) {
        *sx = fvLo_.x;
        *sy = fvLo_.y;
        return true;
    }
    return false;
}

void App::syncPlayback() {
    if (!src_ || !geomUploaded_ || metas_.empty()) return;
    const int n = src_->scene().nodeCount;
    if (n <= 0) return;

    const PlaybackController::DisplayState ds = playback_.state();

    // ---- 位置 materialization：始终计算（供密度/点选/近景），仅 LOD且moving 时跳 canvas 上传。
    const float* px = nullptr;
    const float* py = nullptr;
    const auto tAcq0 = std::chrono::steady_clock::now();
    if (ds.interpolating && ds.lo >= 0 && ds.hi >= 0) {
        if (!src_->frame(ds.lo, fvLo_) || !src_->frame(ds.hi, fvHi_)) { positionsValid_=false; lastPx_ = nullptr; lastPy_ = nullptr; lastN_ = 0; status_ = "帧加载失败"; return; }
    } else {
        const int k = ds.lo >= 0 ? ds.lo : 0;
        if (!src_->frame(k, fvLo_)) { positionsValid_=false; lastPx_ = nullptr; lastPy_ = nullptr; lastN_ = 0; status_ = "帧加载失败"; return; }
    }
    const double acqMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tAcq0).count();
    const auto tLerp0 = std::chrono::steady_clock::now();
    if (ds.interpolating && ds.lo >= 0 && ds.hi >= 0) {
        const float* aX = fvLo_.x; const float* aY = fvLo_.y;
        const float* bX = fvHi_.x; const float* bY = fvHi_.y;
        const float w = static_cast<float>(ds.alpha);
        const float w1 = 1.f - w;
        for (int i = 0; i < n; ++i) {
            interpX_[static_cast<size_t>(i)] = aX[i] * w1 + bX[i] * w;
            interpY_[static_cast<size_t>(i)] = aY[i] * w1 + bY[i] * w;
        }
        px = interpX_.data();
        py = interpY_.data();
    } else {
        px = fvLo_.x;
        py = fvLo_.y;
    }
    const double lerpMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tLerp0).count();
    const auto tUp0 = std::chrono::steady_clock::now();
    const bool moving = playback_.playing() || scrubbingSlider_;
    // 只上传一次：LOD且播放跳上传并置脏；其余情况立即上传并清脏。
    lastPx_ = px; lastPy_ = py; lastN_ = n;
    if (viz::shouldUploadPositions(true, lastPx_ && lastPy_, lodActive_ && moving)) {
        canvas_.setPositions(px, py, n);
        gpuPositionsDirty_ = false;
    } else {
        gpuPositionsDirty_ = true;
    }
    const double upMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tUp0).count();
    profFrame_ = acqMs; profLerp_ = lerpMs; profUpload_ = upMs;
    positionsValid_ = true;

    // T17：按播放/拖动方向提示后台预读关键帧（不阻塞主循环）。
    // 播放前进；拖动时按 playhead 变化定方向；停下保持最近方向。
    int pdir = prefetchDir_;
    const double ph = playback_.playhead();
    if (playback_.playing()) {
        pdir = 1;
    } else if (scrubbingSlider_) {
        if (ph > lastPlayhead_ + 1e-9) pdir = 1;
        else if (ph < lastPlayhead_ - 1e-9) pdir = -1;
    }
    prefetchDir_ = pdir;
    lastPlayhead_ = ph;
    src_->prefetch(ds.statFrame, pdir);

    // ---- 懒统计锚定 ----
    if (moving) {
        if (ds.statFrame != statsKeyFrame_) {
            statsKeyFrame_ = ds.statFrame;
            statsFromInterp_ = false;
            indexDirty_ = true;
            densityDirty_ = true;
        }
    } else if (!statsFromInterp_) {
        statsFromInterp_ = true;
        statsKeyFrame_ = -1;
        indexDirty_ = true;
        densityDirty_ = true;
    }
    // 暂停状态下，点关键帧/阶段/曲线使显示 lo/hi/alpha 变化时，派生（热图/点选）也必须置脏，
    // 否则 statsFromInterp_ 已为 true 不会再触发，热图/点选停在上一帧。
    if (!moving &&
        (ds.lo != lastSyncLo_ || ds.hi != lastSyncHi_ ||
         std::abs(ds.alpha - lastSyncAlpha_) > 1e-6)) {
        indexDirty_ = true;
        densityDirty_ = true;
    }
    lastSyncLo_ = ds.lo;
    lastSyncHi_ = ds.hi;
    lastSyncAlpha_ = ds.alpha;
    updateContinuousValues();
}

void App::rebuildIndexIfDirty() {
    if (!indexDirty_ || !src_) return;
    const float* sx = nullptr; const float* sy = nullptr;
    if (statsSource(&sx, &sy)) {
        pickIndex_.build(src_->scene().chip, sx, sy, src_->geometry());
        indexDirty_ = false;
    }
}

// T24：点击时用当前插值位置强制重建索引（不依赖 statsSource 关键帧）。
void App::rebuildIndexAtCurrent() {
    if (!src_) return;
    const PlaybackController::DisplayState ds = playback_.state();
    FrameView lo, hi;
    src_->frame(std::max(0, ds.lo), lo);
    if (ds.hi > ds.lo) src_->frame(ds.hi, hi);
    const int n = src_->scene().nodeCount;
    interpX_.assign(n, 0.f); interpY_.assign(n, 0.f);
    for (int i = 0; i < n; ++i) {
        float x = lo.x[i], y = lo.y[i];
        if (hi.valid()) {
            x += ds.alpha * (hi.x[i] - x);
            y += ds.alpha * (hi.y[i] - y);
        }
        interpX_[i] = x; interpY_[i] = y;
    }
    pickIndex_.build(src_->scene().chip, interpX_.data(), interpY_.data(), src_->geometry());
}

void App::rebuildDensityIfDirty() {
    if (!densityDirty_ || !src_) return;
    const auto t0 = std::chrono::steady_clock::now();
    // 优先用后台预计算好的关键帧密度场（128x128 ≈64KB），播放切换锚时零 O(n) 重建。
    viz::DensityFieldData pre = preDensityCache_.get(statsKeyFrame_);
    if (pre.ok && !pre.data.empty()) {
        density_.setField(std::move(pre.data), 128, 128, pre.maxNorm);
        densityDirty_ = false;
        profDensity_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return;
    }
    // 冷启动：预计算尚未覆盖该锚帧，退化为当场 build（会随 worker 推进被覆盖）。
    const float* sx = nullptr; const float* sy = nullptr;
    if (statsSource(&sx, &sy)) {
        density_.build(src_->scene().chip, sx, sy, src_->geometry(), 128, 128);
        densityCacheKey_ = statsKeyFrame_;
        densityCacheField_ = density_.field();
        densityCacheCols_ = density_.gridCols();
        densityCacheRows_ = density_.gridRows();
        densityCacheMax_ = density_.maxNorm();
        densityCacheOk_ = true;
        densityDirty_ = false;
    }
    profDensity_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

void App::updateContinuousValues() {
    if (!src_ || !geomUploaded_ || metas_.empty()) return;
    const int n = src_->scene().nodeCount;

    if (compareOn_) {
        // T21：位移按参考三选（纯逻辑见 viz/displacement.h）。
        const PlaybackController::DisplayState ds = playback_.state();
        // 区间选择：hi>lo 用 lo/hi；否则停在 breakBefore 关键帧，用 prev/current（lo-1,lo）
        int loIdx = std::max(0, ds.lo), hiIdx = ds.hi;
        float useAlpha = ds.hi > ds.lo ? ds.alpha : 1.f;
        if (ds.hi == ds.lo && ds.lo > 0) {
            loIdx = ds.lo - 1; hiIdx = ds.lo;  // prev→current 硬跳变
        }
        FrameView flo, fhi;
        src_->frame(loIdx, flo);
        if (hiIdx > loIdx) src_->frame(hiIdx, fhi);
        // 参考帧
        int refFrame = 0;
        if (dispRefMode_ == 1) {
            for (int i = (int)metas_.size() - 1; i >= 0; --i)
                if (metas_[i].stage == "init") { refFrame = i; break; }
        } else if (dispRefMode_ == 2) {
            refFrame = 0;
        }
        FrameView fref;
        bool haveRef = dispRefMode_ != 0 && src_->frame(refFrame, fref) && fref.valid();
        if (flo.valid()) {
            dispValues_.assign((size_t)n, 0.f);
            viz::FrameViewLite L{flo.x, flo.y};
            viz::FrameViewLite H{fhi.valid() ? fhi.x : nullptr,
                                 fhi.valid() ? fhi.y : nullptr};
            viz::FrameViewLite R{haveRef ? fref.x : nullptr,
                                 haveRef ? fref.y : nullptr};
            viz::dispMagnitudes((viz::DispRef)dispRefMode_, L, H, R, useAlpha, n, dispValues_);
            // 固定色表范围：末帧(frameCount-1)相对参考帧 p99，一次回放内不变。
            if (dispRangeDirty_ || dispRange_ <= 0.f) {
                FrameView flast, flref;
                bool haveLast = src_->frame(src_->frameCount() - 1, flast) && flast.valid();
                bool haveRef2 = src_->frame(refFrame, flref) && flref.valid();
                viz::FrameViewLite L2{haveLast ? flast.x : nullptr, haveLast ? flast.y : nullptr};
                viz::FrameViewLite H2{nullptr, nullptr};
                viz::FrameViewLite R2{haveRef2 ? flref.x : nullptr, haveRef2 ? flref.y : nullptr};
                std::vector<float> mags;
                viz::dispMagnitudes((viz::DispRef)dispRefMode_, L2, H2, R2, 0.f, n, mags);
                dispRange_ = std::max(1e-6f, viz::percentile(mags, 0.99));
                dispRangeDirty_ = false;
            }
            canvas_.setColorMode(ColorMode::Displacement);
            canvas_.setContinuousValues(dispValues_.data(), n);
            canvas_.setValueRange(0.f, dispRange_);
            lastValMin_ = 0.f; lastValMax_ = dispRange_;
            return;
        }
    }

    if (colorMode_ == ColorMode::NetCount && src_->nets()) {
        // 每节点所连不同 net 个数（同 net 多引脚算一次），懒缓存。
        if ((int)netCountValues_.size() != n) {
            netCountValues_.assign(n, 0.f);
            const NetList* nl = src_->nets();
            for (int net = 0; net < nl->netCount; ++net) {
                int q0 = nl->netStart[net], q1 = nl->netStart[net + 1];
                std::vector<int> nodes;
                for (int q = q0; q < q1; ++q) nodes.push_back(nl->pinNode[nl->netPins[q]]);
                std::sort(nodes.begin(), nodes.end());
                nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
                for (int nd : nodes)
                    if (nd >= 0 && nd < n) netCountValues_[nd] += 1.f;
            }
        }
        float mx = 1.f;
        for (float v : netCountValues_) mx = std::max(mx, v);
        canvas_.setColorMode(ColorMode::NetCount);
        canvas_.setContinuousValues(netCountValues_.data(), n);
        canvas_.setValueRange(0.f, mx);
        lastValMin_ = 0.f; lastValMax_ = mx;
        return;
    }

    if (colorMode_ == ColorMode::Density) {
        rebuildDensityIfDirty();
        densityScratch_.assign(static_cast<size_t>(n), 0.f);
        float mx = 1e-6f;
        const float* sx = nullptr; const float* sy = nullptr;
        if (statsSource(&sx, &sy)) {
            for (int i = 0; i < n; ++i) {
                const float d = density_.sample(sx[i], sy[i]);
                densityScratch_[static_cast<size_t>(i)] = d;
                mx = std::max(mx, d);
            }
        }
        canvas_.setColorMode(ColorMode::Density);
        canvas_.setContinuousValues(densityScratch_.data(), n);
        canvas_.setValueRange(0.f, mx);
        lastValMin_ = 0.f;
        lastValMax_ = mx;
        return;
    }

    lastValMin_ = 0.f;
    lastValMax_ = 1.f;
    canvas_.setColorMode(ColorMode::Kind);
}

int App::run() {
    if (!initWindow()) return 1;

    if (!opt_.initialBundle.empty() && !src_) {
        std::string err;
        auto s = makeBundleSource(opt_.initialBundle, &err);
        if (s) {
            bundlePath_ = opt_.initialBundle;
            setSource(std::move(s));
        } else {
            status_ = "加载失败：" + err;
        }
    }

    std::strncpy(pathBuf_, bundlePath_.c_str(), sizeof(pathBuf_) - 1);

    if (opt_.benchFrames > 0) return runBench();

    uploadGeometry();   // 数据源可能在 run() 之前就注入了，此刻 GL 才真正可用
    if (opt_.benchUiFrames > 0) {
        // 真实 UI 端到端基准：关 vsync、循环播放（loopOn_ 也要置位，否则
        // 每帧 drawTimeline 会用 loopOn_=false 覆盖掉，导致播一圈就静止）。
        glfwSwapInterval(0);
        loopOn_ = true;
        if (src_) { playback_.setLoop(true); playback_.play(); }
    }
    if (src_ && opt_.startFrame != -999) {
        gotoFrame(opt_.startFrame < 0 ? src_->frameCount() - 1 : opt_.startFrame);
    }
    if (src_ && opt_.compareOn && src_->frameCount() >= 2) {
        compareOn_ = true;
        updateContinuousValues();
    }

    while (!glfwWindowShouldClose(window_)) {
        const auto iterT0 = std::chrono::steady_clock::now();
        glfwPollEvents();
        uploadGeometry();

        // T14：按真实帧间隔推进回放时间轴，再同步位置/懒统计。
        {
            const double now = glfwGetTime();
            const double dt = lastFrameTime_ > 0.0 ? now - lastFrameTime_ : 0.0;
            lastFrameTime_ = now;
            if (src_ && !metas_.empty()) {
                playback_.advance(dt);
                syncPlayback();
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawUi();
        ImGui::Render();

        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(window_, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.97f, 0.97f, 0.97f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 画布画在 ImGui 之前：它是背景内容，面板浮在上面。
        // 画的位置是 dockspace 中央节点那块矩形（drawCanvasPanel 里取的），
        // 通过设 glViewport 实现，不需要 scissor。
        if (src_ && canvasW_ > 1 && canvasH_ > 1) {
            const float dpr = fbH > 0 ? static_cast<float>(fbH) / ImGui::GetIO().DisplaySize.y : 1.f;
            const int vx = static_cast<int>(canvasX_ * dpr);
            const int vy = static_cast<int>((ImGui::GetIO().DisplaySize.y - canvasY_ - canvasH_) * dpr);
            const int vw = static_cast<int>(canvasW_ * dpr);
            const int vh = static_cast<int>(canvasH_ * dpr);
            glViewport(vx, vy, vw, vh);

            // T20：默认画真实矩形（33万矩形≈1.8ms 不需 LOD）。仅当节点>100万 且行高<1px
            // 时自动降级为热力图；heatmapOverlay_ 是可选叠加图层（默认关）。
            const float rowPx = src_->scene().rowHeight > 0.f
                                    ? src_->scene().rowHeight * cam_.scale : 0.f;
            lodActive_ = src_->scene().nodeCount > 1000000 && rowPx > 0.f && rowPx < 1.f;

            const auto t0 = std::chrono::steady_clock::now();
            // 密度场重建后才重新上传纹理，静止视角不重复 GL 上传。
            const auto uploadHm = [&](float alpha) {
                rebuildDensityIfDirty();
                if (density_.generation() != lastHmGen_) {
                    lastHmGen_ = density_.generation();
                    // T14：回放用固定标尺（目标密度 1.0），不随每帧 maxNorm
                    // 归一化——否则同一密度在不同帧颜色不同，盖过真实扩散过程。
                    canvas_.setHeatmap(density_.field().data(), density_.gridCols(),
                                       density_.gridRows(), heatmapScale_);
                }
                canvas_.drawHeatmapOverlay(cam_, vw, vh, src_->scene().chip, alpha);
            };
            if (lodActive_) {
                if (viz::shouldDrawPositions(positionsValid_)) uploadHm(0.9f);
            } else {
                if (gpuPositionsDirty_ &&
                    viz::shouldUploadPositions(positionsValid_, lastPx_ && lastPy_, false)) {
                    canvas_.setPositions(lastPx_, lastPy_, lastN_);
                    gpuPositionsDirty_ = false;
                }
                if (viz::shouldDrawPositions(positionsValid_)) {
                    if (heatmapOverlay_) uploadHm(heatmapAlpha_);
                    canvas_.draw(cam_, vw, vh);
                }
            }
            drawMs_ = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
            profDraw_ = drawMs_;
            glViewport(0, 0, fbW, fbH);
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glFinish();   // 端到端计时等 GPU 完成，仅 bench-ui 路径（正常 UI 不加）

        // T14：真实 UI 端到端基准——渲染段（推进+插值+drawUi+LOD+绘制+finish）计时。
        double renderMs = 0.0;
        if (opt_.benchUiFrames > 0) {
            renderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - iterT0).count();
        }
        const auto swapT0 = std::chrono::steady_clock::now();
        glfwSwapBuffers(window_);
        if (opt_.autoShotAfterSec > 0.0 && glfwGetTime() >= opt_.autoShotAfterSec) {
            if (!opt_.screenshot.empty()) saveScreenshot(opt_.screenshot, fbW, fbH);
            std::fprintf(stderr, "[viz] 自动截图后退出，当前帧数 %d\n",
                         src_ ? src_->frameCount() : 0);
            glfwSetWindowShouldClose(window_, 1);
        }

        // T14 基准统计在交换之后结算：render 段 + swap 段分开，边界清晰。
        if (opt_.benchUiFrames > 0) {
            const double swapMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - swapT0).count();
            const PlaybackController::DisplayState ds = playback_.state();
            benchUiRender_.push_back(renderMs);
            benchUiSwap_.push_back(swapMs);
            benchUiTimes_.push_back(renderMs + swapMs);
            if (lodActive_) ++benchUiLodFrames_;
            if (playback_.playing()) ++benchUiPlaying_;
            if (ds.interpolating) ++benchUiInterp_;
            if (ds.globalT < benchUiTmin_) benchUiTmin_ = ds.globalT;
            if (ds.globalT > benchUiTmax_) benchUiTmax_ = ds.globalT;
            if (benchUiPrevVt_ >= 0 && ds.vt < benchUiPrevVt_ - 1e-6) ++benchUiLoops_;
            benchUiPrevVt_ = ds.vt;
            benchCsv_ += std::to_string(benchUiTimes_.size()) + ","
                        + std::to_string(renderMs) + "," + std::to_string(swapMs) + ","
                        + std::to_string(profFrame_) + "," + std::to_string(profLerp_) + ","
                        + std::to_string(profUpload_) + "," + std::to_string(profDensity_) + ","
                        + std::to_string(profDraw_) + ","
                        + std::to_string(ds.globalT) + "," + std::to_string(ds.lo) + ","
                        + std::to_string(ds.hi) + "," + std::to_string(ds.alpha) + ","
                        + std::to_string(statsKeyFrame_) + "," + (lodActive_ ? "1" : "0") + ","
                        + std::to_string(preDensityCache_.ready()) + ","
                        + (playback_.playing() ? "1" : "0") + "\n";
            if (static_cast<int>(benchUiTimes_.size()) >= opt_.benchUiFrames) {
                auto v = benchUiTimes_;
                std::sort(v.begin(), v.end());
                const size_t sz = v.size();
                double sum = 0; for (double x : benchUiTimes_) sum += x;
                const double avg = sum / sz;
                const double p50 = v[sz / 2];
                const double p95 = v[static_cast<size_t>(0.95 * (sz - 1))];
                const double mx = v.back();
                int over33 = 0; for (double x : v) if (x > 33.0) ++over33;
                std::printf("[bench-ui] 节点 %d  采样帧数 %zu  LOD热图帧 %d  实际循环 %d\n",
                            src_ ? src_->scene().nodeCount : 0, sz, benchUiLodFrames_,
                            benchUiLoops_);
                std::printf("[bench-ui] playing %d/%zu  interpolating %d/%zu  globalT[%.1f, %.1f]\n",
                            benchUiPlaying_, sz, benchUiInterp_, sz, benchUiTmin_, benchUiTmax_);
                std::printf("[bench-ui] 每帧(渲染+交换) avg %.2f  p50 %.2f  p95 %.2f  max %.2f ms  >33ms %d帧 => avg fps %.1f\n",
                            avg, p50, p95, mx, over33, 1000.0 / std::max(avg, 1e-6));
                double swapSum = 0; for (double x : benchUiSwap_) swapSum += x;
                std::printf("[bench-ui] 交换段 avg %.2f ms（vsync 关闭）\n",
                            swapSum / sz);
                if (!benchCsvPath_.empty()) {
                    std::ofstream cf(benchCsvPath_);
                    cf << "frame,render_ms,swap_ms,frameAcq_ms,lerp_ms,upload_ms,density_ms,draw_ms,globalT,lo,hi,alpha,anchor,lod,preReady,playing\n" << benchCsv_;
                    std::printf("[bench-ui] CSV 已写 %s\n", benchCsvPath_.c_str());
                }
                if (!opt_.screenshot.empty()) saveScreenshot(opt_.screenshot, fbW, fbH);
                break;
            }
        }
    }
    return 0;
}

/// 基准模式：不开 ImGui 面板，纯粹反复画画布并计时。
///
/// 关掉 vsync 才能测出真实上限——否则帧时间会被钳在 16.7 ms，看起来"刚好够用"，
/// 实际留了多少余量一无所知。
int App::runBench() {
    uploadGeometry();
    if (!src_) {
        std::fprintf(stderr, "基准模式需要一个 bundle\n");
        return 1;
    }
    glfwSwapInterval(0);
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    cam_.fit(src_->scene().chip, static_cast<float>(fbW), static_cast<float>(fbH));

    // T14：bench 真正跑连续回放——循环播放、按 1/60 s 推进时间轴，
    // 把插值与位置上传都算进负载，而不是逐帧硬跳。
    playback_.setLoop(true);
    playback_.play();
    const double benchDt = 1.0 / 60.0;

    std::vector<double> tCpu, tGpu, tLerp;
    tCpu.reserve(static_cast<size_t>(opt_.benchFrames));
    tGpu.reserve(static_cast<size_t>(opt_.benchFrames));
    for (int i = 0; i < opt_.benchFrames; ++i) {
        glfwPollEvents();

        // 1) CPU 段：推进时间轴 + 取帧(IO/缓存) + 线性插值 + 位置缓冲上传。
        const auto c0 = std::chrono::steady_clock::now();
        playback_.advance(benchDt);
        // 插值 CPU 成本单独计（仅 interpolating 时非零）。
        const auto l0 = std::chrono::steady_clock::now();
        syncPlayback();
        const auto l1 = std::chrono::steady_clock::now();
        tCpu.push_back(std::chrono::duration<double, std::milli>(l1 - c0).count());
        tLerp.push_back(std::chrono::duration<double, std::milli>(l1 - l0).count());

        // 2) GPU 段：清屏 + 实例化绘制 + glFinish 等待光栅化完成。
        glViewport(0, 0, fbW, fbH);
        glClearColor(1.f, 1.f, 1.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        const auto g0 = std::chrono::steady_clock::now();
        canvas_.draw(cam_, fbW, fbH);
        glFinish();                         // 不 finish 测到的只是提交耗时
        tGpu.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - g0).count());
        glfwSwapBuffers(window_);
    }
    if (!opt_.screenshot.empty()) saveScreenshot(opt_.screenshot, fbW, fbH);

    auto med = [](std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::printf("节点 %d  视口 %dx%d  帧数 %zu  阶段 %s\n",
                src_->scene().nodeCount, fbW, fbH, tGpu.size(),
                playback_.state().stage.c_str());
    std::printf("CPU(推进+取帧+插值+上传) 中位 %.2f ms | 其中插值 中位 %.2f ms\n",
                med(tCpu), med(tLerp));
    std::printf("GPU(清屏+实例绘制+finish) 中位 %.2f ms => %.0f fps\n",
                med(tGpu), 1000.0 / std::max(med(tGpu), 1e-6));
    return 0;
}

void App::saveScreenshot(const std::string& path, int w, int h) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    for (int y = h - 1; y >= 0; --y)   // GL 原点在左下，PPM 在左上
        out.write(reinterpret_cast<char*>(px.data() + static_cast<size_t>(y) * w * 3), w * 3);
    std::fprintf(stderr, "[viz] 截图已保存: %s\n", path.c_str());
}

void App::drawUi() {
    // 新版 ImGui 的签名是 (dockspace_id, viewport, flags)，旧版是 (viewport, flags)
    const ImGuiID dockId = ImGui::DockSpaceOverViewport(
        0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
    dockId_ = (unsigned int)dockId;

    // 首次运行搭一套默认布局。不这么做的话三个面板会各自浮着，而且"布局画布"
    // 会塌缩——它的尺寸取自窗口可用区，内容又按该尺寸建 InvisibleButton，
    // 自适应窗口下这是个会一路缩到最小的反馈环（实测缩成 16x1）。
    // 停靠到中央节点后尺寸由 dockspace 决定，环就断了。
    // 首次运行搭默认布局。只把右侧"控制/检视"交给 DockBuilder；底部"收敛曲线/帧对比"
    // 用固定尺寸窗口钉在视口底部（见 drawMetricsPanel/drawComparePanel），避免空中央
    // 节点每帧重排把底部节点收成一条标签条。
    if (!layoutBuilt_) {
        const bool ready =
            ImGui::FindWindowByName("控制") && ImGui::FindWindowByName("检视");
        if (ready) {
            layoutBuilt_ = true;
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace |
                                                  ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);
            ImGuiID right = 0, center = dockId, bottom = 0, rightTop = 0;
            // 中央节点【故意留空】，GL 画布直接画进它的矩形，见 drawCanvasPanel 的说明
            right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
            bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
            // T24：右栏上下切分——控制在上、检视在下，各自独立节点（非标签页）。
            rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.55f, nullptr, &right);
            ImGui::DockBuilderDockWindow("控制", rightTop);
            ImGui::DockBuilderDockWindow("检视", right);
            ImGui::DockBuilderDockWindow("收敛曲线", bottom);
            ImGui::DockBuilderFinish(dockId);
        }
    }
    drawCanvasPanel(dockId);
    drawSidebar();
    drawMetricsPanel();
    drawInspectPanel();
}

void App::drawCanvasPanel(unsigned dockId) {
    // 画布不是一个 ImGui 窗口，而是 dockspace【中央节点】那块留空的矩形。
    //
    // 为什么不能用 ImGui 窗口装它：PassthruCentralNode 的语义是"中央节点【为空时】
    // 才透出背景"。一旦把窗口停靠进中央节点，dockspace 就会照常铺一层 WindowBg，
    // 把底下的 GL 内容整个盖掉（实测画布区域像素恰好等于 ImGuiCol_WindowBg 的 240）。
    // 让中央节点保持空、我们自己往那块矩形里画，才是这个 flag 的设计用法。
    // dockspace 的 id 必须用 DockSpaceOverViewport 的返回值，不能自己拼字符串猜——
    // 猜错时 DockBuilderGetCentralNode 返回 null，画布会静默地停在 1x1。
    ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockId);
    if (!central) return;

    const float newW = std::max(central->Size.x, 1.f);
    // T19：画布矩形 = 中央节点矩形（收敛曲线已停靠进底部子节点，不再用估算保留带）。
    const float newH = std::max(central->Size.y, 1.f);

    // T2：窗口/面板尺寸变化后保持视野。中心不动，scale 按短边比例缩放，
    // 绝不重新 fit（否则丢掉用户缩放到的局部）。
    if (!needFit_ && prevCanvasW_ > 0.f &&
        (std::fabs(newW - prevCanvasW_) > 0.5f || std::fabs(newH - prevCanvasH_) > 0.5f)) {
        cam_.rescaleOnResize(prevCanvasW_, prevCanvasH_, newW, newH);
    }
    prevCanvasW_ = newW;
    prevCanvasH_ = newH;

    canvasX_ = central->Pos.x;
    canvasY_ = central->Pos.y;
    canvasW_ = newW;
    canvasH_ = newH;

    if (src_ && needFit_ && canvasW_ > 32.f && canvasH_ > 32.f) {
        cam_.fit(src_->scene().chip, canvasW_, canvasH_);
        needFit_ = false;
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 lo(canvasX_, canvasY_);
    const ImVec2 hi(canvasX_ + canvasW_, canvasY_ + canvasH_);
    // T18：clip=false 不受隐形 Debug##Default 窗口裁剪框截断；鼠标下无任何 ImGui 窗口才算画布。
    // T18：clip=false 不受隐形 Debug##Default 裁剪框截断；鼠标下无任何 ImGui 窗口才算画布。
    const bool mouseInCanvas = ImGui::IsMouseHoveringRect(lo, hi, /*clip=*/false);
    const bool overCanvas = mouseInCanvas &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
    // 拖拽平移：只有「按下时就在画布上」才允许，避免从面板拖入误平移。
    static bool panning = false;
    if (overCanvas && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        downX_ = io.MousePos.x;
        downY_ = io.MousePos.y;
        panning = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) panning = false;
    if (panning && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        cam_.cx -= io.MouseDelta.x / cam_.scale;
        cam_.cy += io.MouseDelta.y / cam_.scale;
    }
    // F 键 = 适应窗口
    if (mouseInCanvas && ImGui::IsKeyPressed(ImGuiKey_F)) needFit_ = true;
    // T4：左键【单击】（非拖拽）点选；点空白处取消选中。
    if (overCanvas && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const float moved = std::hypot(io.MousePos.x - downX_, io.MousePos.y - downY_);
        if (moved < 4.f && src_) {
            float wx = 0.f, wy = 0.f;
            cam_.screenToWorld(io.MousePos.x - canvasX_, io.MousePos.y - canvasY_,
                               canvasW_, canvasH_, &wx, &wy);
            rebuildIndexAtCurrent();
            // T24：5px 屏幕容差（5/scale），先真实几何再容差内最近。
            selected_ = pickIndex_.pickTolerant(wx, wy, 5.f / cam_.scale, canvas_.layerVisible);
        }
    }
    if (overCanvas && io.MouseWheel != 0.f) {
        cam_.zoomAt(io.MousePos.x - canvasX_, io.MousePos.y - canvasY_, canvasW_, canvasH_,
                    std::exp(io.MouseWheel * 0.25f));
    }

    // T19：坐标轴/比例尺/读数/选中框/箭头画背景层（GL 画布之上、面板之下），clip 到画布。
    ImDrawList* fg = ImGui::GetBackgroundDrawList();
    fg->PushClipRect(lo, hi, true);

    if (src_) {
        // ---- T3：坐标读数
        char buf[160];
        if (overCanvas) {
            float wx = 0, wy = 0;
            cam_.screenToWorld(io.MousePos.x - canvasX_, io.MousePos.y - canvasY_, canvasW_,
                               canvasH_, &wx, &wy);
            std::snprintf(buf, sizeof(buf), "(%.2f, %.2f)   %.4f px/unit   滚轮缩放 · 拖拽平移",
                          wx, wy, cam_.scale);
        } else {
            std::snprintf(buf, sizeof(buf), "%.4f px/unit   滚轮缩放 · 拖拽平移", cam_.scale);
        }
        if (!selectedNameBuf_.empty()) {
            size_t l = std::strlen(buf);
            std::snprintf(buf + l, sizeof(buf) - l, "%s", selectedNameBuf_.c_str());
        }
        // T20：降级热力图提示
        if (lodActive_)
            fg->AddText(ImVec2(canvasX_ + 10, canvasY_ + 30),
                        IM_COL32(255, 160, 0, 255),
                        "热力图模式：图层 / 着色在此模式下不生效，放大后恢复");
        // T20：连续色表色条（与 canvas.cpp shader cmap 同一组 viridis 停点）。
        if (colorMode_ != ColorMode::Kind || compareOn_) {
            const float cw = 180.f, ch = 8.f;
            const float cx0 = canvasX_ + canvasW_ - cw - 14, cy0 = canvasY_ + canvasH_ - 14;
            const float stops[5][3] = {{0.267f,0.005f,0.329f},{0.229f,0.322f,0.545f},
                                       {0.127f,0.566f,0.550f},{0.369f,0.789f,0.383f},
                                       {0.992f,0.906f,0.144f}};
            auto vmap = [&](float t) {
                t = std::clamp(t, 0.f, 1.f);
                int s = std::min(4, (int)(t * 4.f));
                float lt = t * 4.f - s;
                if (s >= 4) { s = 3; lt = 1.f; }
                float r = stops[s][0] + lt * (stops[s+1][0] - stops[s][0]);
                float g = stops[s][1] + lt * (stops[s+1][1] - stops[s][1]);
                float b = stops[s][2] + lt * (stops[s+1][2] - stops[s][2]);
                return IM_COL32((int)(r*255), (int)(g*255), (int)(b*255), 255);
            };
            for (int i = 0; i < 24; ++i) {
                float t = i / 23.f;
                ImU32 col = vmap(t);
                fg->AddRectFilled({cx0 + cw * t, cy0}, {cx0 + cw * (i + 1) / 23.f, cy0 + ch}, col);
            }
            char cb[48];
            std::snprintf(cb, sizeof(cb), "%.3g ~ %.3g  (设计坐标单位)", lastValMin_, lastValMax_);
            fg->AddText({cx0, cy0 - 14}, IM_COL32(90, 90, 90, 255), cb);
        }
        fg->AddText(ImVec2(canvasX_ + 10, canvasY_ + 10),
                    IM_COL32(90, 90, 90, 255), buf);

        // ---- T3：左下角自适应比例尺（1/2/5×10ⁿ）+ 底部/左侧坐标刻度
        const float barLen = scaleBarLength(140.f, cam_.scale);
        const float barPx = barLen * cam_.scale;
        const ImVec2 barA(canvasX_ + 12, canvasY_ + canvasH_ - 48);
        const ImVec2 barB(barA.x + barPx, barA.y);
        fg->AddLine(barA, barB, IM_COL32(60, 60, 60, 255), 2.f);
        fg->AddLine({barA.x, barA.y - 4}, {barA.x, barA.y + 4}, IM_COL32(60, 60, 60, 255), 2.f);
        fg->AddLine({barB.x, barB.y - 4}, {barB.x, barB.y + 4}, IM_COL32(60, 60, 60, 255), 2.f);
        char lab[64];
        std::snprintf(lab, sizeof(lab), "%g", barLen);
        fg->AddText(ImVec2(barA.x, barA.y - 18), IM_COL32(60, 60, 60, 255), lab);

        // 底部横轴刻度（screenToWorld(0,0) 给的就是左边缘世界 x）
        const float visW = canvasW_ / cam_.scale;
        const float stepX = niceStep(visW / 6.f);
        float wx0 = 0.f, wy0 = 0.f;
        cam_.screenToWorld(0, 0, canvasW_, canvasH_, &wx0, &wy0);
        const float lxVis = wx0;
        for (float gx = std::ceil(lxVis / stepX) * stepX; gx < wx0 + visW; gx += stepX) {
            float sx = 0.f, sy = 0.f;
            cam_.worldToScreen(gx, wy0, canvasW_, canvasH_, &sx, &sy);
            if (sx < canvasX_ + 40 || sx > canvasX_ + canvasW_ - 10) continue;
            fg->AddLine({sx, canvasY_ + canvasH_ - 6}, {sx, canvasY_ + canvasH_},
                        IM_COL32(120, 120, 120, 200), 1.f);
            char t[32];
            std::snprintf(t, sizeof(t), "%g", gx);
            fg->AddText(ImVec2(sx - 14, canvasY_ + canvasH_ - 20),
                        IM_COL32(120, 120, 120, 255), t);
        }

        // 左侧纵轴刻度（Y 向上为正，与设计坐标一致）
        const float visH = canvasH_ / cam_.scale;
        const float stepY = niceStep(visH / 6.f);
        const float yTop = wy0;
        const float yBot = wy0 - visH;
        for (float gy = std::ceil(yBot / stepY) * stepY; gy < yTop; gy += stepY) {
            float sx = 0.f, sy = 0.f;
            cam_.worldToScreen(wx0, gy, canvasW_, canvasH_, &sx, &sy);
            if (sy < canvasY_ + 20 || sy > canvasY_ + canvasH_ - 30) continue;
            fg->AddLine({canvasX_, sy}, {canvasX_ + 6, sy},
                        IM_COL32(120, 120, 120, 200), 1.f);
            char t[32];
            std::snprintf(t, sizeof(t), "%g", gy);
            fg->AddText(ImVec2(canvasX_ + 8, sy - 6), IM_COL32(120, 120, 120, 255), t);
        }
    }

    // ---- T4/T24：选中项高亮（跟随 lo/hi/alpha 插值，最小屏幕 8px）。
    if (selected_ >= 0 && src_) {
        const NodeGeometry& g = src_->geometry();
        if (selected_ < static_cast<int>(g.w.size()) &&
            selected_ < static_cast<int>(g.kind.size()) &&
            canvas_.layerVisible[g.kind[selected_]]) {
            // 取当前显示插值位置：lo/hi 两帧按 alpha lerp。
            const PlaybackController::DisplayState ds = playback_.state();
            FrameView flo, fhi;
            src_->frame(std::max(0, ds.lo), flo);
            if (ds.hi > ds.lo) src_->frame(ds.hi, fhi);
            float cx = 0.f, cy = 0.f;
            if (flo.valid()) {
                cx = flo.x[selected_]; cy = flo.y[selected_];
                if (fhi.valid() && ds.hi > ds.lo) {
                    cx += ds.alpha * (fhi.x[selected_] - cx);
                    cy += ds.alpha * (fhi.y[selected_] - cy);
                }
            }
            const float hw = g.w[selected_] * 0.5f, hh = g.h[selected_] * 0.5f;
            float x0, y0, x1, y1;
            cam_.worldToScreen(cx - hw, cy - hh, canvasW_, canvasH_, &x0, &y0);
            cam_.worldToScreen(cx + hw, cy + hh, canvasW_, canvasH_, &x1, &y1);
            // 最小屏幕 8px：不足则画十字。
            const float wpx = x1 - x0, hpx = y1 - y0;
            if (wpx >= 8.f && hpx >= 8.f) {
                fg->AddRect({canvasX_ + x0, canvasY_ + y1},
                            {canvasX_ + x1, canvasY_ + y0},
                            IM_COL32(255, 200, 0, 255), 0.f, 0, 2.5f);
            } else {
                const float mx = canvasX_ + (x0 + x1) * 0.5f;
                const float my = canvasY_ + (y0 + y1) * 0.5f;
                fg->AddLine({mx - 5, my}, {mx + 5, my}, IM_COL32(255, 200, 0, 255), 2.f);
                fg->AddLine({mx, my - 5}, {mx, my + 5}, IM_COL32(255, 200, 0, 255), 2.f);
            }
            // 读数显示选中名
            if (selected_ < (int)g.name.size()) {
                char sb[96];
                std::snprintf(sb, sizeof(sb), "  选中: %s", g.name[selected_].c_str());
                selectedNameBuf_ = sb;
            }
            // T10：net 星形高亮——遍历选中单元所属 net 的全部引脚（基于 lo 帧 node 中心 + lo 偏移）。
            if (const NetList* nl = src_->nets()) {
                const std::vector<float>* off = nl->pinOffsetForFrame(std::max(0, ds.lo), bundlePath_);
                FrameView loF; bool haveLo = src_->frame(std::max(0, ds.lo), loF) && loF.valid();
                FrameView hiF; bool haveHi = ds.hi > ds.lo && src_->frame(ds.hi, hiF) && hiF.valid();
                // 收集选中节点所属的去重 net
                std::vector<int> netsOfSel;
                auto sp0 = nl->nodePinStart[selected_], sp1 = nl->nodePinStart[selected_ + 1];
                for (int pi = sp0; pi < sp1; ++pi) {
                    int pin = nl->nodePins[pi];
                    int net = nl->pinNet[pin];
                    if (net >= 0) netsOfSel.push_back(net);
                }
                std::sort(netsOfSel.begin(), netsOfSel.end());
                netsOfSel.erase(std::unique(netsOfSel.begin(), netsOfSel.end()), netsOfSel.end());
                const float selSx = canvasX_ + (x0 + x1) * 0.5f;
                const float selSy = canvasY_ + (y0 + y1) * 0.5f;
                for (int net : netsOfSel) {
                    if (net < 0) continue;
                    if (selectedNet_ >= 0 && net != selectedNet_) continue;
                    int deg = nl->netStart[net + 1] - nl->netStart[net];
                    if (deg > netDegThreshold_) continue;
                    int q0 = nl->netStart[net], q1 = nl->netStart[net + 1];
                    float bx0 = 1e18f, by0 = 1e18f, bx1 = -1e18f, by1 = -1e18f;
                    for (int q = q0; q < q1; ++q) {
                        int pin = nl->netPins[q];
                        int node = nl->pinNode[pin];
                        if (node < 0 || node >= src_->scene().nodeCount) continue;
                        float ox = 0, oy = 0;
                        if (off && (int)off->size() >= pin * 2 + 2) {
                            ox = (*off)[pin * 2]; oy = (*off)[pin * 2 + 1];
                        }
                        float px = cx, py = cy;
                        if (haveLo && node >= 0 && node < src_->scene().nodeCount) {
                            px = loF.x[node]; py = loF.y[node];
                            if (haveHi) {
                                px += ds.alpha * (hiF.x[node] - px);
                                py += ds.alpha * (hiF.y[node] - py);
                            }
                        }
                        bx0 = std::min(bx0, px + ox); by0 = std::min(by0, py + oy);
                        bx1 = std::max(bx1, px + ox); by1 = std::max(by1, py + oy);
                        float sx, sy;
                        cam_.worldToScreen(px + ox, py + oy, canvasW_, canvasH_, &sx, &sy);
                        // 其他 pin 所属单元描边
                        if (node != selected_)
                            fg->AddRect({canvasX_ + sx - 2.f, canvasY_ + sy - 2.f},
                                        {canvasX_ + sx + 2.f, canvasY_ + sy + 2.f},
                                        IM_COL32(120, 180, 255, 200));
                    }
                    // star 连到 net 引脚 bbox 中心
                    float bcx = (bx0 + bx1) * 0.5f, bcy = (by0 + by1) * 0.5f;
                    float sxc, syc;
                    cam_.worldToScreen(bcx, bcy, canvasW_, canvasH_, &sxc, &syc);
                    fg->AddLine({selSx, selSy}, {canvasX_ + sxc, canvasY_ + syc},
                                IM_COL32(120, 180, 255, 220), 1.5f);
                }
            }
        }
    }

    // ---- T21：位移向量场（参考位置→当前插值位置，末端落当前矩形，stride≤4000）
    if (compareOn_ && showVectors_ && src_ && geomUploaded_) {
        const PlaybackController::DisplayState ds = playback_.state();
        int loIdx = std::max(0, ds.lo), hiIdx = ds.hi;
        float useAlpha = ds.hi > ds.lo ? ds.alpha : 1.f;
        if (ds.hi == ds.lo && ds.lo > 0) { loIdx = ds.lo - 1; hiIdx = ds.lo; }
        FrameView flo, fhi;
        src_->frame(loIdx, flo);
        if (hiIdx > loIdx) src_->frame(hiIdx, fhi);
        int refFrame = 0;
        if (dispRefMode_ == 1) {
            for (int i = (int)metas_.size() - 1; i >= 0; --i)
                if (metas_[i].stage == "init") { refFrame = i; break; }
        } else if (dispRefMode_ == 0) {
            refFrame = loIdx;
        } else {
            refFrame = 0;
        }
        FrameView fref; bool haveRef = src_->frame(refFrame, fref) && fref.valid();
        if (flo.valid()) {
            const int n = src_->scene().nodeCount;
            const int stride = std::max(1, (n + 3999) / 4000);
            for (int i = 0; i < n; i += stride) {
                float cx = flo.x[i], cy = flo.y[i];
                if (fhi.valid()) {
                    cx += useAlpha * (fhi.x[i] - cx);
                    cy += useAlpha * (fhi.y[i] - cy);
                }
                float rx = cx, ry = cy;
                if (haveRef) { rx = fref.x[i]; ry = fref.y[i]; }
                if (std::hypot(cx - rx, cy - ry) < 1e-3f) continue;
                float sx0, sy0, sx1, sy1;
                cam_.worldToScreen(rx, ry, canvasW_, canvasH_, &sx0, &sy0);
                cam_.worldToScreen(cx, cy, canvasW_, canvasH_, &sx1, &sy1);
                const ImVec2 p0(canvasX_ + sx0, canvasY_ + sy0);
                const ImVec2 p1(canvasX_ + sx1, canvasY_ + sy1);
                fg->AddLine(p0, p1, IM_COL32(200, 40, 40, 200), 1.5f);
                const float vx = p1.x - p0.x, vy = p1.y - p0.y;
                const float vlen = std::hypot(vx, vy);
                if (vlen > 6.f) {
                    const float ux = vx / vlen, uy = vy / vlen;
                    const float px = -uy, py = ux;
                    const float hl = 6.f;
                    const ImVec2 h1(p1.x - hl * ux + hl * 0.5f * px,
                                    p1.y - hl * uy + hl * 0.5f * py);
                    const ImVec2 h2(p1.x - hl * ux - hl * 0.5f * px,
                                    p1.y - hl * uy - hl * 0.5f * py);
                    fg->AddLine(p1, h1, IM_COL32(200, 40, 40, 200), 1.5f);
                    fg->AddLine(p1, h2, IM_COL32(200, 40, 40, 200), 1.5f);
                }
            }
        }
    }
    fg->PopClipRect();
}

// T22：可收起栏目（自绘 -/+ 小按钮，状态存 StateStorage 随 ini 记住）。
static bool sectionHeader(const char* name) {
    // T22：栏目收起状态写入独立 ini（跨进程持久），运行态用静态 map 同步。
    static std::map<std::string, bool> s_open;
    static bool s_loaded = false;
    if (!s_loaded) {
        s_loaded = true;
        std::ifstream in("superplace-gui-sections.ini");
        std::string line;
        while (std::getline(in, line)) {
            // 行格式："栏目名" 0/1（名字带空格用双引号）
            if (line.empty() || line[0] != '"') continue;
            size_t q = line.find('"', 1);
            if (q == std::string::npos) continue;
            std::string k = line.substr(1, q - 1);
            int v = 0;
            try { v = std::stoi(line.substr(q + 1)); } catch (...) { continue; }
            s_open[k] = (v != 0);
        }
    }
    auto it = s_open.find(name);
    bool open = it == s_open.end() ? true : it->second;
    char btn[8]; std::snprintf(btn, sizeof(btn), "%c", open ? '-' : '+');
    if (ImGui::SmallButton(btn)) {
        open = !open; s_open[name] = open;
        std::ofstream out("superplace-gui-sections.ini", std::ios::trunc);
        for (auto& [kk, vv] : s_open) out << '"' << kk << "\" " << (vv ? 1 : 0) << "\n";
    }
    ImGui::SameLine();
    ImGui::SeparatorText(name);
    return open;
}

void App::drawSidebar() {
    ImGui::Begin("控制");
    // 每帧一次探测 nets/ 目录存在性（不触发 nets() 懒加载）。
    const bool hasNets = src_ && src_->hasNetsAvailable();

    ImGui::SeparatorText("数据源");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##path", pathBuf_, sizeof(pathBuf_));
    if (ImGui::Button("加载 bundle")) {
        std::string err;
        auto s = makeBundleSource(pathBuf_, &err);
        if (s) {
            bundlePath_ = pathBuf_;
            setSource(std::move(s));
        } else {
            status_ = "加载失败：" + err;
        }
    }
    if (ImGui::SmallButton("重置布局")) layoutBuilt_ = false;

    if (src_) {
        const SceneInfo& sc = src_->scene();

        // 主功能：回放/时间轴放在数据源之后、总览之前（使用者打开即见播放控件）。
        if (sectionHeader("总览")) {
        if (const SummaryTable* sum = src_->summary(); sum && !sum->empty()) {
            // T22：关键指标直接显示，其余进默认收起的「全部字段」。
            static const char* kKeys[] = {"hpwl", "overflow", "total_time_s", "finalHpwl",
                                          "finalOverflow", "mLG displacement"};
            auto isKey = [](const std::string& k) {
                for (const char* d : kKeys)
                    if (k.find(d) != std::string::npos) return true;
                return false;
            };
            for (const auto& [key, val] : sum->entries) {
                if (!isKey(key)) continue;
                const char* label = summaryDisplayName(key);
                ImGui::Text("%s", label ? label : key.c_str());
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.f);
                ImGui::Text("%s", formatSummaryValue(val).c_str());
            }
            if (ImGui::TreeNode("全部字段")) {
                for (const auto& [key, val] : sum->entries) {
                    const char* label = summaryDisplayName(key);
                    ImGui::Text("%s", label ? label : key.c_str());
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.f);
                    ImGui::Text("%s", formatSummaryValue(val).c_str());
                }
                ImGui::TreePop();
            }
            if (!sum->durationsMs.empty()) {
                ImGui::SeparatorText("阶段耗时 (ms)");
                for (const auto& [k, v] : sum->durationsMs)
                    ImGui::Text("%-16s %.3f", k.c_str(), v);
            }
        } else {
            ImGui::TextWrapped("该 bundle 不含总览（无 summary.json）。");
        }
        }

if (sectionHeader("时间轴 / 回放")) {
            const PlaybackController::DisplayState ds = playback_.state();
            double t01 = playback_.playheadGlobal01();
            const double tmin = 0.0, tmax = 1.0;
            scrubbingSlider_ = ImGui::SliderScalar(
                "##t", ImGuiDataType_Double, &t01, &tmin, &tmax, "%.2f");
            if (ImGui::IsItemActivated()) playback_.pause();
            if (scrubbingSlider_) playback_.scrubToGlobal01(t01);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetItemRectMin();
            const ImVec2 p1 = ImGui::GetItemRectMax();
            if (!metas_.empty()) {
                const double total = playback_.totalDuration();
                for (size_t i = 0; i < metas_.size(); ++i) {
                    const double fx = total > 1e-9 ? metas_[i].t / metas_.back().t :
                                      static_cast<double>(i) / metas_.size();
                    const float bx = p0.x + static_cast<float>(fx) * (p1.x - p0.x);
                    dl->AddRectFilled(ImVec2(bx, p1.y + 2),
                                      ImVec2(bx + 3, p1.y + 6),
                                      ImGui::ColorConvertFloat4ToU32(stageColor(metas_[i].stage)));
                }
            }
            const ImVec4 scv = stageColor(ds.stage);
            ImGui::ColorButton("##st", scv,
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                               ImVec2(12, 12));
            ImGui::SameLine();
            ImGui::Text("%s", ds.label.empty() ? "—" : ds.label.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("t=%.1f%s", ds.globalT,
                                ds.interpolating ? " (插值)" : "");

            if (ImGui::Button(playback_.playing() ? "暂停" : "播放"))
                playback_.toggle();
            ImGui::SameLine();
            if (ImGui::Button("|< keyframe")) playback_.prevKeyframe();
            ImGui::SameLine();
            if (ImGui::Button("keyframe >|")) playback_.nextKeyframe();
            ImGui::SameLine();
            if (ImGui::Button("适应窗口")) needFit_ = true;
            if (ImGui::Button("|< stage")) playback_.prevStage();
            ImGui::SameLine();
            if (ImGui::Button("stage >|")) playback_.nextStage();
            ImGui::SameLine();
            ImGui::Checkbox("循环", &loopOn_);
            playback_.setLoop(loopOn_);

            float spd = static_cast<float>(playback_.speed());
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat("速度(轮/秒)", &spd, 1.f, 300.f, "%.0f"))
                playback_.setSpeed(static_cast<double>(spd));

            if (playback_.playing() || scrubbingSlider_)
                ImGui::TextDisabled("统计：关键帧 f%d（播放/拖动中）", ds.statFrame);
            else
                ImGui::TextDisabled("统计：%s",
                                    statsFromInterp_ ? "插值位置（已停）" : "关键帧");
        }

        // ---- T1：总览（summary.json）。缺文件/解析失败显示提示，不崩不空。
        if (sectionHeader("图层"))
        for (int k = 0; k < static_cast<int>(Kind::Count); ++k) {
            ImGui::PushID(k);
            const bool wasVisible = canvas_.layerVisible[k];
            if (ImGui::Checkbox("##v", &canvas_.layerVisible[k])) {
                // R2-1：关闭某类时，若当前选中正属于该类，立即取消，不留黄框/检视残留。
                if (wasVisible && !canvas_.layerVisible[k] && selected_ >= 0 && src_) {
                    const NodeGeometry& g = src_->geometry();
                    if (selected_ < static_cast<int>(g.kind.size()) &&
                        static_cast<int>(g.kind[static_cast<size_t>(selected_)]) == k)
                        selected_ = -1;
                }
            }
            ImGui::SameLine();
            ImGui::ColorButton("##c", kKindColor[k],
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                               ImVec2(12, 12));
            ImGui::SameLine();
            ImGui::Text("%s  (%s)", kKindName[k], human(sc.countByKind[k]).c_str());
            ImGui::PopID();
        }

        if (sectionHeader("渲染 / 着色")) {
        // T7：着色模式。net 数依赖 T10 的 nets.bin，当前 bundle 没有，明确禁用。
        {
            const char* cur = colorMode_ == ColorMode::Density ? "按局部密度"
                            : colorMode_ == ColorMode::Displacement ? "按帧间位移"
                            : colorMode_ == ColorMode::NetCount ? "按 net 数"
                            : "按类别";
            if (ImGui::BeginCombo("着色", cur)) {
                auto item = [&](ColorMode m, const char* name, bool enabled) {
                    ImGui::BeginDisabled(!enabled);
                    if (ImGui::Selectable(name, colorMode_ == m && !compareOn_)) {
                        colorMode_ = m;
                        if (m == ColorMode::Displacement) {
                            compareOn_ = true;
                        } else {
                            compareOn_ = false;
                        }
                        updateContinuousValues();
                    }
                    ImGui::EndDisabled();
                };
                item(ColorMode::Kind, "按类别", true);
                item(ColorMode::Density, "按局部密度（面积重叠）", true);
                item(ColorMode::Displacement, "按帧间位移量", true);
                item(ColorMode::NetCount, hasNets ? "按 net 数" : "按 net 数（无网表）", hasNets);
                if (!hasNets) {
                    const std::string& e = src_->netsError();
                    ImGui::TextDisabled("%s", e.empty() ? "bundle 未声明 nets：网表功能禁用" : e.c_str());
                }
                ImGui::EndCombo();
            }
            if (hasNets)
                ImGui::SliderInt("net 高亮度数阈值", &netDegThreshold_, 10, 10000);
            if (compareOn_) {
                const char* refs[] = {"瞬时运动 |pos(hi)-pos(lo)|", "相对 init 结束", "相对输入（第0帧）"};
                int prevRef = dispRefMode_;
                ImGui::Combo("参考", &dispRefMode_, refs, IM_ARRAYSIZE(refs));
                if (prevRef != dispRefMode_) dispRangeDirty_ = true;
                ImGui::Checkbox("位移箭头", &showVectors_);
                ImGui::TextDisabled("色表单位：设计坐标单位（一次回放固定）");
            }
        }
        if (colorMode_ != ColorMode::Kind && !compareOn_) {
            // 连续色表图例 + 数值范围
            ImGui::Text("范围 %.3g ~ %.3g", lastValMin_, lastValMax_);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // 与 canvas shader 同一组 viridis 停点
            const float stops[5][3] = {{0.267f,0.005f,0.329f},{0.229f,0.322f,0.545f},
                                       {0.127f,0.566f,0.550f},{0.369f,0.789f,0.383f},
                                       {0.992f,0.906f,0.144f}};
            for (int i = 0; i < 24; ++i) {
                float t = i / 23.f;
                int s = std::min(4, (int)(t * 4.f)); float lt = t * 4.f - s;
                if (s >= 4) { s = 3; lt = 1.f; }
                float r = stops[s][0]+lt*(stops[s+1][0]-stops[s][0]);
                float g = stops[s][1]+lt*(stops[s+1][1]-stops[s][1]);
                float b = stops[s][2]+lt*(stops[s+1][2]-stops[s][2]);
                ImU32 col = IM_COL32((int)(r*255),(int)(g*255),(int)(b*255),255);
                dl->AddRectFilled({p.x + 160*t, p.y}, {p.x + 160*(i+1)/23.f, p.y+12}, col);
            }
            ImGui::Dummy({160, 12});
        } else if (compareOn_) {
            ImGui::Text("位移范围 %.3g ~ %.3g", lastValMin_, lastValMax_);
        }
        ImGui::Checkbox("热力图图层", &heatmapOverlay_);
        if (heatmapOverlay_)
            ImGui::SetNextItemWidth(120), ImGui::SliderFloat("透明度", &heatmapAlpha_, 0.05f, 1.f, "%.2f");
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("最小显示尺寸（像素）", &canvas_.minPixelSize, 0.5f, 4.f, "%.1f px");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("全图视角下单元不足 1 像素，按此尺寸放大显示，避免碎成噪点");

        }  // 渲染/着色

        if (sectionHeader("统计")) {
        ImGui::Text("设计        %s", sc.benchmark.c_str());
        ImGui::Text("节点        %s", human(sc.nodeCount).c_str());
        ImGui::Text("core        %.0f x %.0f", sc.core.width(), sc.core.height());
        ImGui::Text("chip        %.0f x %.0f", sc.chip.width(), sc.chip.height());
        ImGui::Text("帧数        %d", src_->frameCount());
        ImGui::Text("绘制        %.2f ms", drawMs_);
        ImGui::Text("位置上传    %.2f ms", canvas_.lastUploadMs());
        ImGui::Text("UI          %.1f fps", ImGui::GetIO().Framerate);
        if (lodActive_) ImGui::TextUnformatted("LOD 热力图模式（放大后恢复矩形）");
        }
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", status_.c_str());
    ImGui::End();
}

void App::drawMetricsPanel() {
    // T19：收敛曲线停靠进底部节点，不再浮动固定位置。
    ImGui::Begin("收敛曲线");
    const MetricsTable* m = src_ ? src_->metrics() : nullptr;
    if (!m || m->rowCount == 0) {
        ImGui::TextUnformatted("该 bundle 不含 metrics.csv");
        ImGui::End();
        return;
    }
    const int ch = m->columnIndex("hpwl");
    const int co = m->columnIndex("overflow");
    if (ch < 0) {
        ImGui::TextUnformatted("metrics.csv 缺少 hpwl 列");
        ImGui::End();
        return;
    }

    // T5：构建 metrics 行 ↔ 抽样帧映射。T15：只读元数据（frameInfo），
    // 绝不为取标签对每帧调 frame()——那会把全部位置读进来并挤出当前帧缓存。
    if (!mapBuilt_) {
        std::vector<FrameView> all(src_->frameCount());
        for (int i = 0; i < src_->frameCount(); ++i) {
            const FrameMeta fm = src_->frameInfo(i);
            all[static_cast<size_t>(i)].stage = fm.stage;
            all[static_cast<size_t>(i)].label = fm.label;
            all[static_cast<size_t>(i)].index = fm.index;
        }
        frameMap_.build(*m, all);
        mapBuilt_ = true;
    }

    // 横轴用【全局行序号】：metrics 的 iter 每阶段从 0 重数，直接当横轴会折回去。
    if (xs_.size() != static_cast<size_t>(m->rowCount)) {
        xs_.resize(static_cast<size_t>(m->rowCount));
        for (int i = 0; i < m->rowCount; ++i) xs_[static_cast<size_t>(i)] = static_cast<float>(i);
    }

    if (ImPlot::BeginPlot("##conv", ImVec2(-1, -1))) {
        ImPlot::SetupAxes("metrics 行（全局序号）", "HPWL");
        ImPlot::SetupAxis(ImAxis_Y2, "overflow", ImPlotAxisFlags_AuxDefault);
        ImPlot::SetupLegend(ImPlotLocation_NorthEast, ImPlotLegendFlags_Outside);

        // T5：按阶段拆成多条序列，不同颜色；x 仍是全局行序号。
        int segStart = 0;
        for (int r = 1; r <= m->rowCount; ++r) {
            if (r < m->rowCount && m->stage[static_cast<size_t>(r)] ==
                                       m->stage[static_cast<size_t>(r - 1)])
                continue;
            const std::string& st = m->stage[static_cast<size_t>(r - 1)];
            // R2-2：空阶段 = 未知，中性灰色，不冒充具名分段，也不参与帧映射。
            const bool unknown = st.empty();
            const ImVec4 col = unknown ? ImVec4(0.55f, 0.55f, 0.55f, 0.9f) : stageColor(st);
            const char* label = unknown ? "（未知阶段）" : st.c_str();
            ImPlot::PlotLine(label, &xs_[static_cast<size_t>(segStart)],
                             m->values[static_cast<size_t>(ch)].data() + segStart,
                             r - segStart,
                             {ImPlotProp_LineColor, col, ImPlotProp_LineWeight, 2.0f});
            segStart = r;
        }
        if (co >= 0) {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::PlotLine("overflow", xs_.data(),
                             m->values[static_cast<size_t>(co)].data(), m->rowCount);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }

        // T5/T14：游标按当前显示关键帧对 (lo,hi,alpha) 连续映射到 metrics 行——
        // 不再是 statFrame 阶梯；插值时在两合法抽样行之间线性移动，质量诚实标注。
        const PlaybackController::DisplayState ds = playback_.state();
        MapQuality fq = MapQuality::Unavailable;
        double cursorRow = -1.0;
        int rowLo = frameMap_.frameToRow(ds.lo, &fq);
        if (ds.interpolating && rowLo >= 0) {
            MapQuality fqHi = MapQuality::Unavailable;
            const int rowHi = frameMap_.frameToRow(ds.hi, &fqHi);
            if (rowHi >= 0) {
                cursorRow = rowLo + ds.alpha * static_cast<double>(rowHi - rowLo);
                fq = MapQuality::Approx;   // 两抽样行之间插值，非精确样本
            }
        }
        if (cursorRow < 0) {
            const int r = frameMap_.frameToRow(ds.statFrame, &fq);
            if (r >= 0) cursorRow = r;
        }
        if (cursorRow >= 0) {
            double x = cursorRow;
            // 精确=深黑，最近邻=蓝，行间插值=橙，避免把猜测画成精确游标。
            ImVec4 cue = ImVec4(0.2f, 0.2f, 0.2f, 0.8f);
            if (fq == MapQuality::Nearest) cue = ImVec4(0.2f, 0.35f, 0.9f, 0.9f);
            else if (fq == MapQuality::Approx) cue = ImVec4(0.95f, 0.6f, 0.15f, 0.9f);
            ImPlot::DragLineX(1, &x, cue, 1.5f, ImPlotDragToolFlags_NoInputs);
        }

        // T5：点曲线跳到对应帧（诚实映射；Unavailable 不跳转）。
        if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const double mx = ImPlot::GetPlotMousePos().x;
            const int row = std::clamp(static_cast<int>(std::lround(mx)), 0, m->rowCount - 1);
            MapQuality rq = MapQuality::Unavailable;
            const int f = frameMap_.rowToFrame(row, &rq);
            if (f >= 0) gotoFrame(f);
        }

        // 映射质量说明（诚实标注，不混淆精确/近似/无映射）。
        ImGui::TextDisabled("映射：%s", mapQualityLabel(fq));
        // R2-2：存在空阶段时明确提示——未知阶段不参与帧映射，不是被继承的具名段。
        bool hasBlank = false;
        for (int r = 0; r < m->rowCount; ++r)
            if (m->stage[static_cast<size_t>(r)].empty()) { hasBlank = true; break; }
        if (hasBlank)
            ImGui::TextDisabled("注：空白/缺失阶段记为未知，不继承上一阶段，也不参与帧映射。");
        ImPlot::EndPlot();
    }
    ImGui::End();
}

void App::drawInspectPanel() {
    ImGui::Begin("检视");
    if (!src_) {
        ImGui::TextUnformatted("未加载数据");
        ImGui::End();
        return;
    }
    if (selected_ < 0) {
        ImGui::TextWrapped("单击画布节点进行点选检视。点空白处取消。");
        ImGui::End();
        return;
    }
    const NodeGeometry& g = src_->geometry();
    if (selected_ >= static_cast<int>(g.w.size())) {
        selected_ = -1;
        ImGui::End();
        return;
    }
    // R2-1：选中项所在层已关闭时，清空过时检视，不展示隐藏节点。
    if (selected_ < static_cast<int>(g.kind.size()) &&
        !canvas_.layerVisible[g.kind[static_cast<size_t>(selected_)]]) {
        ImGui::TextWrapped("该节点所在图层已关闭，已取消选中。");
        selected_ = -1;
        ImGui::End();
        return;
    }

    const bool hasName = !g.name.empty() && selected_ < static_cast<int>(g.name.size());
    ImGui::Text("下标      %d", selected_);
    ImGui::Text("名字      %s", hasName ? g.name[static_cast<size_t>(selected_)].c_str()
                                     : "—（bundle 未带 names.txt，待 T9）");
    ImGui::Text("类别      %s", kKindName[g.kind[static_cast<size_t>(selected_)]]);
    ImGui::Text("宽 × 高   %.3f × %.3f", g.w[selected_], g.h[selected_]);
    const float* sx = nullptr; const float* sy = nullptr;
    if (statsSource(&sx, &sy) && selected_ < src_->scene().nodeCount) {
        ImGui::Text("中心      (%.3f, %.3f)", sx[selected_], sy[selected_]);
        // 所属 bin（与 T6 同网格，仅展示用）
        const Rect& chip = src_->scene().chip;
        const int c = static_cast<int>((sx[selected_] - chip.lx) /
                                       (chip.width() / 128.f));
        const int r = static_cast<int>((sy[selected_] - chip.ly) /
                                       (chip.height() / 128.f));
        ImGui::Text("bin       (%d, %d)", c, r);
    }
    // T10：网表信息（引脚数/所连 net）。
    if (const NetList* nl = src_->nets()) {
        int p0 = nl->nodePinStart[selected_], p1 = nl->nodePinStart[selected_ + 1];
        ImGui::SeparatorText("网表");
        ImGui::Text("引脚数    %d", p1 - p0);
        if (selectedNet_ >= 0 && ImGui::SmallButton("显示全部 net")) selectedNet_ = -1;
        for (int pi = p0; pi < p1; ++pi) {
            int pin = nl->nodePins[pi];
            int net = nl->pinNet[pin];
            if (net < 0) continue;
            if (selectedNet_ >= 0 && net != selectedNet_) continue;
            int deg = nl->netStart[net + 1] - nl->netStart[net];
            const char* nm = (nl->hasNames && net < (int)nl->netNames.size())
                                 ? nl->netNames[net].c_str() : "—";
            float w = nl->hasWeights && net < (int)nl->netWeight.size() ? nl->netWeight[net] : 1.f;
            char lbl[160]; std::snprintf(lbl, sizeof(lbl), "%s  度=%d  权重=%.2f", nm, deg, w);
            if (ImGui::Selectable(lbl)) selectedNet_ = net;
        }
    } else {
        const std::string& e = src_->netsError();
        ImGui::TextDisabled("%s", e.empty() ? "无网表（bundle 未声明 nets）" : e.c_str());
    }
    ImGui::End();
}

}  // namespace viz
