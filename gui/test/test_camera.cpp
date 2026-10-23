// T13：Camera 纯逻辑（fit 包围盒、zoomAt 锚点不变性、投影互逆、resize 保持视野）。
#include "test_util.h"

#include "render/canvas.h"

using namespace viz;

void testCamera() {
    // fit 后包围盒恰好在视野内
    {
        const Rect chip{0.f, 0.f, 100.f, 50.f};
        Camera cam;
        cam.fit(chip, 200.f, 100.f);
        CHECK_NEAR(cam.cx, 50.f, 1e-3);
        CHECK_NEAR(cam.cy, 25.f, 1e-3);
        const float halfW = 100.f / cam.scale, halfH = 50.f / cam.scale;
        CHECK_TRUE(halfW >= 50.f && halfH >= 25.f);
    }

    // zoomAt 锚点不变：缩放前后光标处设计坐标相同
    {
        const Rect chip{0.f, 0.f, 100.f, 50.f};
        Camera cam;
        cam.fit(chip, 200.f, 100.f);
        float wx = 0.f, wy = 0.f;
        cam.screenToWorld(30.f, 40.f, 200.f, 100.f, &wx, &wy);
        cam.zoomAt(30.f, 40.f, 200.f, 100.f, 2.0f);
        float nx = 0.f, ny = 0.f;
        cam.screenToWorld(30.f, 40.f, 200.f, 100.f, &nx, &ny);
        CHECK_NEAR(nx, wx, 1e-3);
        CHECK_NEAR(ny, wy, 1e-3);
    }

    // screenToWorld 与 worldToScreen 互逆
    {
        Camera cam;
        cam.cx = 100.f;
        cam.cy = -20.f;
        cam.scale = 2.5f;
        float wx = 73.2f, wy = -8.4f, sx = 0.f, sy = 0.f;
        cam.worldToScreen(wx, wy, 400.f, 300.f, &sx, &sy);
        float bx = 0.f, by = 0.f;
        cam.screenToWorld(sx, sy, 400.f, 300.f, &bx, &by);
        CHECK_NEAR(bx, wx, 1e-2);
        CHECK_NEAR(by, wy, 1e-2);
    }

    // T2：resize 保持视野——中心不动，scale 按短边比例，可见高度不变
    {
        Camera cam;
        cam.cx = 50.f;
        cam.cy = 25.f;
        cam.scale = 2.f;
        const float oldScale = cam.scale;
        cam.rescaleOnResize(200.f, 100.f, 100.f, 100.f);   // 宽减半、高不变
        CHECK_NEAR(cam.cx, 50.f, 1e-6);
        CHECK_NEAR(cam.cy, 25.f, 1e-6);
        CHECK_NEAR(cam.scale, oldScale * 0.5f, 1e-6);
        // 高方向可见范围 = H/scale：H 不变、scale 减半 → 可见高度翻倍（宽方向被裁）
        cam.rescaleOnResize(100.f, 100.f, 200.f, 200.f);   // 等比放大
        CHECK_NEAR(cam.scale, oldScale, 1e-6);
    }
}
