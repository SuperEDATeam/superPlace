// PNG 输出（RGB8，非隔行，无元数据），基于 libpng。
//
// 该接口刻意不暴露实现细节，因此底层换成什么都不影响调用方：
// M1 初版曾用「手写容器 + zlib」实现（本机当时没有 libpng），
// 后切换为 libpng 以获得逐行自适应滤波与成熟的格式实现，调用方零改动。
//
// 若将来需要 alpha 通道、16 位深度或 tEXt 元数据（例如把 HPWL/λ 写进图像属性），
// 都在本模块内部扩展即可。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sp {

/// 将交错的 RGB8 缓冲写为 PNG。rgb.size() 必须等于 width*height*3。
/// 失败抛 std::runtime_error。
void writePng(const std::string& path, const std::vector<uint8_t>& rgb, int width, int height);

}  // namespace sp
