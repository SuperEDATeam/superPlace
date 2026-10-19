#include "render/gl_loader.h"

#include <cstdio>

#include <GLFW/glfw3.h>

#define SP_DEFINE(ret, name, args) ret (*sp_gl##name) args = nullptr;
SP_GL_FUNCS(SP_DEFINE)
#undef SP_DEFINE

bool glLoad() {
    bool ok = true;
#define SP_LOAD(ret, name, args)                                                 \
    sp_gl##name = reinterpret_cast<ret (*) args>(glfwGetProcAddress("gl" #name)); \
    if (!sp_gl##name) {                                                          \
        std::fprintf(stderr, "[gl] 缺少入口 gl%s\n", #name);                     \
        ok = false;                                                              \
    }
    SP_GL_FUNCS(SP_LOAD)
#undef SP_LOAD
    return ok;
}
