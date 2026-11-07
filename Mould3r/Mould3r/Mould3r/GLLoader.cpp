#include "GLLoader.h"

#include <wx/glcanvas.h>     // WX_GL_* attribute constants

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#else
#include <GL/glx.h>
#endif

namespace GLLoader
{
    int glArgs[] = {
        WX_GL_RGBA, WX_GL_DOUBLEBUFFER,
        WX_GL_DEPTH_SIZE, 24,
        WX_GL_STENCIL_SIZE, 8,
        WX_GL_SAMPLE_BUFFERS, 1,
        WX_GL_SAMPLES, 4,
        0
    };

    void* GetAnyGLFuncAddress(const char* name)
    {
#ifdef __WXMSW__
        void* p = (void*)wglGetProcAddress(name);
        if (p && p != reinterpret_cast<void*>(1) &&
            p != reinterpret_cast<void*>(2) && p != reinterpret_cast<void*>(3) &&
            p != reinterpret_cast<void*>(-1)) return p;
        // Cache the opengl32 module across calls — once loaded the handle
        // stays valid for process lifetime, and re-LoadLibrary'ing on every
        // call would be silly. Local static is thread-safe init in C++11+.
        static HMODULE module = LoadLibraryA("opengl32.dll");
        if (!module) return nullptr;
        return (void*)GetProcAddress(module, name);
#elif defined(__APPLE__)
        // Load only the system framework; retain its handle for glad's pointers.
        static void* framework = dlopen(
            "/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
        return framework ? dlsym(framework, name) : nullptr;
#else
        return reinterpret_cast<void*>(glXGetProcAddressARB(
            reinterpret_cast<const GLubyte*>(name)));
#endif
    }
}
