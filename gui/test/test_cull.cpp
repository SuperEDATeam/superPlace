// 视口剔除纯函数单测：边缘半可见/隐藏类/动态 off→on/顺序保留/minsize 扩张/invalid kind。
#include "test_util.h"
#include "render/cull.h"
using namespace viz;
void testCull() {
    const int N = 6;
    float cx[6] = { 0, -10, 10, 0, 0, 0.9f };
    float cy[6] = { 0, 0, 0, 10, -10, 0 };
    float hw[6] = { 1, 1, 1, 1, 1, 0.2f };
    float hh[6] = { 1, 1, 1, 1, 1, 0.2f };
    float kind[6] = { 0, 0, 0, 0, 0, 0 };
    bool lv[4] = {true,true,true,true};
    auto vis = cullVisible(cx, cy, hw, hh, kind, N, lv, -1.f, 1.f, -1.f, 1.f);
    CHECK_TRUE(vis.size() == 2 && vis[0] == 0 && vis[1] == 5);
    bool lv2[4] = {false,true,true,true};
    vis = cullVisible(cx, cy, hw, hh, kind, N, lv2, -5.f, 5.f, -5.f, 5.f);
    CHECK_TRUE(vis.empty());
    cx[1] = 0;
    vis = cullVisible(cx, cy, hw, hh, kind, N, lv, -5.f, 5.f, -5.f, 5.f);
    CHECK_TRUE(vis.size() == 3 && vis[1] == 1 && vis[2] == 5);
}
void testCullExtra() {
    // minsize 扩张：节点恰好跨边界。
    float cx[3] = { 1.5f, -1.5f, 0 };
    float cy[3] = { 0, 0, 0 };
    float hw[3] = { 0.4f, 0.4f, 1 };
    float hh[3] = { 0.4f, 0.4f, 1 };
    float kind[3] = { 0, 0, 0 };
    bool lv[4] = {true,true,true,true};
    // 视口 [-1,1]，扩张 0.5 → [-1.5,1.5]，边缘节点纳入。
    auto v = cullVisible(cx, cy, hw, hh, kind, 3, lv, -1.5f, 1.5f, -1.5f, 1.5f);
    CHECK_TRUE(v.size() == 3);
    // 无扩张 → 边缘节点排除。
    v = cullVisible(cx, cy, hw, hh, kind, 3, lv, -1.f, 1.f, -1.f, 1.f);
    CHECK_TRUE(v.size() == 1 && v[0] == 2);
    // invalid kind=-1 跳过。
    kind[0] = -1; kind[1] = -1; kind[2] = -1;
    v = cullVisible(cx, cy, hw, hh, kind, 3, lv, -10.f, 10.f, -10.f, 10.f);
    CHECK_TRUE(v.empty());
}
