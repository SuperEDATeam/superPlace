// summary.json 健壮解析（T1）。
//
// 设计要点（06 §T1）：键会随算法演进增减，**不写死键名清单**，有则显示、缺则跳过；
// 文件缺失/损坏/类型不符时返回 false 由调用方降级为"该 bundle 不含总览"，绝不崩溃。
// 本模块只依赖标准库 + nlohmann/json，不碰 GL，可脱离窗口单测。
#pragma once

#include <string>

#include "viz/scene.h"

namespace viz {

/// 从文件解析 summary.json。
/// @param path   summary.json 路径
/// @param out    解析结果（成功时清空后填充）
/// @param err    失败原因（可空）
/// @return true=成功且至少有一个条目；false=文件缺失/损坏/为空，out 保持空。
///
/// 容忍：顶层非对象、个别键类型不是标量、durations_ms 缺失或非对象、
/// 数字写成字符串等。遇到无法识别的子对象/数组直接跳过，不报错。
bool loadSummaryFile(const std::string& path, SummaryTable& out, std::string* err);

/// 把一个 SummaryValue 格式化成适合面板展示的字符串（数字按量级去掉多余 0）。
std::string formatSummaryValue(const SummaryValue& v);

/// 已知键的中文展示名；未知键返回 nullptr，调用方应原样显示键名。
const char* summaryDisplayName(const std::string& key);

/// 是否需要标红的告警项（06 §T1：cgp_macros_moved 非 0 是算法侧 bug 信号）。
bool summaryIsWarning(const std::string& key, const SummaryValue& v);

}  // namespace viz
