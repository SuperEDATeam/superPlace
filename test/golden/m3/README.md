# M3 0.10 黄金结果

配置为 `configs/m3/golden.json`，种子 1002，CPU 后端。三个设计的 `init.pl` / `gp.pl`
均已验证在 1、8、32 线程下逐字节相同，并通过对应 9 项 CTest。

首次生成环境（2026-11-09）：x86_64、GCC 13.3.0（Ubuntu 24.04）、FFTW3f 3.3.10、Release、`ENABLE_NATIVE=ON`。
参考二进制 SHA256：`7b4815088f16b0bb0f7062759cf4c05c072d7ff72423606cf055edd93331d74c`。
源码基点为 `06f7fc8` 加本次 M3 改动；完整实验配置和二进制标识见
`results/m3/<实验>/<suite>/<设计>/seed-N/manifest.json`。

逐线程一致不代表不同编译器、指令集或 FFTW 构建必然逐位一致。当前环境的未修改算法基线
也不匹配旧黄金，因此保留旧文件，不用新哈希覆盖历史记录。跨环境失败时应先用同环境
基线比较，查清差异后再决定是否更新。

更新新集合：`python3 scripts/m3_golden.py --bin <CLI> --update`。
当前集合检查：`ctest --test-dir <build> -L m3 --output-on-failure`。
旧集合显式注册：配置 `-DENABLE_LEGACY_GOLDEN=ON` 后使用 `-L legacy`。
