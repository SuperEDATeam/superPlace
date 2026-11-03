// T14：回放控制器无头单测——插值、硬切、里程碑停留、循环、速度、无 t 退化。
#include "test_util.h"

#include <cmath>
#include <vector>

#include "viz/playback.h"

using namespace viz;

namespace {

FrameMeta mk(int idx, const char* stage, int iter, double t, bool brk) {
    FrameMeta m;
    m.index = idx;
    m.stage = stage;
    m.label = stage;
    m.iter = iter;
    m.t = t;
    m.breakBefore = brk;
    return m;
}

// 仿 newblue1 开头：input(里程碑,t0) -> init(里程碑,breakBefore,t0) ->
// mGP iter0(t0) -> mGP iter10(t10) -> mGP iter20(t20)。
std::vector<FrameMeta> sample() {
    return {mk(0, "input", -1, 0, false), mk(1, "init", -1, 0, true),
            mk(2, "mGP", 0, 0, false),   mk(3, "mGP", 10, 10, false),
            mk(4, "mGP", 20, 20, false)};
}

}  // namespace

void testPlayback() {
    PlaybackController pc;
    pc.setSpeed(60.0);
    pc.setMilestoneDwell(0.5);
    pc.setFrames(sample());

    // arrival: a0=0; 帧0里程碑停0.5, 0->1硬切gap0 => a1=0.5
    //          帧1里程碑停0.5, 1->2 t相等硬切gap0 => a2=1.0
    //          帧2普通, 2->3 smooth (10-0)/60=0.1667 => a3=1.1667
    //          帧3普通, 3->4 smooth (20-10)/60 => a4=1.3333; total=1.3333
    CHECK_NEAR(pc.totalDuration(), 4.0 / 3.0, 1e-3);

    // vt=0.25：帧0里程碑停留
    pc.scrubTo(0.25);
    PlaybackController::DisplayState s = pc.state();
    CHECK_EQ(s.lo, 0); CHECK_EQ(s.hi, 0);
    CHECK_TRUE(!s.interpolating);
    CHECK_EQ(s.statFrame, 0);

    // vt=0.75：帧1里程碑停留（硬切到 init，不插值）
    pc.scrubTo(0.75);
    s = pc.state();
    CHECK_EQ(s.lo, 1); CHECK_EQ(s.hi, 1);
    CHECK_TRUE(!s.interpolating);

    // vt=1.05：在 2->3 插值，alpha=(1.05-1.0)/(1.1667-1.0)=0.3
    pc.scrubTo(1.05);
    s = pc.state();
    CHECK_EQ(s.lo, 2); CHECK_EQ(s.hi, 3);
    CHECK_TRUE(s.interpolating);
    CHECK_NEAR(s.alpha, 0.3, 2e-2);
    CHECK_EQ(s.statFrame, 2);             // alpha<0.5 取 lo
    CHECK_NEAR(s.globalT, 3.0, 1e-2);     // 0 + 0.3*10

    // vt=1.2：在 3->4 插值，alpha≈0.2，statFrame=3，globalT=12
    pc.scrubTo(1.2);
    s = pc.state();
    CHECK_EQ(s.lo, 3); CHECK_EQ(s.hi, 4);
    CHECK_NEAR(s.alpha, 0.2, 2e-2);
    CHECK_EQ(s.statFrame, 3);
    CHECK_NEAR(s.globalT, 12.0, 1e-2);

    // 硬切：init(breakBefore) 与上一帧 t 相等，绝不插值。
    pc.scrubTo(0.55);
    s = pc.state();
    CHECK_EQ(s.lo, 1); CHECK_TRUE(!s.interpolating);

    // 速度缩放：120 轮/秒则 gap 减半。两个 smooth gap 各 10/120；
    // 里程碑停留 0.5+0.5 不随速度变 => total = 1.0 + 20/120 = 1.1667。
    pc.setSpeed(120.0);
    CHECK_NEAR(pc.totalDuration(), 1.0 + 20.0 / 120.0, 1e-3);
    pc.setSpeed(60.0);

    // 循环：推进越过 total 自动回绕。
    pc.setLoop(true);
    pc.scrubTo(pc.totalDuration() - 0.01);
    pc.play();
    pc.advance(1.0);   // 远超剩余，应回绕到前段
    s = pc.state();
    CHECK_TRUE(s.lo >= 0 && s.lo < 5);

    // 关键帧/阶段跳转
    pc.pause();
    pc.scrubTo(1.05);
    pc.nextKeyframe();
    CHECK_EQ(pc.state().lo, 3);
    pc.prevKeyframe();
    CHECK_EQ(pc.state().lo, 2);
    pc.nextStage();      // mGP -> 样本里后续无其他阶段，落到末帧 lo=4
    CHECK_TRUE(pc.state().lo == 4);
    pc.prevStage();      // 与 nextStage 对称：回到 init 阶段【首帧】lo=0（不是末帧1）
    CHECK_EQ(pc.state().lo, 0);
}

