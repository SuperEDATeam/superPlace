#include "plot/png_writer.h"

#include <png.h>

#include <csetjmp>
#include <cstdio>
#include <stdexcept>

namespace sp {
namespace {

/// RAII 清理：libpng 的错误处理走 longjmp，必须保证任何退出路径都释放资源。
struct PngWriteGuard {
    png_structp png = nullptr;
    png_infop   info = nullptr;
    std::FILE*  fp = nullptr;

    ~PngWriteGuard() {
        if (png) png_destroy_write_struct(&png, info ? &info : nullptr);
        if (fp) std::fclose(fp);
    }
};

}  // namespace

void writePng(const std::string& path, const std::vector<uint8_t>& rgb, int width, int height) {
    if (width <= 0 || height <= 0)
        throw std::runtime_error("writePng: non-positive dimensions");
    if (rgb.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3u)
        throw std::runtime_error("writePng: buffer size mismatch");

    PngWriteGuard g;

    g.fp = std::fopen(path.c_str(), "wb");
    if (!g.fp) throw std::runtime_error("writePng: cannot open " + path);

    g.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!g.png) throw std::runtime_error("writePng: png_create_write_struct failed");

    g.info = png_create_info_struct(g.png);
    if (!g.info) throw std::runtime_error("writePng: png_create_info_struct failed");

    // 行指针必须在 setjmp 之前构造完毕：longjmp 之后，setjmp 与 longjmp 之间
    // 被修改过的非 volatile 局部变量值是不确定的。
    std::vector<png_const_bytep> rows(static_cast<size_t>(height));
    for (int y = 0; y < height; ++y)
        rows[static_cast<size_t>(y)] =
            rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;

    // setjmp 返回非 0 表示 libpng 内部报错并跳了回来。此时仍在本函数栈帧内，
    // 抛异常可正常退栈，PngWriteGuard 析构会释放资源。
    if (setjmp(png_jmpbuf(g.png))) throw std::runtime_error("writePng: libpng error on " + path);

    png_init_io(g.png, g.fp);

    png_set_IHDR(g.png, g.info, static_cast<png_uint_32>(width),
                 static_cast<png_uint_32>(height), /*bit_depth=*/8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    // 显式固定压缩参数，保证同输入逐字节可复现（铁律 7），
    // 同时让 libpng 逐行自适应选择滤波器——这是相对手写编码器的主要收益。
    png_set_compression_level(g.png, 9);
    png_set_filter(g.png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);

    png_write_info(g.png, g.info);
    // png_write_image 的签名要求 png_bytepp，这里的数据实际只读
    png_write_image(g.png, const_cast<png_bytepp>(rows.data()));
    png_write_end(g.png, nullptr);
}

}  // namespace sp
