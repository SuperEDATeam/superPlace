// T34：当前帧 GDSII 导出（纯文件 IO，不依赖 GL/ImGui，可无头单测）。
//
// 格式要点见 work_report/06 §9.1e T34：全部大端；记录集
// HEADER/BGNLIB/LIBNAME/UNITS/BGNSTR/STRNAME/BOUNDARY/LAYER/DATATYPE/XY/ENDEL/ENDSTR/ENDLIB；
// UNITS 是 excess-64 8 字节实数（非 IEEE754）；坐标为 round(值×1000) 的 int32。
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "viz/scene.h"

namespace viz {

/// GDS 8 字节实数编码（excess-64，以 16 为底）。0 写成全零。
std::array<uint8_t, 8> encodeGdsReal(double v);

/// 节点 -> GDS 图层。返回 0 表示不导出（filler、零面积 F_NI 等）。
/// 1 标准单元 / 2 宏 / 3 固定终端 / 10 Core 边界由调用方单独写。
int gdsLayerForKind(uint8_t kind, uint8_t flags);

/// 把一帧节点矩形 + core 边界同步写成一个 GDSII 文件。
/// px/py 为节点中心（暂停在插值中间时传插值位置）；geom.w/h 为宽高。
/// @return 写出的文件路径；失败返回空串并填 err。
std::string writeGdsFile(const std::string& outPath, const std::string& designName,
                         const NodeGeometry& geom, const float* px, const float* py,
                         const Rect& core, std::string* err);

}  // namespace viz