// 第三轮六项修复的覆盖（外部 probe 之外的仓内断言）。
void testPlaybackFixes() {
    // ① 重复尾停留：4 个里程碑帧 t 全为 0，每帧 dwell=0.5，total=2.0（不是 2.5）。
    {
        std::vector<FrameMeta> ms = {mk(0, "a", -1, 0, false), mk(1, "a", -1, 0, true),
                                     mk(2, "a", -1, 0, true), mk(3, "a", -1, 0, true)};
        PlaybackController pc; pc.setMilestoneDwell(0.5); pc.setFrames(ms);
        CHECK_NEAR(pc.totalDuration(), 2.0, 1e-6);
    }
    // ② 非循环尾端按播放从头重播。
    {
        std::vector<FrameMeta> ms = {mk(0, "a", 0, 0, false), mk(1, "a", 0, 10, false)};
        PlaybackController pc; pc.setSpeed(60); pc.setFrames(ms);
        pc.scrubTo(pc.totalDuration());
        CHECK_TRUE(!pc.playing());
        pc.play();                       // 在尾端按播放
        pc.advance(0.01);
        CHECK_TRUE(pc.playhead() < pc.totalDuration() - 0.05);  // 回到开头附近
    }
    // ③ setSpeed 保持 globalT（不跳画面）。
    {
        std::vector<FrameMeta> ms = {mk(0, "a", 0, 0, false), mk(1, "a", 0, 100, false),
                                     mk(2, "a", 0, 200, false)};
        PlaybackController pc; pc.setSpeed(60); pc.setFrames(ms);
        pc.scrubTo(1.667);               // 全局 t≈100
        const double g0 = pc.state().globalT;
        const int lo0 = pc.state().lo;
        pc.setSpeed(120);
        CHECK_NEAR(pc.state().globalT, g0, 1.0);
        CHECK_EQ(pc.state().lo, lo0);
    }
    // ④ next/prev 插值中段不跳过相邻帧。
    {
        std::vector<FrameMeta> ms;
        for (int i = 0; i < 5; ++i) ms.push_back(mk(i, "s", 0, (double)i * 10, false));
        PlaybackController pc; pc.setMilestoneDwell(0); pc.setSpeed(60); pc.setFrames(ms);
        pc.scrubTo(0.333 + 0.7 * 0.1667);   // lo=2 hi=3 alpha=.7
        pc.nextKeyframe();
        CHECK_EQ(pc.state().lo, 3);          // 不是 4
        pc.scrubTo(0.333 + 0.3 * 0.1667);   // lo=2 hi=3 alpha=.3
        pc.prevKeyframe();
        CHECK_EQ(pc.state().lo, 2);           // 不是 1
    }
    // ⑤ prevStage 到上一阶段首帧（与 nextStage 对称）。
    {
        std::vector<FrameMeta> ms = {mk(0, "mGP", 0, 0, false), mk(1, "mGP", 5, 50, false),
                                     mk(2, "mGP", 10, 100, false), mk(3, "cGP", 0, 150, false),
                                     mk(4, "cGP", 10, 200, false)};
        PlaybackController pc; pc.setMilestoneDwell(0); pc.setFrames(ms);
        pc.scrubTo(pc.totalDuration() * 0.8);   // cGP
        CHECK_EQ(pc.state().stage, "cGP");
        pc.prevStage();
        CHECK_EQ(pc.state().lo, 0);            // mGP 首帧，不是末帧2
        pc.nextStage();
        CHECK_EQ(pc.state().lo, 3);             // 回到 cGP 首帧
    }
    // ⑥ slider 用全局 t 轴：scrubToGlobal01(0.5) 落在中段 globalT≈中点。
    {
        std::vector<FrameMeta> ms = {mk(0, "a", 0, 0, false), mk(1, "a", 0, 100, false),
                                     mk(2, "a", 0, 200, false)};
        PlaybackController pc; pc.setSpeed(60); pc.setFrames(ms);
        pc.scrubToGlobal01(0.5);
        CHECK_NEAR(pc.state().globalT, 100.0, 2.0);
        CHECK_NEAR(pc.playheadGlobal01(), 0.5, 0.05);
    }
    // A UI toggle 走 play() 尾回卷：非循环在尾端 toggle+advance 应从头重播。
    {
        std::vector<FrameMeta> ms = {mk(0, "a", 0, 0, false), mk(1, "a", 0, 10, false)};
        PlaybackController pc; pc.setSpeed(60); pc.setFrames(ms);
        pc.scrubTo(pc.totalDuration());      // 停在尾
        CHECK_TRUE(!pc.playing());
        pc.toggle();                          // 按钮路径
        CHECK_TRUE(pc.playing());
        pc.advance(0.01);
        CHECK_TRUE(pc.playhead() < pc.totalDuration() - 0.05);  // 已回开头
    }
    // B 全同 t=0 非里程碑 3 帧 a/b/c：arrival 全 0，pin 必须能逐个到达 0/1/2。
    {
        std::vector<FrameMeta> ms = {mk(0, "a", 0, 0, false), mk(1, "b", 0, 0, false),
                                     mk(2, "c", 0, 0, false)};
        PlaybackController pc; pc.setMilestoneDwell(0.0); pc.setFrames(ms);
        pc.pinIndex(0); CHECK_EQ(pc.state().lo, 0); CHECK_EQ(pc.state().stage, "a");
        pc.pinIndex(1); CHECK_EQ(pc.state().lo, 1); CHECK_EQ(pc.state().stage, "b");
        pc.pinIndex(2); CHECK_EQ(pc.state().lo, 2); CHECK_EQ(pc.state().stage, "c");
        pc.nextKeyframe();   // 已在末帧，仍停在末帧
        CHECK_EQ(pc.state().lo, 2);
        pc.prevKeyframe(); CHECK_EQ(pc.state().lo, 1);  // 能回 b
        pc.prevKeyframe(); CHECK_EQ(pc.state().lo, 0);  // 能回 a
    }
    // hold 调速保留已走时长：init 里程碑走了 0.25s，调速后不得重置回整段起点。
    {
        std::vector<FrameMeta> ms = {mk(0, "init", -1, 0, false), mk(1, "mGP", 0, 5, false)};
        PlaybackController pc; pc.setSpeed(60); pc.setMilestoneDwell(0.5);
        pc.setFrames(ms);
        pc.scrubTo(0.25);                 // init hold 内已走 0.25
        const double vtBefore = pc.playhead();
        pc.setSpeed(120);                 // 调速
        // 仍在 init hold 内，且已走 0.25 不应被重置为 0
        CHECK_EQ(pc.state().lo, 0);
        CHECK_TRUE(pc.playhead() > vtBefore - 0.01 && pc.playhead() < 0.5);
    }
}

void testPlaybackDegrade() {
    std::vector<FrameMeta> fr;
    for (int i = 0; i < 5; ++i) fr.push_back(mk(i, "mGP", -2, (double)i, false));
    PlaybackController pc;
    pc.setSpeed(60.0);
    pc.setFrames(fr);
    // 每帧无里程碑停留，gap = 1/60；total = 4/60
    CHECK_NEAR(pc.totalDuration(), 4.0 / 60.0, 1e-4);
    pc.scrubTo(0.5 * pc.totalDuration());
    PlaybackController::DisplayState s = pc.state();
    CHECK_TRUE(s.interpolating);
    CHECK_TRUE(s.lo >= 0 && s.lo < 5);
}
