#include "plot/cimg_renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "db/place_db.h"
#include "plot/png_writer.h"
#include "util/config.h"
#include "util/logger.h"

// CImg 仅用于图像绘制，显示后端已在 CMake 中关闭（cimg_display=0），不依赖 X11。
#include "CImg.h"

namespace sp {
namespace {

using cimg_library::CImg;

const unsigned char kWhite[]  = {255, 255, 255};
const unsigned char kBlack[]  = {0, 0, 0};
const unsigned char kRed[]    = {220, 60, 60};    // 标准单元
const unsigned char kOrange[] = {245, 150, 40};   // 宏单元
const unsigned char kBlue[]   = {60, 110, 220};   // 固定终端
const unsigned char kGreen[]  = {80, 190, 90};    // 填充单元
const unsigned char kGray[]   = {150, 150, 150};  // core 边界

}  // namespace

void CimgRenderer::plot(const std::string& path, const PlaceDB& db, const Config& cfg,
                        const std::string& caption) {
    const Rect& chip = db.chipRegion;
    const float chipW = chip.width();
    const float chipH = chip.height();
    if (chipW <= 0.f || chipH <= 0.f) {
        SP_WARN("chipRegion is degenerate, skip plotting");
        return;
    }

    // 短边固定为 plot_min_side，长边按纵横比缩放
    int imgW, imgH;
    if (chipW < chipH) {
        imgW = cfg.plot_min_side;
        imgH = static_cast<int>(std::lround(chipH / chipW * cfg.plot_min_side));
    } else {
        imgH = cfg.plot_min_side;
        imgW = static_cast<int>(std::lround(chipW / chipH * cfg.plot_min_side));
    }
    imgW = std::max(imgW, 1);
    imgH = std::max(imgH, 1);

    const int margin = cfg.plot_margin;
    const float unitX = static_cast<float>(imgW) / chipW;
    const float unitY = static_cast<float>(imgH) / chipH;

    CImg<unsigned char> img(imgW + 2 * margin, imgH + 2 * margin, 1, 3);
    img.draw_rectangle(0, 0, img.width() - 1, img.height() - 1, kWhite);

    // 数学坐标 -> 屏幕坐标。铁律 2：Y 轴翻转全项目仅此两行。
    auto sx = [&](float x) { return static_cast<int>((x - chip.lx) * unitX) + margin; };
    auto sy = [&](float y) { return static_cast<int>((chipH - (y - chip.ly)) * unitY) + margin; };

    auto drawBox = [&](const Rect& r, const unsigned char* color, float opacity) {
        int x0 = sx(r.lx), x1 = sx(r.hx);
        int y0 = sy(r.hy), y1 = sy(r.ly);   // 翻转后 hy 对应上边
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        // 保证亚像素单元至少可见 1px，否则大规模设计上整片空白
        if (x1 == x0) ++x1;
        if (y1 == y0) ++y1;
        img.draw_rectangle(x0, y0, x1, y1, color, opacity);
    };

    // ---- core 区域边框
    drawBox(Rect{db.coreRegion.lx, db.coreRegion.ly, db.coreRegion.hx, db.coreRegion.hy},
            kGray, 0.15f);

    // ---- 固定终端（跳过零面积的 terminal_NI）
    for (int i = db.numMovable; i < db.numNodes; ++i) {
        if (db.isNI(i)) continue;
        drawBox(db.box(i), kBlue, 0.7f);
    }

    // ---- 可移动单元：宏 vs 标准单元
    for (int i = 0; i < db.numMovable; ++i) {
        drawBox(db.box(i), db.isMacro(i) ? kOrange : kRed, 0.7f);
    }

    // ---- 填充单元（可选）
    if (cfg.plot_fillers) {
        for (int i = db.numNodes; i < db.totalNodes(); ++i) drawBox(db.box(i), kGreen, 0.5f);
    }

    if (!caption.empty()) {
        // 背景色必须给出具体类型的空指针，否则模板参数无法推导
        const unsigned char* noBg = nullptr;
        img.draw_text(margin, 8, caption.c_str(), kBlack, noBg, 1.0f, 24u);
    }

    // ---- 转为交错 RGB 后写 PNG（CImg 内部是分平面存储）
    const int W = img.width(), H = img.height();
    std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3u);
    const unsigned char* r = img.data(0, 0, 0, 0);
    const unsigned char* g = img.data(0, 0, 0, 1);
    const unsigned char* b = img.data(0, 0, 0, 2);
    for (size_t p = 0, n = static_cast<size_t>(W) * H; p < n; ++p) {
        rgb[p * 3 + 0] = r[p];
        rgb[p * 3 + 1] = g[p];
        rgb[p * 3 + 2] = b[p];
    }
    writePng(path, rgb, W, H);
    SP_INFO("plot saved: %s (%dx%d)", path.c_str(), W, H);
}

}  // namespace sp
