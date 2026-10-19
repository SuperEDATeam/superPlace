// superplace-gui —— 集成版：内嵌布局器，实时观察迭代过程。
//
// 与 superplace-viz 的差别**只有一行**：注入的是 LiveSource 而不是 BundleSource。
// 窗口、画布、面板、交互全部是同一份代码（viz_core）。
//
//     ./superplace-gui <design.aux> [--stride N]
//     ./superplace-gui --bundle <目录>      # 也能当离线查看器用
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app.h"
#include "viz/live_source.h"

int main(int argc, char** argv) {
    viz::AppOptions opt;
    std::string aux, bundle;
    int stride = 5;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::printf(
                "superplace-gui —— 内嵌引擎的布局查看器\n\n"
                "用法: superplace-gui <design.aux> [--stride N]\n"
                "      superplace-gui --bundle <目录>\n\n"
                "--stride N  每 N 轮迭代发布一帧（默认 5）。调小更流畅，但内存占用\n"
                "            按 节点数x8 字节/帧 线性增长。\n");
            return 0;
        }
        if (a == "--stride" && i + 1 < argc) { stride = std::atoi(argv[++i]); continue; }
        if (a == "--bundle" && i + 1 < argc) { bundle = argv[++i]; continue; }
        if (a == "--screenshot" && i + 1 < argc) { opt.screenshot = argv[++i]; continue; }
        if (a == "--shot-after" && i + 1 < argc) { opt.autoShotAfterSec = std::atof(argv[++i]); continue; }
        if (a == "--frame" && i + 1 < argc) { opt.startFrame = std::atoi(argv[++i]); continue; }
        aux = a;
    }

    viz::App app(opt);
    std::string err;
    if (!bundle.empty()) {
        auto src = viz::makeBundleSource(bundle, &err);
        if (!src) { std::fprintf(stderr, "加载 bundle 失败：%s\n", err.c_str()); return 1; }
        app.setSource(std::move(src));
    } else if (!aux.empty()) {
        auto src = viz::makeLiveSource(aux, stride, &err);
        if (!src) { std::fprintf(stderr, "启动布局器失败：%s\n", err.c_str()); return 1; }
        app.setSource(std::move(src));
    } else {
        std::fprintf(stderr, "需要一个 .aux 或 --bundle，见 --help\n");
        return 1;
    }
    return app.run();
}
