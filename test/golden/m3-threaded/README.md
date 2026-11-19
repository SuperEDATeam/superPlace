# M3 FFTW 多线程黄金

配置 `configs/m3/golden-threaded.json`，CPU、种子 1002、mGP/cGP 目标 0.10、密度 1.0。
FFTW 预算固定 4；小于 65536 个 bin 时实际计划预算固定 1，与 OpenMP 线程数无关。

生成环境（2026-11-19）：AMD Ryzen 7 8845H、Ubuntu 24.04、GCC 13.3.0、FFTW3f 3.3.10，
Release、ENABLE_NATIVE=ON。源码基点 b005f19 加本轮改动；完整源码及二进制指纹见
`results/fftw-threads-20261119/manifest.json`。

三个设计均通过 OpenMP 1/8/32 线程逐字节一致检查及对应 9 项 CTest。
本机六个 init/gp 哈希恰与串行集合相同；这不证明其他设计或其他机器也相同。
旧串行集合 `test/golden/m3/` 保留；跨机器确认状态见 07-M3 附录 C。

检查：`ctest --test-dir build/fftw-threads -L fftw --output-on-failure`。
有意更新时使用：

```bash
python3 scripts/m3_golden.py --bin build/fftw-threads/superplace-cli \
  --config configs/m3/golden-threaded.json --golden-dir test/golden/m3-threaded \
  --out results/fftw-threads-20261119/golden-generation --update
```

脚本要求请求预算实际生效，且同一设计在三种 OpenMP 线程数下哈希一致，才允许写入。
