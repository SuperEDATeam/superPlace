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

    // ---------------------------------------------------------- 初始布局（M2）
    /// random | cluster_fc | cluster_bc | quadratic
    std::string init_method = "quadratic";
    /// 聚类目标簇数；<=0 表示按每簇约 25 个单元自动推算
    int   cluster_target_count = 0;
    /// 密度统计网格维度（任务 4 的「面积控制」维度）
    int   density_stat_dim = 64;

    // 二次解析布局
    int   qp_max_iter        = 20;    // 外层迭代（权重依赖位置，须迭代）
    float qp_tol             = 1e-6f; // BiCGSTAB 残差阈值
    int   qp_solver_max_iter = 100;   // 单次 BiCGSTAB 上限
    float qp_min_distance    = 1.0f;  // 权重分母下限，防止奇异

    // ------------------------------------------------------ 全局布局（M3）
    float target_density    = 1.0f;
    float target_overflow   = 0.10f;   ///< mGP 停止阈值 τ
    float cgp_target_overflow = 0.07f; ///< cGP 停止阈值（比 mGP 更严）
    int   gp_max_iter       = 1000;
    int   ignore_net_degree = 100;
    /// bin 网格维度；0 表示按 05 §5.5.3 自动推算（2 的幂，上限 1024）
    int   gp_bin_dim        = 0;
    /// λ 调度的 ΔHPWL 参考尺度（05 §5.5.9）
    float delta_hpwl_ref    = 3.5e5f;
    /// Nesterov 是否启用 Barzilai-Borwein 步长
    bool  use_bb            = true;
    /// FILLERONLY 阶段的固定轮数
    int   filler_only_iters = 20;
    /// τ 连续多少轮没有改善就判定停滞并停止（<=0 关闭）。
    /// λ 调度是 ΔHPWL 的速率控制器，本身没有"τ 不动就收手"这一项，
    /// 密排混合尺寸设计上会出现 τ 锁死而 HPWL 无限膨胀的空转。
    int   gp_stagnation_window = 200;

    // -------------------------------------------------- 宏单元合法化 mLG（M3）
    /// SA 移动次数；0 表示按宏数量自动推算（每宏 3000 次，下限 2 万）
    int   macro_sa_moves       = 0;
    /// 初温标定：初始时一次典型恶化被接受的目标概率
    float macro_sa_init_accept = 0.9f;
    /// 终温与初温之比。按端点定义降温曲线，比给"每步降温率"更可解释——
    /// 后者的实际效果取决于移动次数，改一个参数会悄悄改变另一个的含义。
    float macro_sa_temp_ratio  = 1e-4f;

    /// 从命令行构造。会先读 --config 指向的 JSON，再让 CLI 参数覆盖。
    /// 解析失败时抛 std::runtime_error。
    static Config fromArgs(int argc, char** argv);

    /// 读取 JSON 配置（不覆盖已由 CLI 显式指定的项）。
    void loadJson(const std::string& path);

    static std::string usage();
};

}  // namespace sp
