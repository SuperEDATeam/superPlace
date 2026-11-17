// Tests App command dispatch/state transitions without opening a GUI window.
#include "test_util.h"
#include "app.h"
#include "viz/child_process.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <signal.h>
#include <unistd.h>

namespace viz {
class MouseFixtureSource final : public IPlacementSource {
    SceneInfo scene_;
    NodeGeometry geom_;
    std::shared_ptr<const std::vector<float>> data_;
    std::string name_ = "mouse-fixture";
public:
    MouseFixtureSource() {
        scene_.benchmark = name_; scene_.nodeCount = 1;
        scene_.chip = scene_.core = Rect{0, 0, 100, 100};
        geom_.w = {10}; geom_.h = {10}; geom_.flags = {0};
        geom_.kind = {static_cast<uint8_t>(Kind::Cell)}; geom_.name = {"single-cell"};
        data_ = std::make_shared<const std::vector<float>>(std::vector<float>{50, 50});
    }
    const SceneInfo& scene() const override { return scene_; }
    const NodeGeometry& geometry() const override { return geom_; }
    const MetricsTable* metrics() const override { return nullptr; }
    int frameCount() const override { return 1; }
    bool frame(int index, FrameView& out) override {
        if(index != 0) return false;
        out.data = data_; out.x = data_->data(); out.y = out.x + 1;
        out.count = 1; out.index = 0; return true;
    }
    FrameMeta frameInfo(int index) const override { FrameMeta f; f.index=index; return f; }
    const std::string& sourceName() const override { return name_; }
};
struct AppCommandTestAccess {
    static void checkDocking() {
        // ImGui frame/layout integration only: no native window, GL or screenshot.
        ImGui::CreateContext(); ImPlot::CreateContext();
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = nullptr; io.DisplaySize = ImVec2(1920, 1080); io.DeltaTime = 1.f/60.f;
        io.Fonts->AddFontDefault(); io.Fonts->Build();
        {
            App a(AppOptions{});
            auto frame = [&] { ImGui::NewFrame(); a.drawUi(); ImGui::Render(); };
            frame();
            CHECK_TRUE(a.layoutBuilt_ && a.canvasW_ > 500.f && a.canvasH_ > 300.f);
            auto* control = ImGui::FindWindowByName("控制");
            auto* curves = ImGui::FindWindowByName("收敛曲线");
            auto* terminal = ImGui::FindWindowByName("终端");
            CHECK_TRUE(control && curves && terminal);
            CHECK_TRUE(control->DockId != 0 && curves->DockId == terminal->DockId);
            CHECK_TRUE(ImGui::FindWindowByName("检视") == nullptr);
            const float x = a.canvasX_, y = a.canvasY_, w = a.canvasW_, h = a.canvasH_;
            // Reset in the empty state must deterministically rebuild the same layout.
            a.layoutBuilt_ = false; a.focusTab_ = "收敛曲线";
            frame();
            CHECK_NEAR(a.canvasX_, x, 0.1); CHECK_NEAR(a.canvasY_, y, 0.1);
            CHECK_NEAR(a.canvasW_, w, 0.1); CHECK_NEAR(a.canvasH_, h, 0.1);
            a.openBundle(SYNTHETIC_BUNDLE); frame();
            CHECK_NEAR(a.canvasX_, x, 0.1); CHECK_NEAR(a.canvasY_, y, 0.1);
            CHECK_NEAR(a.canvasW_, w, 0.1); CHECK_NEAR(a.canvasH_, h, 0.1);
            auto* inspect = ImGui::FindWindowByName("检视");
            CHECK_TRUE(inspect && inspect->DockId != 0 && inspect->DockId != control->DockId);
            // Inject only ImGui IO: tests App's actual press/release path, not desktop input.
            a.setSource(std::make_unique<MouseFixtureSource>()); frame(); frame();
            float px=0, py=0;
            a.cam_.worldToScreen(50, 50, a.canvasW_, a.canvasH_, &px, &py);
            io.AddMousePosEvent(a.canvasX_+px, a.canvasY_+py); frame();
            io.AddMouseButtonEvent(0, true); frame();
            io.AddMouseButtonEvent(0, false); frame();
            CHECK_EQ(a.selected_, 0);
            CHECK_TRUE(!a.selectedNameBuf_.empty());
            a.cam_.worldToScreen(85, 85, a.canvasW_, a.canvasH_, &px, &py);
            io.AddMousePosEvent(a.canvasX_+px, a.canvasY_+py); frame();
            io.AddMouseButtonEvent(0, true); frame();
            io.AddMouseButtonEvent(0, false); frame();
            CHECK_EQ(a.selected_, -1);
            CHECK_TRUE(a.selectedNameBuf_.empty());
        }
        ImPlot::DestroyContext(); ImGui::DestroyContext();
    }
    static void run() {
        checkDocking();
        App a(AppOptions{});
        a.handleTerminalCommand(" help ");
        CHECK_EQ(a.terminalLines_.size(), 7u);
        a.handleTerminalCommand("clear");
        CHECK_TRUE(a.terminalLines_.empty());
        a.handleTerminalCommand("gdsinfo");
        CHECK_STREQ(a.terminalLines_.back(), "先在『数据源』中选择设计");
        a.handleTerminalCommand("legalize");
        CHECK_STREQ(a.terminalLines_.back(), "只有 newblue1 能合法化");
        a.placeRunning_ = true;
        a.handleTerminalCommand("legalize");
        CHECK_STREQ(a.terminalLines_.back(), "布局运行中，先 stop 或等它完成");
        a.placeRunning_ = false;
        a.openBundle(SYNTHETIC_BUNDLE);
        CHECK_TRUE(a.src_ != nullptr);
        a.handleTerminalCommand("legalize");
        CHECK_STREQ(a.terminalLines_.back(), "只有 newblue1 能合法化");
        a.gdsExportMsg_ = "old export"; a.lastGdsPath_ = "old.gds";
        a.openBundle(SYNTHETIC_BUNDLE);
        CHECK_TRUE(a.gdsExportMsg_.empty() && a.lastGdsPath_.empty());

        namespace fs = std::filesystem;
        const fs::path root(App::repoRoot());
        const fs::path output = root / "results/demo-lg/newblue1";
        fs::create_directories(output);
        { std::ofstream out(output/"summary.json"); out << R"({"lg_cells":3,"lg_failed":0,"lg_hpwl_before":100,"lg_hpwl":110,"lg_disp_mean":2,"lg_disp_max":7})"; }
        const auto path = a.bundlePath_;
        const auto state = a.playback_.state();
        const auto focus = a.focusTab_;
        IPlacementSource* source = a.src_.get();
        a.terminalLines_.clear();
        CHECK_EQ(spawnLogged({"/bin/true"}, (output/"fixture.log").string(), a.placePid_), 0);
        a.processTask_ = App::ProcessTask::Check;
        a.placeRunning_ = true;
        a.placeStartSec_ = 0;
        a.placeLastPoll_ = 0;
        for (int i=1; i<200 && a.placeRunning_; ++i) {
            a.pollPlace(i * 0.5);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK_TRUE(!a.placeRunning_);
        CHECK_EQ(a.terminalLines_.size(), 3u);
        CHECK_STREQ(a.terminalLines_.back(), "检查器：PASS");
        CHECK_TRUE(a.src_.get() == source && a.bundlePath_ == path);
        CHECK_NEAR(a.playback_.state().globalT, state.globalT, 1e-12);
        CHECK_STREQ(a.focusTab_, focus);
        a.terminalLines_.clear();
        CHECK_EQ(spawnLogged({"/bin/false"}, (output/"fixture.log").string(), a.placePid_), 0);
        a.processTask_ = App::ProcessTask::Legalize;
        a.placeRunning_ = true; a.placeLastPoll_ = 0;
        for (int i=1; i<200 && a.placeRunning_; ++i) {
            a.pollPlace(i * 0.5);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK_EQ(a.terminalLines_.size(), 1u);
        CHECK_TRUE(a.terminalLines_.back().find("合法化失败（退出码 1）") != std::string::npos);
        CHECK_TRUE(a.src_.get() == source && a.bundlePath_ == path);

        auto pollDone = [&] {
            a.placeLastPoll_ = 0;
            for (int i=1; i<200 && a.placeRunning_; ++i) {
                a.pollPlace(i * 0.5);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            CHECK_TRUE(!a.placeRunning_);
        };
        a.terminalLines_.clear();
        CHECK_EQ(spawnLogged({"/bin/false"}, (output/"fixture.log").string(), a.placePid_), 0);
        a.processTask_ = App::ProcessTask::Check; a.placeRunning_ = true;
        pollDone();
        CHECK_EQ(a.terminalLines_.size(), 3u);
        CHECK_STREQ(a.terminalLines_.back(), "检查器：FAIL，详见 results/demo-lg/newblue1/check.log");

        a.terminalLines_.clear();
        fs::remove(output/"summary.json");
        CHECK_EQ(spawnLogged({"/bin/true"}, (output/"fixture.log").string(), a.placePid_), 0);
        a.processTask_ = App::ProcessTask::Legalize; a.placeRunning_ = true;
        pollDone();
        CHECK_EQ(a.terminalLines_.size(), 1u);
        CHECK_TRUE(a.terminalLines_.back().find("合法化失败") != std::string::npos);

        a.terminalLines_.clear();
        CHECK_EQ(spawnLogged({"/bin/sleep", "5"}, (output/"fixture.log").string(), a.placePid_), 0);
        a.processTask_ = App::ProcessTask::Legalize; a.placeRunning_ = true;
        a.placeStopping_ = true; a.placeStopSec_ = 0;
        CHECK_EQ(kill(a.placePid_, SIGTERM), 0);
        pollDone();
        CHECK_TRUE(a.terminalLines_.empty());
        CHECK_TRUE(a.src_.get() == source && a.bundlePath_ == path);
        CHECK_NEAR(a.playback_.state().globalT, state.globalT, 1e-12);

        // Legalize success really spawns a second process (checker), without loading LG.
        { std::ofstream out(output/"summary.json"); out << R"({"lg_cells":3,"lg_failed":0,"lg_hpwl_before":100,"lg_hpwl":110,"lg_disp_mean":2,"lg_disp_max":7})"; }
        fs::create_directories(root/"build");
        const auto checker = root/"build/superplace-check";
        if (!fs::exists(checker)) fs::create_symlink("/bin/true", checker);
        a.terminalLines_.clear();
        CHECK_EQ(spawnLogged({"/bin/true"}, (output/"fixture.log").string(), a.placePid_), 0);
        a.processTask_ = App::ProcessTask::Legalize; a.placeRunning_ = true;
        pollDone();
        CHECK_TRUE(a.processTask_ == App::ProcessTask::Check);
        CHECK_EQ(a.terminalLines_.size(), 3u);
        CHECK_STREQ(a.terminalLines_.back(), "检查器：PASS");
        CHECK_TRUE(fs::exists(output/"check.log"));
        CHECK_TRUE(a.src_.get() == source && a.bundlePath_ == path);
        CHECK_NEAR(a.playback_.state().globalT, state.globalT, 1e-12);
        CHECK_STREQ(a.focusTab_, focus);

        a.terminalLines_.clear();
        a.bundlePath_ = (root/"results/play/ispd2006/newblue1/viz").string();
        a.handleTerminalCommand("legalize");
        CHECK_TRUE(a.terminalLines_.back().find("未找到 GP 结果 ") == 0);
        a.bundlePath_ = path;

        // Exercise actual export -> on-disk readback, then corrupt the file.
        FrameView fv;
        CHECK_TRUE(a.src_->frame(20, fv));
        a.bundlePath_ = (root/"results/fixture/viz").string();
        a.lastPx_ = fv.x; a.lastPy_ = fv.y; a.lastN_ = fv.count; a.positionsValid_ = true;
        a.terminalLines_.clear();
        a.handleTerminalCommand("gdsinfo");
        CHECK_TRUE(!a.lastGdsPath_.empty());
        CHECK_TRUE(fs::exists(a.lastGdsPath_));
        CHECK_EQ(a.terminalLines_.size(), 7u);
        CHECK_TRUE(a.terminalLines_.back().find("记录结构完整") != std::string::npos);
        { std::ofstream out(a.lastGdsPath_, std::ios::binary|std::ios::trunc); out << "BAD"; }
        a.handleTerminalCommand("gdsinfo");
        CHECK_TRUE(a.terminalLines_.back().find("GDSII 解析失败：字节偏移") != std::string::npos);
    }
};
}
int main() {
    viz::AppCommandTestAccess::run();
    std::printf("app_command_tests: %d checks, %d failures\n",viztest::checks(),viztest::failures());
    return viztest::failures() ? 1 : 0;
}
