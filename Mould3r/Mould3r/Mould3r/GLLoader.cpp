#include "GLLoader.h"

#include <wx/glcanvas.h>     // WX_GL_* attribute constants

// wx/glcanvas.h transitively pulls in <windows.h> on Win32, which is what
// gives us wglGetProcAddress / LoadLibraryA / GetProcAddress / HMODULE.
// Including <windows.h> explicitly here would be more defensive but
// duplicates a transitive include the rest of the codebase already
// relies on — left implicit to match the existing convention.

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
        void* p = (void*)wglGetProcAddress(name);
        if (p) return p;
        // Cache the opengl32 module across calls — once loaded the handle
        // stays valid for process lifetime, and re-LoadLibrary'ing on every
        // call would be silly. Local static is thread-safe init in C++11+.
        static HMODULE module = LoadLibraryA("opengl32.dll");
        if (!module) return nullptr;
        return (void*)GetProcAddress(module, name);
    }
}
