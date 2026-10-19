#include "render/canvas.h"

#include <chrono>
#include <string>

namespace viz {
namespace {

// 配色与算法侧 PNG 输出（src/plot/cimg_renderer.cpp）一致，
// 这样界面和报告里的图看起来是同一个东西。
// 顺序必须与 Kind 枚举对齐：Filler / Fixed / Cell / Macro
const float kColors[4][4] = {
    {0.31f, 0.75f, 0.35f, 0.55f},   // filler 绿
    {0.24f, 0.43f, 0.86f, 0.85f},   // 固定终端 蓝
    {0.86f, 0.24f, 0.24f, 0.80f},   // 标准单元 红
    {0.96f, 0.59f, 0.16f, 0.90f},   // 宏 橙
};

const char* kVert = R"(#version 330 core
layout(location=0) in vec2 aCorner;   // 单位方片 0..1
layout(location=1) in vec2 aCenter;   // 每实例：中心（设计坐标）
layout(location=2) in vec2 aSize;     // 每实例：宽高
layout(location=3) in float aKind;    // 每实例：类别

uniform vec4 uView;       // (cx, cy, scalePxPerUnit, 未用)
uniform vec2 uViewport;   // 像素
uniform float uMinPx;

flat out int vKind;

void main() {
    vKind = int(aKind);
    // 亚像素保护：全芯片视角下标准单元只有零点几个像素，原样画会碎成一片
    // 摩尔纹噪点，看起来像"布局是坏的"。把屏幕空间边长撑到至少 uMinPx。
    vec2 pxSize = aSize * uView.z;
    vec2 size = aSize * max(vec2(1.0), uMinPx / max(pxSize, vec2(1e-6)));

    vec2 world = aCenter + (aCorner - 0.5) * size;
    vec2 px = (world - uView.xy) * uView.z;          // 相对视野中心的像素偏移
    gl_Position = vec4(px / (uViewport * 0.5), 0.0, 1.0);
}
)";

const char* kFrag = R"(#version 330 core
flat in int vKind;
uniform vec4 uColors[4];
out vec4 fragColor;
void main() {
    vec4 c = uColors[vKind];
    if (c.a <= 0.0) discard;       // 图层关闭
    fragColor = c;
}
)";

unsigned compile(GLenum type, const char* src, std::string* err) {
    const unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        *err = std::string("着色器编译失败: ") + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

bool Canvas::init(std::string* error) {
    const unsigned vs = compile(GL_VERTEX_SHADER, kVert, error);
    if (!vs) return false;
    const unsigned fs = compile(GL_FRAGMENT_SHADER, kFrag, error);
    if (!fs) return false;

    prog_ = glCreateProgram();
    glAttachShader(prog_, vs);
    glAttachShader(prog_, fs);
    glLinkProgram(prog_);
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(prog_, sizeof(log) - 1, nullptr, log);
        *error = std::string("着色器链接失败: ") + log;
        return false;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    uView_ = glGetUniformLocation(prog_, "uView");
    uColors_ = glGetUniformLocation(prog_, "uColors");
    uMinPx_ = glGetUniformLocation(prog_, "uMinPx");
    uViewport_ = glGetUniformLocation(prog_, "uViewport");

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &quad_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glGenBuffers(1, &bufCenter_);
    glGenBuffers(1, &bufSize_);
    glGenBuffers(1, &bufKind_);
    const struct { unsigned loc, buf, comps; } attrs[] = {
        {1, bufCenter_, 2}, {2, bufSize_, 2}, {3, bufKind_, 1},
    };
    for (const auto& a : attrs) {
        glBindBuffer(GL_ARRAY_BUFFER, a.buf);
        glEnableVertexAttribArray(a.loc);
        glVertexAttribPointer(a.loc, static_cast<GLint>(a.comps), GL_FLOAT, GL_FALSE, 0, nullptr);
        glVertexAttribDivisor(a.loc, 1);   // 每实例取一次，而非每顶点
    }
    glBindVertexArray(0);
    return true;
}

void Canvas::shutdown() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    const unsigned bufs[] = {quad_, bufCenter_, bufSize_, bufKind_};
    for (unsigned b : bufs)
        if (b) glDeleteBuffers(1, &b);
    vao_ = quad_ = bufCenter_ = bufSize_ = bufKind_ = 0;
    count_ = 0;
}

void Canvas::setGeometry(const NodeGeometry& geom) {
    count_ = static_cast<int>(geom.w.size());
    if (count_ <= 0) return;
    const size_t n = static_cast<size_t>(count_);

    std::vector<float> size(n * 2), kind(n);
    for (size_t i = 0; i < n; ++i) {
        // 零面积的 terminal_NI（MMS 数据里有几百个）直接把尺寸置 0 即可。
        // 亚像素保护写的是 `aSize * max(1.0, ...)`，0 乘任何数还是 0，
        // 退化四边形不产生任何片元，GPU 自己就消掉了——不需要额外的 discard 分支，
        // 更不能把 kind 设成 -1 去索引 uColors[]，那是越界 UB。
        const bool skip = (geom.flags[i] & F_NI) != 0 || geom.w[i] <= 0.f || geom.h[i] <= 0.f;
        size[i * 2] = skip ? 0.f : geom.w[i];
        size[i * 2 + 1] = skip ? 0.f : geom.h[i];
        kind[i] = static_cast<float>(geom.kind[i]);
    }

    glBindBuffer(GL_ARRAY_BUFFER, bufSize_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * 2 * sizeof(float)), size.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, bufKind_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(float)), kind.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, bufCenter_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * 2 * sizeof(float)), nullptr,
                 GL_DYNAMIC_DRAW);
    interleaved_.assign(n * 2, 0.f);
}

void Canvas::setPositions(const float* x, const float* y, int count) {
    if (count != count_ || count_ <= 0) return;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < count_; ++i) {
        interleaved_[static_cast<size_t>(i) * 2] = x[i];
        interleaved_[static_cast<size_t>(i) * 2 + 1] = y[i];
    }
    glBindBuffer(GL_ARRAY_BUFFER, bufCenter_);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(interleaved_.size() * sizeof(float)),
                    interleaved_.data());
    uploadMs_ = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
}

void Canvas::draw(const Camera& cam, int viewW, int viewH) {
    if (count_ <= 0 || viewW <= 0 || viewH <= 0) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_);
    glBindVertexArray(vao_);

    glUniform4f(uView_, cam.cx, cam.cy, cam.scale, 0.f);
    glUniform2f(uViewport_, static_cast<float>(viewW), static_cast<float>(viewH));
    glUniform1f(uMinPx_, minPixelSize);

    // 隐藏某类就把它的 alpha 置 0，片元里 discard。比按类拆多个 draw call
    // 简单得多，而 33 万实例下 GPU 完全不在乎这点无效片元。
    float cols[16];
    for (int k = 0; k < 4; ++k) {
        for (int c = 0; c < 3; ++c) cols[k * 4 + c] = kColors[k][c];
        cols[k * 4 + 3] = layerVisible[k] ? kColors[k][3] : 0.f;
    }
    glUniform4fv(uColors_, 4, cols);

    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count_);
    glBindVertexArray(0);
    glUseProgram(0);
}

}  // namespace viz
