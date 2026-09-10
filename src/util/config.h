// 运行配置：JSON 文件 + CLI 覆盖（05 §4/决策 5，参数不进源码）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sp {

struct Config {
    // ---------------------------------------------------------------- 路径
    std::string aux_path;
    std::string config_path;
    std::string out_dir = "results";
    std::string benchmark_name;   // 由 aux_path 推导

    /// 要执行的阶段，按序组装 pipeline。M1 仅支持 "parse"。
    std::vector<std::string> stages{"parse"};

    // ---------------------------------------------------------------- 通用
    uint64_t random_seed = 1002;
    int      num_threads = 0;     // 0 = 交给 OpenMP 决定
    bool     verbose     = false;

    // ---------------------------------------------------------------- 绘图
    bool plot          = true;
    bool full_plot     = false;   // 逐迭代出图
    int  plot_interval = 10;
    int  plot_min_side = 1000;    // 短边像素数
    int  plot_margin   = 30;
    bool plot_fillers  = false;

    // ------------------------------------------------- 后续里程碑参数（M1 未使用）
    float target_density   = 1.0f;
    float target_overflow  = 0.10f;
    int   gp_max_iter      = 1000;
    int   qp_max_iter      = 20;
    float qp_min_distance  = 1.0f;
    int   ignore_net_degree = 100;

    /// 从命令行构造。会先读 --config 指向的 JSON，再让 CLI 参数覆盖。
    /// 解析失败时抛 std::runtime_error。
    static Config fromArgs(int argc, char** argv);

    /// 读取 JSON 配置（不覆盖已由 CLI 显式指定的项）。
    void loadJson(const std::string& path);

    static std::string usage();
};

}  // namespace sp
