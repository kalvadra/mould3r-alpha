#pragma once
// ===========================================================================
// m3tetmesh — Mould3r's narrow C interface to fTetWild (volume tet meshing).
//
// Built as its own DLL (see CMakeLists.txt / build_windows.bat) so fTetWild,
// its CMake dependency tree (libigl, Geogram, spdlog, fmt, oneTBB, mini-gmp) and
// its MPL-2.0 sources stay out of the Mould3r project. Mould3r delay-loads the
// DLL and only calls it from the `--mesh-worker` process (MeshWorker.cpp), so
// the main app never loads it, and a crash or runaway run inside fTetWild can
// only ever take down the worker.
//
// Plain C types only across the boundary (no Eigen / STL / exceptions), so the
// DLL and the app don't have to agree on anything but this header.
//
// fTetWild keeps global state (statistics, logger), so one mesh per PROCESS:
// call m3tet_mesh once per worker run.
// ===========================================================================

#include <stdint.h>

#if defined(_WIN32)
#  if defined(M3TETMESH_BUILD)
#    define M3TET_API __declspec(dllexport)
#  else
#    define M3TET_API __declspec(dllimport)
#  endif
#else
#  define M3TET_API __attribute__((visibility("default")))
#endif

// Bumped whenever a struct below changes layout; the app refuses a DLL whose
// m3tet_abi_version() differs from the one it was compiled against.
#define M3TET_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct M3TetParams
{
    double edgeLength;   // target tet edge length, input units (mm); <= 0 -> fTetWild default (bbox diag / 20)
    double epsilon;      // surface envelope, input units (mm);       <= 0 -> fTetWild default (bbox diag / 1000)
    double stopEnergy;   // optimisation stops once the worst tet energy is below this (fTetWild default 10)
    int32_t maxPasses;   // optimisation passes cap (fTetWild default 80)
    int32_t maxThreads;  // 0 = all hardware threads
    int32_t coarsen;     // 1 = coarsen the output where the envelope allows
    int32_t logLevel;    // spdlog level 0 (trace) .. 6 (off)
} M3TetParams;

typedef struct M3TetResult
{
    int32_t  nVerts;
    double*  verts;      // 3 * nVerts, xyz
    int32_t  nTets;
    int32_t* tets;       // 4 * nTets, vertex indices (positive orientation)
    char     message[512];
} M3TetResult;

// Status codes returned by m3tet_mesh.
enum
{
    M3TET_OK            = 0,
    M3TET_BAD_INPUT     = 1,   // empty / malformed arrays
    M3TET_FAILED        = 2,   // fTetWild returned an error
    M3TET_EMPTY_RESULT  = 3,   // ran, but no tets were inside the surface
    M3TET_EXCEPTION     = 4    // a C++ exception escaped fTetWild (message has what())
};

M3TET_API int32_t m3tet_abi_version(void);
M3TET_API void    m3tet_default_params(M3TetParams* p);

// Tetrahedralise the volume enclosed by a triangle surface (any "triangle
// soup": duplicate vertices, small gaps and self-intersections are tolerated).
// On M3TET_OK the result arrays are allocated and must be released with
// m3tet_free. `message` always holds a one-line summary or the error.
M3TET_API int32_t m3tet_mesh(const double* verts, int32_t nVerts,
                             const int32_t* tris, int32_t nTris,
                             const M3TetParams* params, M3TetResult* out);

M3TET_API void    m3tet_free(M3TetResult* r);

#ifdef __cplusplus
}
#endif
