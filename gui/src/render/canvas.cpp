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

// 连续色表（T7 局部密度 / T8 位移量共用）：蓝 -> 绿 -> 红。
const char* kCmapGLSL = R"(
vec3 cmap(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 lo = vec3(0.15, 0.25, 0.85);
    vec3 mid = vec3(0.20, 0.80, 0.40);
    vec3 hi = vec3(0.92, 0.25, 0.20);
    return t < 0.5 ? mix(lo, mid, t * 2.0) : mix(mid, hi, t * 2.0 - 1.0);
}
)";

const char* kVert = R"(#version 330 core
layout(location=0) in vec2 aCorner;   // 单位方片 0..1
layout(location=1) in vec2 aCenter;   // 每实例：中心（设计坐标）
layout(location=2) in vec2 aSize;     // 每实例：宽高
layout(location=3) in float aKind;     // 每实例：类别
layout(location=4) in float aValue;    // 每实例：连续属性值（T7/T8）

uniform vec4 uView;       // (cx, cy, scalePxPerUnit, 未用)
uniform vec2 uViewport;   // 像素
uniform float uMinPx;

flat out int vKind;
out float vValue;

void main() {
    vKind = int(aKind);
    vValue = aValue;
    // 亚像素保护：全芯片视角下标准单元只有零点几个像素，原样画会碎成一片
    // 摩尔纹噪点，看起来像"布局是坏的"。把屏幕空间边长撑到至少 uMinPx。
    vec2 pxSize = aSize * uView.z;
    vec2 size = aSize * max(vec2(1.0), uMinPx / max(pxSize, vec2(1e-6)));

    vec2 world = aCenter + (aCorner - 0.5) * size;
    vec2 px = (world - uView.xy) * uView.z;          // 相对视野中心的像素偏移
    gl_Position = vec4(px / (uViewport * 0.5), 0.0, 1.0);
}
)";

const std::string kFrag = std::string(R"(#version 330 core
flat in int vKind;
in float vValue;
uniform vec4 uColors[4];
uniform int uColorMode;
uniform float uValueMin, uValueMax;
out vec4 fragColor;
)") + kCmapGLSL + R"(
void main() {
    vec4 col;
    if (uColorMode == 0) {
        col = uColors[vKind];
    } else {
        float t = (vValue - uValueMin) / max(uValueMax - uValueMin, 1e-6);
        col = vec4(cmap(t), uColors[vKind].a);
    }
    if (col.a <= 0.0) discard;       // 图层关闭
    fragColor = col;
}
)";

// 热力图叠加程序：把归一化密度场画成覆盖 chip 的纹理四边形（T6）。
const char* kHmVert = R"(#version 330 core
layout(location=0) in vec2 aCorner;   // 0..1 四边形
uniform vec4 uView;
uniform vec2 uViewport;
uniform vec4 uChip;   // lx, ly, hx, hy
out vec2 vUv;
void main() {
    vUv = aCorner;
    vec2 world = mix(uChip.xy, uChip.zw, aCorner);
    vec2 px = (world - uView.xy) * uView.z;
    gl_Position = vec4(px / (uViewport * 0.5), 0.0, 1.0);
}
)";

