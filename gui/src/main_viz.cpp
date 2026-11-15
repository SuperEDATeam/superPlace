// superplace-viz —— SuperPlace GUI 布局查看器。
//
// 这个可执行文件**不链接 libsuperplace**，也不需要 FFTW / Eigen / libpng / OpenMP。
// 它唯一认识的东西是 viz bundle 目录格式。
//
//     ./superplace-viz [bundle 目录]
//
// 不给参数时进入空白状态（在右侧「数据源」下拉框里选设计）；
// 直接给 bundle 目录则跳过空白状态立即打开。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app.h"

int main(int argc, char** argv) {
    viz::AppOptions opt;
    // 不带参数：空白启动（T31）。bundle 直开只走位置参数。
    opt.initialBundle.clear();

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::printf(
                "SuperPlace GUI —— superplace-viz 布局查看器\n\n"
                "用法: superplace-viz [bundle 目录] [--no-vsync]\n"
                "          [--width W] [--height H] [--ui-scale S]\n"
                "          [--frame N] [--bench N] [--bench-ui N] [--bench-csv FILE]\n"
                "          [--zoom F] [--center X Y] [--paused] [--no-cull]\n"
                "          [--screenshot out.ppm] [--shot-after 秒]\n\n"
                "bundle 由 scripts/make_viz.py 生成，格式见\n"
                "work_report/06-前端开发方案与接口契约.md\n");
            return 0;
        }
        if (a == "--no-vsync") {
            opt.vsync = false;
            continue;
        }
        if (a == "--width" && i + 1 < argc) { opt.width = std::atoi(argv[++i]); continue; }
        if (a == "--height" && i + 1 < argc) { opt.height = std::atoi(argv[++i]); continue; }
        if (a == "--ui-scale" && i + 1 < argc) {
            opt.uiScale = std::atof(argv[++i]);
            if (opt.uiScale <= 0.0) opt.uiScale = 1.5;
            continue;
        }
        if (a == "--bench" && i + 1 < argc) {
            opt.benchFrames = std::atoi(argv[++i]);
            continue;
        }
        if (a == "--bench-ui" && i + 1 < argc) {
            opt.benchUiFrames = std::atoi(argv[++i]);
            continue;
        }
        if (a == "--bench-csv" && i + 1 < argc) {
            opt.benchCsvPath = argv[++i];
            continue;
        }
        if (a == "--screenshot" && i + 1 < argc) {
            opt.screenshot = argv[++i];
            continue;
        }
        if (a == "--shot-after" && i + 1 < argc) {
            opt.autoShotAfterSec = std::atof(argv[++i]);
            continue;
        }
        if (a == "--frame" && i + 1 < argc) {
            opt.startFrame = std::atoi(argv[++i]);
            continue;
        }
        if (a == "--zoom" && i + 1 < argc) { opt.zoomFactor = std::atof(argv[++i]); continue; }
        if (a == "--center" && i + 2 < argc) {
            opt.centerX = std::atof(argv[i + 1]);
            opt.centerY = std::atof(argv[i + 2]);
            i += 2; continue;
        }
        if (a == "--no-cull") { opt.renderCull = false; continue; }
        if (a == "--paused") { opt.paused = true; continue; }
        if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "superplace-viz: 未知参数 '%s'（用 -h 查看帮助）\n", a.c_str());
            return 2;
        }
        opt.initialBundle = a;
    }

    viz::App app(std::move(opt));
    return app.run();
}
