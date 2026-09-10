// 布局结果可视化（课程任务 6 第一种方式：CImg 绘图）。
// 铁律 2：数学坐标系 -> 屏幕坐标系的 Y 轴翻转，全项目仅此模块出现。
#pragma once

#include <string>

namespace sp {

class PlaceDB;
struct Config;

class CimgRenderer {
public:
    /// 绘制当前布局并保存为 PNG。path 为完整文件路径（含 .png）。
    static void plot(const std::string& path, const PlaceDB& db, const Config& cfg,
                     const std::string& caption = "");
};

}  // namespace sp
