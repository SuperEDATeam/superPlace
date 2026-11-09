#include "render/canvas.h"
#include "render/cull.h"

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

// 连续色表：viridis 5 控制点（shader 与 CPU legend 共用，见 app.cpp 色条）。
const char* kCmapGLSL = R"(
vec3 cmap(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 c0 = vec3(0.267, 0.005, 0.329);
    vec3 c1 = vec3(0.229, 0.322, 0.545);
    vec3 c2 = vec3(0.127, 0.566, 0.550);
    vec3 c3 = vec3(0.369, 0.789, 0.383);
    vec3 c4 = vec3(0.992, 0.906, 0.144);
    if (t < 0.25) return mix(c0, c1, t * 4.0);
    if (t < 0.5)  return mix(c1, c2, (t - 0.25) * 4.0);
    if (t < 0.75) return mix(c2, c3, (t - 0.5) * 4.0);
    return mix(c3, c4, (t - 0.75) * 4.0);
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
    halfW_.assign(n, 0.f);
    halfH_.assign(n, 0.f);
    geomKind_.assign(n, 0.f);
    for (size_t i = 0; i < n; ++i) {
        const bool skip = (geom.flags[i] & F_NI) != 0 || geom.w[i] <= 0.f || geom.h[i] <= 0.f;
        size[i * 2] = skip ? 0.f : geom.w[i];
        size[i * 2 + 1] = skip ? 0.f : geom.h[i];
        kind[i] = static_cast<float>(geom.kind[i]);
        halfW_[i] = skip ? 0.f : geom.w[i] * 0.5f;
        halfH_[i] = skip ? 0.f : geom.h[i] * 0.5f;
        geomKind_[i] = kind[i];
    }
    staticSizeBuf_ = std::move(size);
    staticKindBuf_ = std::move(kind);
    activeCenter_.assign(n * 2, 0.f);
    activeValue_.assign(n, 0.f);
    activeIndex_.assign(n, 0);
    cachedActiveIndex_.assign(n, 0);
    fullMode_ = false;
    mappingDirty_ = true;
    geomDirty_ = true;
    activeCount_ = 0;

    glBindBuffer(GL_ARRAY_BUFFER, bufSize_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * 2 * sizeof(float)),
                 staticSizeBuf_.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, bufKind_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(float)),
                 staticKindBuf_.data(), GL_STATIC_DRAW);
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
    if (!cullEnabled_) {
        // fastpath：全量上传，与旧版一致。
        glBindBuffer(GL_ARRAY_BUFFER, bufCenter_);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(interleaved_.size() * sizeof(float)),
                        interleaved_.data());
    }
    geomDirty_ = true;
    // positions 更新不使 mapping（视口/隐藏层）失效。
    uploadMs_ = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
}

