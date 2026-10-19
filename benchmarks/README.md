# benchmarks/ — 测试数据（需自行下载）

本目录体积 5.3 GB，**不进 git**（`.gitignore` 里只放行了这份 README）。
clone 仓库后这里是空的，按下面说明自行下载。

不下载也能跑：`test_data/` 里的小样例足够跑通流程，前端另有自带的合成样本
（`gui/sample-data/`）。下面这些是**做规模验证与混合尺寸验证**才需要的。

## 应有的内容

```
benchmarks/
├── mms/          16 个设计，2.6 GB   ← 最重要，混合尺寸布局的唯一验证数据
├── ispd2005/      8 个设计，968 MB
├── ispd2006/      8 个设计，1.4 GB
└── _dl/          原始压缩包，270 MB（可选保留）
```

### mms/ — Modern Mixed-Size（**优先下这个**）

Yan / Viswanathan / Chu 在 DAC 2009 基于 ISPD2005/2006 构造，
把原版中**固定的宏单元解放为可移动**，是混合尺寸布局的事实标准 benchmark。

**为什么非它不可**：ISPD2005/2006 原版把宏全部固定，我们四阶段流程里的
mLG / FILLERONLY / cGP **一次都不会触发**。MMS 的 16 个设计全部带可移动宏
（adaptec1 有 63 个，bigblue3 有 2549 个），是唯一能真正走通混合尺寸路径的数据。

**下载**：[mms_mod.zip（Dropbox）](https://www.dropbox.com/scl/fi/23i1jlmc9xhvfnj0jv82k/mms_mod.zip?rlkey=x4c0a4r877ii5ychu5lk4ur48&st=iyq93xlz&dl=0)

解压后应得到 16 个设计目录，外加官方的 `readme`、`run.pl`、`check_density_target.pl`。

> 官方原站 `public.iastate.edu/~zijunyan` 已失效；IEEE DataPort 上的重传版
> （DOI `10.21227/2n68-tx57`）需要付费订阅。上面这个 Dropbox 链接是目前可用的途径。

> ⚠️ 两处格式特点，读数据前要知道：MMS 把 I/O pad 塌缩成**左下角一个点**
> （尺寸置 0、引脚偏移一律清零），且**所有可移动对象的 `.pl` 坐标都是 0**——
> 不能直接拿它的 `.pl` 当初值，必须先跑初始布局。我们的流程本来就是 QP 起步，不受影响。

### ispd2005/ 与 ispd2006/ — 规模阶梯

来自 IBM 真实 ASIC 设计，21 万 → 250 万单元，用于性能、内存与数值稳定性验证。
宏单元**全部固定**，所以验证不了混合尺寸（那是 MMS 的活）。
ISPD2006 的额外价值是每个设计指定不同的密度目标（0.5 ~ 0.9），
可以验证 `target_density` 在全范围上的收敛性。

**下载**：

```bash
# ISPD2005（8 个）
BASE=https://www.ispd.cc/contests/05/ispd05-contest
wget $BASE/adaptec1.tar.gz $BASE/adaptec3.tar.gz
for d in adaptec2 adaptec4 bigblue1 bigblue2 bigblue3 bigblue4; do
    wget $BASE/benchmarks/$d.tar.gz
done

# ISPD2006（8 个）
BASE=https://www.ispd.cc/contests/06/contest
for d in adaptec5 newblue1 newblue2 newblue3 newblue4 newblue5 newblue6 newblue7; do
    wget $BASE/$d.tar.gz
done
```

> ⚠️ **`wget` / `curl` 可能拿到一个 12 KB 的 HTML 挑战页而不是 tar**
> （ispd.cc 有反爬，对 `HEAD` 放行、对 `GET` 返回带 JS 跳转的页面）。
> 下完务必 `file *.tar.gz` 确认类型；若中招，改用浏览器手动下载。
>
> UT Austin 的打包版 `cerc.utexas.edu/~zixuan/ispd2005dp.tar.xz` 已返回 403，不要用。

解压后按 `<套件>/<设计>/<设计>.aux` 组织，与 `test_data/` 一致，
这样 `scripts/run_matrix.py` 扫描目录就能自动发现全部用例。

### _dl/ — 原始压缩包（可选）

ISPD2005/2006 的 16 个 `.tar.gz`。数据上与解压目录冗余，删了不影响使用。
但**上游源随时可能失效**（已经坏过两个），而且给别人传数据时直接发
`adaptec1.tar.gz`（4.9 MB）比打包解压目录方便，建议留着。

## 校验

下载解压后，用解析器确认数据完整：

```bash
./build/superplace-cli --aux benchmarks/mms/adaptec1/adaptec1.aux --stage parse
```

adaptec1 应报告 `Objects: 211447 (fixed: 480, macro: 63)`、
`Nets: 221142`、`Pins: 944053`。各设计的宏数量对照表见
[work_report/04-Benchmark 调研与获取指南.md](../work_report/04-Benchmark%20调研与获取指南.md)。
