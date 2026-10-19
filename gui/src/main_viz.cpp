// superplace-viz —— 独立布局查看器。
//
// 这个可执行文件**不链接 libsuperplace**，也不需要 FFTW / Eigen / libpng / OpenMP。
// 它唯一认识的东西是 viz bundle 目录格式。
//
//     ./superplace-viz [bundle 目录]
//
// 不给参数时默认加载仓库自带的合成样本。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app.h"

int main(int argc, char** argv) {
    viz::AppOptions opt;
    opt.initialBundle = "sample-data/synthetic";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::printf(
                "superplace-viz —— superPlace 布局查看器\n\n"
                "用法: superplace-viz [bundle 目录] [--no-vsync]\n"
                "          [--bench N] [--screenshot out.ppm] [--shot-after 秒]\n\n"
                "bundle 由 scripts/make_viz.py 生成，格式见\n"
                "work_report/06-前端开发方案与接口契约.md\n");
            return 0;
        }
        if (a == "--no-vsync") {
            opt.vsync = false;      // 跑帧率基准时用，否则被 vsync 钳在 60
            continue;
        }
        if (a == "--bench" && i + 1 < argc) {
            opt.benchFrames = std::atoi(argv[++i]);
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
        opt.initialBundle = a;
    }

    viz::App app(std::move(opt));
    return app.run();
}