const std::string kHmFrag = std::string(R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform float uAlpha;
out vec4 fragColor;
)") + kCmapGLSL + R"(
void main() {
    float t = texture(uTex, vUv).r;
    vec3 c = cmap(t);
    if (t <= 0.001) discard;
    fragColor = vec4(c, uAlpha);
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

unsigned linkProgram(const char* vsSrc, const char* fsSrc, std::string* err) {
    const unsigned vs = compile(GL_VERTEX_SHADER, vsSrc, err);
    if (!vs) return 0;
    const unsigned fs = compile(GL_FRAGMENT_SHADER, fsSrc, err);
    if (!fs) { glDeleteShader(vs); return 0; }
    const unsigned p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
        *err = std::string("着色器链接失败: ") + log;
        glDeleteProgram(p);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return ok ? p : 0;
}

}  // namespace

bool Canvas::init(std::string* error) {
    prog_ = linkProgram(kVert, kFrag.c_str(), error);
    if (!prog_) return false;

    uView_ = glGetUniformLocation(prog_, "uView");
    uColors_ = glGetUniformLocation(prog_, "uColors");
    uMinPx_ = glGetUniformLocation(prog_, "uMinPx");
    uViewport_ = glGetUniformLocation(prog_, "uViewport");
    uColorMode_ = glGetUniformLocation(prog_, "uColorMode");
    uValueMin_ = glGetUniformLocation(prog_, "uValueMin");
    uValueMax_ = glGetUniformLocation(prog_, "uValueMax");

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
    glGenBuffers(1, &bufValue_);
    const struct { unsigned loc, buf, comps; } attrs[] = {
        {1, bufCenter_, 2}, {2, bufSize_, 2}, {3, bufKind_, 1}, {4, bufValue_, 1},
    };
    for (const auto& a : attrs) {
        glBindBuffer(GL_ARRAY_BUFFER, a.buf);
        glEnableVertexAttribArray(a.loc);
        glVertexAttribPointer(a.loc, static_cast<GLint>(a.comps), GL_FLOAT, GL_FALSE, 0, nullptr);
        glVertexAttribDivisor(a.loc, 1);   // 每实例取一次，而非每顶点
    }
    glBindVertexArray(0);

    // 热力图程序（懒创建纹理，这里只建程序与 VAO）。
    std::string hmErr;
    hmProg_ = linkProgram(kHmVert, kHmFrag.c_str(), &hmErr);
    if (!hmProg_) std::fprintf(stderr, "[heatmap] 程序链接失败：%s\n", hmErr.c_str());
    if (hmProg_) {
        hmUView_ = glGetUniformLocation(hmProg_, "uView");
        hmUViewport_ = glGetUniformLocation(hmProg_, "uViewport");
        hmUTex_ = glGetUniformLocation(hmProg_, "uTex");
        hmUChip_ = glGetUniformLocation(hmProg_, "uChip");
        hmUAlpha_ = glGetUniformLocation(hmProg_, "uAlpha");
        glGenVertexArrays(1, &hmVao_);
        glBindVertexArray(hmVao_);
        glBindBuffer(GL_ARRAY_BUFFER, quad_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
    }
    return true;
}

void Canvas::shutdown() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (hmVao_) glDeleteVertexArrays(1, &hmVao_);
    if (hmTex_) glDeleteTextures(1, &hmTex_);
    const unsigned bufs[] = {quad_, bufCenter_, bufSize_, bufKind_, bufValue_};
    for (unsigned b : bufs)
        if (b) glDeleteBuffers(1, &b);
    if (prog_) glDeleteProgram(prog_);
    if (hmProg_) glDeleteProgram(hmProg_);
    vao_ = hmVao_ = hmTex_ = 0;
    prog_ = hmProg_ = 0;
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
    glBindBuffer(GL_ARRAY_BUFFER, bufValue_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(float)), nullptr,
                 GL_DYNAMIC_DRAW);
    interleaved_.assign(n * 2, 0.f);
    values_.assign(n, 0.f);
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

void Canvas::setContinuousValues(const float* values, int count) {
    if (count != count_ || count_ <= 0 || !values) return;
    // R3：类别模式下不激活连续值，直接跳过 bufValue_ 上传，避免无效 n 浮点传输。
    if (!valueActive_) return;
    glBindBuffer(GL_ARRAY_BUFFER, bufValue_);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(static_cast<size_t>(count) * sizeof(float)),
                    values);
}

void Canvas::setHeatmap(const float* norm, int cols, int rows, float maxNorm) {
    if (!hmProg_ || cols <= 0 || rows <= 0 || maxNorm <= 0.f) return;
    hmCols_ = cols;
    hmRows_ = rows;
    // CPU 侧把归一化密度转成 RGBA8 纹理（R 通道存密度，兼容性最好）。
    std::vector<unsigned char> tex(static_cast<size_t>(cols) * rows * 4);
    const float inv = 1.f / maxNorm;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            float t = norm[static_cast<size_t>(r * cols + c)] * inv;
            if (t < 0.f) t = 0.f;
            if (t > 1.f) t = 1.f;
            const size_t k = (static_cast<size_t>(r) * cols + c) * 4;
            tex[k] = static_cast<unsigned char>(t * 255.f + 0.5f);
            tex[k + 1] = 0;
            tex[k + 2] = 0;
            tex[k + 3] = 255;
        }
    while (glGetError() != 0) {}  // 清空本帧历史累积错误，避免误报
    if (!hmTex_) glGenTextures(1, &hmTex_);
    glBindTexture(GL_TEXTURE_2D, hmTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x2600 /*GL_NEAREST*/);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, 0x2600);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x8058 /*GL_RGBA8*/, cols, rows, 0,
                 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, tex.data());
    glBindTexture(GL_TEXTURE_2D, 0);
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
    glUniform1i(uColorMode_, static_cast<int>(colorMode_));
    glUniform1f(uValueMin_, valMin_);
    glUniform1f(uValueMax_, valMax_);

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

void Canvas::drawHeatmapOverlay(const Camera& cam, int viewW, int viewH,
                                const Rect& chip, float alpha) {
    if (!hmProg_ || !hmTex_ || alpha <= 0.f || viewW <= 0 || viewH <= 0) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(hmProg_);
    glBindVertexArray(hmVao_);
    glActiveTexture(0);
    glBindTexture(GL_TEXTURE_2D, hmTex_);
    glUniform1i(hmUTex_, 0);
    glUniform4f(hmUView_, cam.cx, cam.cy, cam.scale, 0.f);
    glUniform2f(hmUViewport_, static_cast<float>(viewW), static_cast<float>(viewH));
    glUniform4f(hmUChip_, chip.lx, chip.ly, chip.hx, chip.hy);
    glUniform1f(hmUAlpha_, alpha);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
}

}  // namespace viz
