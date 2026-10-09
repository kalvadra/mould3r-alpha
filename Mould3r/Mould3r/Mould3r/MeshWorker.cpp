// MeshWorker.cpp — see MeshWorker.h. No wx in this file: the worker runs
// before (and instead of) any UI.
#include "MeshWorker.h"
#include "TetMeshJob.h"

#include <chrono>
#include <cstring>
#include <string>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <crtdbg.h>
#  include <cstdlib>
#endif

#ifdef M3_HAVE_TETMESH
#  include "m3tetmesh.h"
#endif

namespace
{
#ifdef _WIN32
    // Last resort for anything that escapes: end the process with the
    // exception code (the app reports it) instead of a system crash dialog.
    LONG WINAPI ExitOnUnhandledException(EXCEPTION_POINTERS* info)
    {
        const DWORD code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0xE0000001u;
        TerminateProcess(GetCurrentProcess(), code);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void SilenceCrashDialogs()
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        SetUnhandledExceptionFilter(ExitOnUnhandledException);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);   // abort(): no dialog, no WER report
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);             // debug CRT: assert/error to the
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);              // debugger, not a message box
    }
#endif

#ifdef M3_HAVE_TETMESH
#  ifdef _WIN32
    // m3tetmesh.dll is delay-loaded. Load it explicitly from the executable's
    // own folder first, so a missing / broken / wrong-version DLL becomes a
    // clean error message instead of a delay-load exception on first call.
    // (The delay-load stubs then bind to this already-loaded module.)
    bool LoadMesherDll(std::string& error)
    {
        wchar_t exe[4096];
        const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
        if (n == 0 || n >= sizeof(exe) / sizeof(exe[0]))
        {
            error = "Could not locate Mould3r.exe to find the 3D mesher.";
            return false;
        }
        const std::filesystem::path dll = std::filesystem::path(exe).parent_path() / L"m3tetmesh.dll";
        HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h)
        {
            const DWORD e = GetLastError();
            error = (e == ERROR_MOD_NOT_FOUND)
                ? "The 3D mesher (m3tetmesh.dll) is missing from Mould3r's folder."
                : "The 3D mesher (m3tetmesh.dll) could not be loaded (Windows error " + std::to_string(e) + ").";
            return false;
        }
        using AbiFn = int32_t (*)();
        const auto abi = reinterpret_cast<AbiFn>(reinterpret_cast<void*>(GetProcAddress(h, "m3tet_abi_version")));
        if (!abi || abi() != M3TET_ABI_VERSION)
        {
            error = "m3tetmesh.dll is from a different version of Mould3r - rebuild it "
                    "(third_party/m3tetmesh/build_windows.bat).";
            return false;
        }
        return true;
    }
#  endif

    void RunMesher(const TetMesh::Surface& s, const TetMesh::Params& p, TetMesh::JobResult& r)
    {
#  ifdef _WIN32
        std::string error;
        if (!LoadMesherDll(error))
        {
            r.status = TetMesh::StatusMesherMissing;
            r.message = error;
            return;
        }
#  endif
        M3TetParams mp;
        m3tet_default_params(&mp);
        mp.edgeLength = p.edgeLength;
        mp.epsilon = p.epsilon;
        mp.stopEnergy = p.stopEnergy;
        mp.maxPasses = p.maxPasses;
        mp.maxThreads = p.maxThreads;
        mp.coarsen = p.coarsen;
        mp.logLevel = 6;   // quiet: nobody reads the worker's console

        M3TetResult res;
        std::memset(&res, 0, sizeof(res));
        const auto t0 = std::chrono::steady_clock::now();
        const int32_t rc = m3tet_mesh(s.verts.data(), (int32_t)s.VertexCount(), s.tris.data(),
                                      (int32_t)s.TriangleCount(), &mp, &res);
        r.workerSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        r.status = rc;
        r.message = res.message;
        if (rc == M3TET_OK)
        {
            r.mesh.verts.assign(res.verts, res.verts + 3 * (size_t)res.nVerts);
            r.mesh.tets.assign(res.tets, res.tets + 4 * (size_t)res.nTets);
        }
        m3tet_free(&res);
    }
#endif
} // namespace

int RunMeshWorker(const std::filesystem::path& inPath, const std::filesystem::path& outPath)
{
#ifdef _WIN32
    SilenceCrashDialogs();
#endif
    TetMesh::JobResult r;
    TetMesh::Surface s;
    TetMesh::Params p;
    std::string error;

    if (!TetMesh::ReadJobInput(inPath, s, p, error))
    {
        r.status = TetMesh::StatusBadJobFile;
        r.message = error;
    }
    else
    {
#ifdef M3_HAVE_TETMESH
        RunMesher(s, p, r);
#else
        r.status = TetMesh::StatusMesherMissing;
        r.message = "This build of Mould3r doesn't include the 3D mesher: run "
                    "third_party/m3tetmesh/build_windows.bat, then rebuild Mould3r.";
#endif
    }

    if (!TetMesh::WriteJobOutput(outPath, r, error))
        return TetMesh::StatusBadJobFile;
    return r.status;
}
