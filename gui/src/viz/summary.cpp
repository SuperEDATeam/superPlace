#include "viz/summary.h"

#include <cstdio>
#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace viz {
namespace {

// 已知键的中文展示名。未知键不在表里，面板直接原样显示键名——
// 这样算法侧将来加新键不需要改这里也能看见（06 §T1：有则显示）。
const std::unordered_map<std::string, const char*>& knownLabels() {
    static const std::unordered_map<std::string, const char*> m = {
        {"gp_hpwl",            "最终 HPWL"},
        {"gp_hpwl_before",     "初始 HPWL"},
        {"gp_overflow",       "最终溢出率"},
        {"gp_movable_macros",  "可移动宏数"},
        {"gp_num_fillers",     "filler 数"},
        {"gp_out_of_core",     "越界单元数"},
        {"gp_time_ms",         "GP 总耗时 (ms)"},
        {"gp_frames",         "GP 出图帧数"},
        {"mgp_hpwl",          "mGP HPWL"},
        {"mgp_overflow",      "mGP 溢出率"},
        {"mgp_iterations",    "mGP 迭代轮数"},
        {"mlg_overlap_before","mLG 重叠前"},
        {"mlg_overlap_after", "mLG 重叠后"},
        {"mlg_macro_disp_mean","宏平均位移"},
        {"mlg_macro_disp_max", "宏最大位移"},
        {"mlg_hpwl",          "mLG 后 HPWL"},
        {"mlg_repaired",      "mLG 修复单元数"},
        {"filleronly_iterations","FILLERONLY 轮数"},
        {"cgp_hpwl",          "cGP HPWL"},
        {"cgp_overflow",      "cGP 溢出率"},
        {"cgp_iterations",    "cGP 迭代轮数"},
        {"cgp_macros_moved",  "cGP 被移动宏数"},
    };
    return m;
}

}  // namespace

bool loadSummaryFile(const std::string& path, SummaryTable& out, std::string* err) {
    out = {};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "打不开 " + path;
        return false;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        if (err) *err = std::string("summary.json 解析失败：") + e.what();
        return false;
    }
    if (!j.is_object()) {
        if (err) *err = "summary.json 顶层不是对象";
        return false;
    }

    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        const nlohmann::json& v = it.value();
        if (key == "durations_ms") {
            // 子对象单独摊平；不是对象就整段跳过，不影响其他键。
            if (v.is_object()) {
                for (auto d = v.begin(); d != v.end(); ++d) {
                    double num = 0.0;
                    if (d.value().is_number()) num = d.value().get<double>();
                    else if (d.value().is_string()) {
                        try { num = std::stod(d.value().get<std::string>()); } catch (...) {}
                    }
                    out.durationsMs.emplace_back(d.key(), num);
                }
            }
            continue;
        }
        SummaryValue sv;
        if (v.is_string()) {
            sv.isString = true;
            sv.text = v.get<std::string>();
        } else if (v.is_number()) {
            sv.isString = false;
            sv.number = v.get<double>();
        } else if (v.is_boolean()) {
            sv.isString = false;
            sv.number = v.get<bool>() ? 1.0 : 0.0;
        } else {
            continue;   // 数组/嵌套对象：跳过，不崩、不显示
        }
        out.entries.emplace_back(key, sv);
    }
    return !out.empty();
}

std::string formatSummaryValue(const SummaryValue& v) {
    if (v.isString) return v.text;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v.number);
    return buf;
}

const char* summaryDisplayName(const std::string& key) {
    const auto& m = knownLabels();
    const auto it = m.find(key);
    return it == m.end() ? nullptr : it->second;
}

bool summaryIsWarning(const std::string& key, const SummaryValue& v) {
    // 06 §T1：cgp_macros_moved 非 0 是算法侧 bug 信号，标红。
    if (key == "cgp_macros_moved" && !v.isString) return v.number != 0.0;
    return false;
}

}  // namespace viz
