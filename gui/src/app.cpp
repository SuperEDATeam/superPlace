#include "app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>   // DockBuilder：搭默认停靠布局
#include <implot.h>

namespace viz {
namespace {

const char* kKindName[] = {"Filler", "固定终端", "标准单元", "宏单元"};
const ImVec4 kKindColor[] = {
    {0.31f, 0.75f, 0.35f, 1.f}, {0.24f, 0.43f, 0.86f, 1.f},
    {0.86f, 0.24f, 0.24f, 1.f}, {0.96f, 0.59f, 0.16f, 1.f},
};

void glfwErrorCb(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] %d: %s\n", code, desc);
}

std::string human(long long v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

}  // namespace

App::App(AppOptions opt) : opt_(std::move(opt)) {}

App::~App() {
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
    src_ = std::move(src);
    frameIndex_ = 0;
    needFit_ = true;
    geomUploaded_ = false;
    cur_ = {};
    if (src_) status_ = src_->scene().benchmark + " 已加载";
    // 这里【不能】直接传几何给 canvas：调用方可能在 run() 之前就注入数据源
    // （集成版 main_gui.cpp 就是这样），那时 GL 上下文还不存在、函数指针全是空的。
    // 真正的上传推迟到 uploadGeometry()，由主循环在上下文就绪后触发。
    uploadGeometry();
}

void App::uploadGeometry() {
    if (!glReady_ || !src_ || geomUploaded_) return;
    canvas_.setGeometry(src_->geometry());
    geomUploaded_ = true;
    gotoFrame(frameIndex_);
}

bool App::initWindow() {
    glfwSetErrorCallback(glfwErrorCb);
    if (!glfwInit()) return false;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    window_ = glfwCreateWindow(opt_.width, opt_.height, "superPlace 布局查看器", nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr,
                     "创建窗口失败。WSL2 下请确认 WSLg 可用，并设 GALLIUM_DRIVER=d3d12\n");
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
    // 中文字形：系统有 Noto CJK 就加载，没有就退回默认字体（界面仍可用，只是中文显示为方块）
    static const char* kFonts[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/mnt/c/Windows/Fonts/msyh.ttc",
    };
    for (const char* f : kFonts) {
        if (FILE* fp = std::fopen(f, "rb")) {
            std::fclose(fp);
            io.Fonts->AddFontFromFileTTF(f, 17.0f, nullptr,
                                         io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            break;
        }
    }

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

void App::gotoFrame(int index) {
    if (!src_ || !geomUploaded_) return;
    const int n = src_->frameCount();
    if (n <= 0) return;
    frameIndex_ = std::clamp(index, 0, n - 1);
    FrameView fv;
    if (src_->frame(frameIndex_, fv)) {
        cur_ = fv;
        canvas_.setPositions(fv.x, fv.y, fv.count);
    }
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
    if (src_ && opt_.startFrame != 0)
        gotoFrame(opt_.startFrame < 0 ? src_->frameCount() - 1 : opt_.startFrame);

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        uploadGeometry();

        if (src_ && src_->isLive() && src_->poll() && playing_)
            gotoFrame(src_->frameCount() - 1);   // 实时模式自动追最新帧

        if (playing_ && src_ && src_->frameCount() > 1) {
            const double now = glfwGetTime();
            if (now - lastAdvance_ >= 1.0 / playFps_) {
                lastAdvance_ = now;
                gotoFrame(frameIndex_ + 1 >= src_->frameCount() ? 0 : frameIndex_ + 1);
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
            const auto t0 = std::chrono::steady_clock::now();
            canvas_.draw(cam_, vw, vh);
            drawMs_ = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
            glViewport(0, 0, fbW, fbH);
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (opt_.autoShotAfterSec > 0.0 && glfwGetTime() >= opt_.autoShotAfterSec) {
            if (!opt_.screenshot.empty()) saveScreenshot(opt_.screenshot, fbW, fbH);
            std::fprintf(stderr, "[viz] 自动截图后退出，当前帧数 %d\n",
                         src_ ? src_->frameCount() : 0);
            glfwSetWindowShouldClose(window_, 1);
        }
        glfwSwapBuffers(window_);
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

    std::vector<double> times;
    times.reserve(static_cast<size_t>(opt_.benchFrames));
    for (int i = 0; i < opt_.benchFrames; ++i) {
        glfwPollEvents();
        // 每帧换一帧数据，把"位置上传"的开销也算进来——实际动画播放就是这个负载
        if (src_->frameCount() > 1) gotoFrame((i + 1) % src_->frameCount());

        const auto t0 = std::chrono::steady_clock::now();
        glViewport(0, 0, fbW, fbH);
        glClearColor(1.f, 1.f, 1.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        canvas_.draw(cam_, fbW, fbH);
        glFinish();                         // 不 finish 测到的只是提交耗时
        times.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count());
        glfwSwapBuffers(window_);
    }
    if (!opt_.screenshot.empty()) saveScreenshot(opt_.screenshot, fbW, fbH);

    std::sort(times.begin(), times.end());
    const auto pick = [&](double q) { return times[static_cast<size_t>(q * (times.size() - 1))]; };
    std::printf("节点 %d  视口 %dx%d  帧数 %zu\n", src_->scene().nodeCount, fbW, fbH, times.size());
    std::printf("绘制+上传: 中位 %.2f ms  p95 %.2f ms  最差 %.2f ms  => %.0f fps(中位)\n",
                pick(0.5), pick(0.95), times.back(), 1000.0 / pick(0.5));
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

    // 首次运行搭一套默认布局。不这么做的话三个面板会各自浮着，而且"布局画布"
    // 会塌缩——它的尺寸取自窗口可用区，内容又按该尺寸建 InvisibleButton，
    // 自适应窗口下这是个会一路缩到最小的反馈环（实测缩成 16x1）。
    // 停靠到中央节点后尺寸由 dockspace 决定，环就断了。
    if (!layoutBuilt_ && !ImGui::GetIO().IniFilename) layoutBuilt_ = true;
    if (!layoutBuilt_) {
        layoutBuilt_ = true;
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace |
                                              ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);
        ImGuiID right = 0, bottom = 0, center = dockId;
        // 中央节点【故意留空】，GL 画布直接画进它的矩形，见 drawCanvasPanel 的说明
        right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
        bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
        ImGui::DockBuilderDockWindow("控制", right);
        ImGui::DockBuilderDockWindow("收敛曲线", bottom);
        ImGui::DockBuilderFinish(dockId);
    }
    drawCanvasPanel(dockId);
    drawSidebar();
    drawMetricsPanel();
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

    canvasX_ = central->Pos.x;
    canvasY_ = central->Pos.y;
    canvasW_ = std::max(central->Size.x, 1.f);
    canvasH_ = std::max(central->Size.y, 1.f);

    if (src_ && needFit_ && canvasW_ > 32.f && canvasH_ > 32.f) {
        cam_.fit(src_->scene().chip, canvasW_, canvasH_);
        needFit_ = false;
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 lo(canvasX_, canvasY_);
    const ImVec2 hi(canvasX_ + canvasW_, canvasY_ + canvasH_);
    // 只有鼠标落在中央节点上、且没有任何 ImGui 控件要抢输入时，才算在操作画布
    const bool overCanvas = ImGui::IsMouseHoveringRect(lo, hi) && !io.WantCaptureMouse;

    if (overCanvas && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        cam_.cx -= io.MouseDelta.x / cam_.scale;
        cam_.cy += io.MouseDelta.y / cam_.scale;   // 屏幕 y 向下，设计 y 向上
    }
    if (overCanvas && io.MouseWheel != 0.f) {
        cam_.zoomAt(io.MousePos.x - canvasX_, io.MousePos.y - canvasY_, canvasW_, canvasH_,
                    std::exp(io.MouseWheel * 0.18f));
    }

    if (src_) {
        char buf[128];
        if (overCanvas) {
            float wx = 0, wy = 0;
            cam_.screenToWorld(io.MousePos.x - canvasX_, io.MousePos.y - canvasY_, canvasW_,
                               canvasH_, &wx, &wy);
            std::snprintf(buf, sizeof(buf), "(%.0f, %.0f)   %.4f px/unit   滚轮缩放 · 拖拽平移",
                          wx, wy, cam_.scale);
        } else {
            std::snprintf(buf, sizeof(buf), "%.4f px/unit   滚轮缩放 · 拖拽平移", cam_.scale);
        }
        // 叠加文字走前景绘制列表：中央节点里没有窗口，没地方放普通控件
        ImGui::GetForegroundDrawList()->AddText(ImVec2(canvasX_ + 10, canvasY_ + canvasH_ - 22),
                                                IM_COL32(90, 90, 90, 255), buf);
    }
}

void App::drawSidebar() {
    ImGui::Begin("控制");

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

    if (src_) {
        const SceneInfo& sc = src_->scene();
        ImGui::SeparatorText("图层");
        for (int k = 0; k < static_cast<int>(Kind::Count); ++k) {
            ImGui::PushID(k);
            ImGui::Checkbox("##v", &canvas_.layerVisible[k]);
            ImGui::SameLine();
            ImGui::ColorButton("##c", kKindColor[k],
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                               ImVec2(12, 12));
            ImGui::SameLine();
            ImGui::Text("%s  (%s)", kKindName[k], human(sc.countByKind[k]).c_str());
            ImGui::PopID();
        }

        ImGui::SeparatorText("时间轴");
        int idx = frameIndex_;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##frame", &idx, 0, std::max(0, src_->frameCount() - 1), "%d"))
            gotoFrame(idx);
        ImGui::TextUnformatted(cur_.label.empty() ? "—" : cur_.label.c_str());
        if (ImGui::Button(playing_ ? "暂停" : "播放")) playing_ = !playing_;
        ImGui::SameLine();
        if (ImGui::Button("适应窗口")) needFit_ = true;
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("帧率", &playFps_, 1.f, 60.f, "%.0f fps");

        ImGui::SeparatorText("渲染");
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("最小像素", &canvas_.minPixelSize, 0.5f, 4.f, "%.1f px");

        ImGui::SeparatorText("统计");
        ImGui::Text("设计        %s", sc.benchmark.c_str());
        ImGui::Text("节点        %s", human(sc.nodeCount).c_str());
        ImGui::Text("core        %.0f x %.0f", sc.core.width(), sc.core.height());
        ImGui::Text("帧数        %d%s", src_->frameCount(), src_->isLive() ? "（实时）" : "");
        ImGui::Text("绘制        %.2f ms", drawMs_);
        ImGui::Text("位置上传    %.2f ms", canvas_.lastUploadMs());
        ImGui::Text("UI          %.1f fps", ImGui::GetIO().Framerate);
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", status_.c_str());
    ImGui::End();
}

void App::drawMetricsPanel() {
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

    // 横轴用【行序号】而不是 metrics.csv 里的 iter 列：iter 在每个阶段都从 0 重数，
    // 直接拿它当横轴会让曲线在阶段交界处折回去，看着像发散。
    if (xs_.size() != static_cast<size_t>(m->rowCount)) {
        xs_.resize(static_cast<size_t>(m->rowCount));
        for (int i = 0; i < m->rowCount; ++i) xs_[static_cast<size_t>(i)] = static_cast<float>(i);
    }

    if (ImPlot::BeginPlot("##conv", ImVec2(-1, -1))) {
        ImPlot::SetupAxes("sample", "HPWL");
        ImPlot::SetupAxis(ImAxis_Y2, "overflow τ", ImPlotAxisFlags_AuxDefault);
        ImPlot::PlotLine("HPWL", xs_.data(), m->values[static_cast<size_t>(ch)].data(),
                         m->rowCount);
        if (co >= 0) {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::PlotLine("τ", xs_.data(), m->values[static_cast<size_t>(co)].data(),
                             m->rowCount);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }
        // 阶段分界线，顺便标出 mGP / mLG / cGP 各自的区间。
        // 相邻阶段很近时（FILLERONLY 只有 20 轮）标签会糊成一团，所以做间距过滤。
        const double minGap = (ImPlot::GetPlotLimits().X.Max - ImPlot::GetPlotLimits().X.Min) * 0.06;
        double lastLabelX = -1e30;
        for (int i = 1; i < m->rowCount; ++i) {
            if (m->stage[static_cast<size_t>(i)] == m->stage[static_cast<size_t>(i - 1)]) continue;
            double x = i;
            ImPlot::DragLineX(i, &x, ImVec4(0.6f, 0.6f, 0.6f, 0.5f), 1.0f,
                              ImPlotDragToolFlags_NoInputs);
            if (x - lastLabelX < minGap) continue;   // 太挤就只画线不画字
            lastLabelX = x;
            ImPlot::Annotation(x, ImPlot::GetPlotLimits().Y.Max, ImVec4(0.4f, 0.4f, 0.4f, 1.f),
                               ImVec2(4, 10), false, "%s",
                               m->stage[static_cast<size_t>(i)].c_str());
        }
        ImPlot::EndPlot();
    }
    ImGui::End();
}

}  // namespace viz
