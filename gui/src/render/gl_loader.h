// 极简 OpenGL 函数加载器。
//
// 为什么不用 glad / GLEW：我们只需要约 30 个函数（着色器、VBO、VAO、实例化）。
// glad v2 要跑 Python 生成器，GLEW 要多一个系统包，都比这个文件本身更重。
// ImGui 自带的 loader 也不行——它是那个 .cpp 里的 static，而且不含实例化相关的
// glVertexAttribDivisor / glDrawArraysInstanced（ImGui 用不到）。
//
// 调用 glLoad() 必须在创建 GL 上下文之后。
#pragma once

#include <cstddef>
#include <cstdint>

using GLenum = unsigned int;
using GLbitfield = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

#define GL_FLOAT 0x1406
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_MULTISAMPLE 0x809D

// 这些在 libGL.so 里直接有符号（GL 1.x），不需要 getProcAddress
extern "C" {
void glClear(GLbitfield);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glEnable(GLenum);
void glDisable(GLenum);
void glBlendFunc(GLenum, GLenum);
const unsigned char* glGetString(GLenum);
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
void glFinish();
void glPixelStorei(GLenum, GLint);
GLenum glGetError();
GLboolean glIsEnabled(GLenum);
void glGetIntegerv(GLenum, GLint*);
}

#define GL_RGB 0x1907
#define GL_UNSIGNED_BYTE 0x1401
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_SCISSOR_TEST 0x0C11
#define GL_SCISSOR_BOX 0x0C10
#define GL_VIEWPORT 0x0BA2
#define GL_CURRENT_PROGRAM 0x8B8D

// GL 2.0+ 的函数必须运行时解析
#define SP_GL_FUNCS(X)                                                              \
    X(GLuint, CreateShader, (GLenum))                                               \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))    \
    X(void, CompileShader, (GLuint))                                                \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                  \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                 \
    X(void, DeleteShader, (GLuint))                                                 \
    X(GLuint, CreateProgram, ())                                                    \
    X(void, AttachShader, (GLuint, GLuint))                                         \
    X(void, LinkProgram, (GLuint))                                                  \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                 \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                \
    X(void, UseProgram, (GLuint))                                                   \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                           \
    X(void, Uniform1f, (GLint, GLfloat))                                            \
    X(void, Uniform1i, (GLint, GLint))                                              \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                   \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                 \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*))                           \
    X(void, GenBuffers, (GLsizei, GLuint*))                                         \
    X(void, BindBuffer, (GLenum, GLuint))                                           \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                  \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))             \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                    \
    X(void, BindVertexArray, (GLuint))                                              \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                           \
    X(void, EnableVertexAttribArray, (GLuint))                                      \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void, VertexAttribDivisor, (GLuint, GLuint))                                  \
    X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))

#define SP_DECLARE(ret, name, args) extern ret (*sp_gl##name) args;
SP_GL_FUNCS(SP_DECLARE)
#undef SP_DECLARE

/// 解析全部函数指针。返回 false 表示驱动缺东西（上下文版本太低）。
bool glLoad();

// 用 sp_ 前缀避免与 libGL 可能导出的同名符号打架
#define glCreateShader sp_glCreateShader
#define glShaderSource sp_glShaderSource
#define glCompileShader sp_glCompileShader
#define glGetShaderiv sp_glGetShaderiv
#define glGetShaderInfoLog sp_glGetShaderInfoLog
#define glDeleteShader sp_glDeleteShader
#define glCreateProgram sp_glCreateProgram
#define glAttachShader sp_glAttachShader
#define glLinkProgram sp_glLinkProgram
#define glGetProgramiv sp_glGetProgramiv
#define glGetProgramInfoLog sp_glGetProgramInfoLog
#define glUseProgram sp_glUseProgram
#define glGetUniformLocation sp_glGetUniformLocation
#define glUniform1f sp_glUniform1f
#define glUniform1i sp_glUniform1i
#define glUniform2f sp_glUniform2f
#define glUniform4f sp_glUniform4f
#define glUniform4fv sp_glUniform4fv
#define glGenBuffers sp_glGenBuffers
#define glBindBuffer sp_glBindBuffer
#define glBufferData sp_glBufferData
#define glBufferSubData sp_glBufferSubData
#define glDeleteBuffers sp_glDeleteBuffers
#define glGenVertexArrays sp_glGenVertexArrays
#define glBindVertexArray sp_glBindVertexArray
#define glDeleteVertexArrays sp_glDeleteVertexArrays
#define glEnableVertexAttribArray sp_glEnableVertexAttribArray
#define glVertexAttribPointer sp_glVertexAttribPointer
#define glVertexAttribDivisor sp_glVertexAttribDivisor
#define glDrawArraysInstanced sp_glDrawArraysInstanced