void Canvas::setContinuousValues(const float* values, int count) {
    if (count != count_ || count_ <= 0 || !values) return;
    std::copy(values, values + count, values_.begin());
    geomDirty_ = true;
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

int Canvas::rebuildActive(float*, bool geomUpload) {
    if (count_ <= 0) return 0;
    // 每帧都按当前位置重新判定可见集（offscreen→on-viewport 不能漏）。
    const float halfW = curViewW_ * 0.5f / curCam_.scale;
    const float halfH = curViewH_ * 0.5f / curCam_.scale;
    const float pxPad = minPixelSize * 0.5f / curCam_.scale;
    float x0 = curCam_.cx - halfW - pxPad, x1 = curCam_.cx + halfW + pxPad;
    float y0 = curCam_.cy - halfH - pxPad, y1 = curCam_.cy + halfH + pxPad;
    // 复用 cull.h 纯函数（与单测同一 predicate）。
    // 先把 interleaved 转成纯 cx/cy 数组喂给 helper，避免复制两份公式。
    tmpCx_.assign(count_, 0.f); tmpCy_.assign(count_, 0.f);
    for (int i = 0; i < count_; ++i) {
        tmpCx_[i] = interleaved_[i * 2];
        tmpCy_[i] = interleaved_[i * 2 + 1];
    }
    std::vector<int> idx = cullVisible(tmpCx_.data(), tmpCy_.data(),
                                       halfW_.data(), halfH_.data(),
                                       geomKind_.data(), count_, layerVisible,
                                       x0, x1, y0, y1);
    int n = (int)idx.size();
    for (int j = 0; j < n; ++j) {
        int i = idx[j];
        activeCenter_[j * 2] = tmpCx_[i]; activeCenter_[j * 2 + 1] = tmpCy_[i];
        activeValue_[j] = values_[i];
        activeIndex_[j] = i;
    }
    // 精确比对 activeIndex 序列（与上一帧缓存），一致才省 size/kind 上传。
    bool setChanged = geomUpload || n != activeCount_;
    if (!setChanged) {
        for (int j = 0; j < n; ++j)
            if (activeIndex_[j] != cachedActiveIndex_[j]) { setChanged = true; break; }
    }
    if (setChanged) std::copy(activeIndex_.begin(), activeIndex_.begin() + n, cachedActiveIndex_.begin());
    // 视口覆盖全部节点：进入 fullMode，后续帧跳过筛选。
    fullMode_ = (n == count_);
    if (setChanged) {
        tmpSize_.assign(n * 2, 0.f);
        tmpKind_.assign(n, 0.f);
        for (int j = 0; j < n; ++j) {
            int i = activeIndex_[j];
            tmpSize_[j * 2] = staticSizeBuf_[i * 2];
            tmpSize_[j * 2 + 1] = staticSizeBuf_[i * 2 + 1];
            tmpKind_[j] = staticKindBuf_[i];
        }
        glBindBuffer(GL_ARRAY_BUFFER, bufSize_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, n * 2 * sizeof(float), tmpSize_.data());
        glBindBuffer(GL_ARRAY_BUFFER, bufKind_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, n * sizeof(float), tmpKind_.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, bufCenter_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * 2 * sizeof(float), activeCenter_.data());
    if (valueActive_) {
        glBindBuffer(GL_ARRAY_BUFFER, bufValue_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, n * sizeof(float), activeValue_.data());
    }
    return n;
}

void Canvas::draw(const Camera& cam, int viewW, int viewH) {
    if (count_ <= 0 || viewW <= 0 || viewH <= 0) return;
    int active = count_;
    if (cullEnabled_) {
        bool camChanged = cam.cx != curCam_.cx || cam.cy != curCam_.cy ||
            cam.scale != curCam_.scale || viewW != curViewW_ || viewH != curViewH_;
        if (camChanged) { curCam_ = cam; curViewW_ = viewW; curViewH_ = viewH; mappingDirty_ = true; fullMode_ = false; }
        if (fullMode_ && !mappingDirty_) {
            // 全视口覆盖：直接全量上传 center，跳过筛选。
            glBindBuffer(GL_ARRAY_BUFFER, bufCenter_);
            glBufferSubData(GL_ARRAY_BUFFER, 0, interleaved_.size() * sizeof(float), interleaved_.data());
            if (valueActive_) {
                glBindBuffer(GL_ARRAY_BUFFER, bufValue_);
                glBufferSubData(GL_ARRAY_BUFFER, 0, values_.size() * sizeof(float), values_.data());
            }
            active = count_;
        } else {
            if (geomDirty_ || mappingDirty_) {
                activeCount_ = rebuildActive(nullptr, mappingDirty_);
                geomDirty_ = false; mappingDirty_ = false;
            }
            active = activeCount_;
        }
    } else {
        activeCount_ = count_;
        active = count_;
    }
    if (active <= 0) return;

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

    float cols[16];
    for (int k = 0; k < 4; ++k) {
        for (int c = 0; c < 3; ++c) cols[k * 4 + c] = kColors[k][c];
        cols[k * 4 + 3] = layerVisible[k] ? kColors[k][3] : 0.f;
    }
    glUniform4fv(uColors_, 4, cols);

    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, active);
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
    glActiveTexture(GL_TEXTURE0);
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
