// 二维网格的非拥有视图（扁平 row-major 缓冲）。
//
// 铁律 1：本文件属于 numeric/ 层，不得引入任何布局语义。
// 它只描述"一块 nx*ny 的 float 缓冲怎么寻址"，不知道 bin 里装的是密度还是别的什么。
//
// 布局约定：row-major，**快轴为 y**，即 data[x * ny + y]。
// 这与 FFTW 的 fftwf_plan_r2r_2d(n0=nx, n1=ny, ...) 完全吻合（n1 为快轴），
// 也便于将来整块 cudaMemcpy 上显存（决策 7 / M7）。
#pragma once

namespace sp {

struct BinGridSpan {
    float* data = nullptr;
    int nx = 0;
    int ny = 0;

    float& operator()(int x, int y) { return data[x * ny + y]; }
    const float& operator()(int x, int y) const { return data[x * ny + y]; }

    int size() const { return nx * ny; }
    bool valid() const { return data != nullptr && nx > 0 && ny > 0; }
};

}  // namespace sp
