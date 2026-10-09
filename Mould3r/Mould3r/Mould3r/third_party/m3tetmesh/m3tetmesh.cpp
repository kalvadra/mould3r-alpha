// m3tetmesh.cpp — see m3tetmesh.h.
#include "m3tetmesh.h"

#include <floattetwild/FloatTetwild.h>
#include <floattetwild/Logger.hpp>
#include <floattetwild/MeshIO.hpp>
#include <floattetwild/Parameters.h>
#include <floattetwild/Types.hpp>

#include <geogram/basic/common.h>
#include <geogram/basic/logger.h>
#include <geogram/mesh/mesh.h>

#ifdef FLOAT_TETWILD_USE_TBB
#include <oneapi/tbb/global_control.h>
#include <igl/default_num_threads.h>
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

namespace
{
    void SetMessage(M3TetResult* out, const char* text)
    {
        if (!out) return;
        std::snprintf(out->message, sizeof(out->message), "%s", text ? text : "");
    }

    // Geogram once per process (it registers global factories).
    void InitGeogramOnce()
    {
        static bool done = false;
        if (done) return;
#ifndef _WIN32
        setenv("GEO_NO_SIGNAL_HANDLER", "1", 1);
#endif
        GEO::initialize();
        GEO::Logger::instance()->set_quiet(true);
        done = true;
    }
} // namespace

extern "C" {

int32_t m3tet_abi_version(void) { return M3TET_ABI_VERSION; }

void m3tet_default_params(M3TetParams* p)
{
    if (!p) return;
    p->edgeLength = 0.0;
    p->epsilon = 0.0;
    p->stopEnergy = 10.0;
    p->maxPasses = 80;
    p->maxThreads = 0;
    p->coarsen = 0;
    p->logLevel = 2;   // info-and-up is chatty; warnings+ by default
}

int32_t m3tet_mesh(const double* verts, int32_t nVerts, const int32_t* tris, int32_t nTris,
                   const M3TetParams* params, M3TetResult* out)
{
    if (!out) return M3TET_BAD_INPUT;
    std::memset(out, 0, sizeof(*out));
    if (!verts || !tris || nVerts < 4 || nTris < 4)
    {
        SetMessage(out, "Input surface is empty or too small to enclose a volume.");
        return M3TET_BAD_INPUT;
    }
    for (int32_t i = 0; i < 3 * nTris; ++i)
        if (tris[i] < 0 || tris[i] >= nVerts)
        {
            SetMessage(out, "Input triangle references a vertex out of range.");
            return M3TET_BAD_INPUT;
        }

    M3TetParams P;
    m3tet_default_params(&P);
    if (params) P = *params;

    try
    {
        InitGeogramOnce();

        using namespace floatTetWild;
        Logger::init(/*use_cout=*/P.logLevel < 6, "");
        spdlog::set_level(static_cast<spdlog::level::level_enum>(std::clamp(P.logLevel, 0, 6)));

        Parameters prm;
        prm.is_quiet = P.logLevel >= 6;
        prm.log_level = std::clamp(P.logLevel, 0, 6);
        prm.stop_energy = P.stopEnergy > 0.0 ? P.stopEnergy : prm.stop_energy;
        prm.max_its = P.maxPasses > 0 ? P.maxPasses : prm.max_its;
        prm.coarsen = P.coarsen != 0;
        if (P.edgeLength > 0.0) prm.ideal_edge_length_abs = P.edgeLength;

        // The envelope is relative to the bounding-box diagonal in fTetWild.
        double mn[3] = { verts[0], verts[1], verts[2] }, mx[3] = { verts[0], verts[1], verts[2] };
        for (int32_t i = 1; i < nVerts; ++i)
            for (int k = 0; k < 3; ++k)
            {
                mn[k] = std::min(mn[k], verts[3 * i + k]);
                mx[k] = std::max(mx[k], verts[3 * i + k]);
            }
        const double diag = std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                                      (mx[2] - mn[2]) * (mx[2] - mn[2]));
        if (!(diag > 0.0))
        {
            SetMessage(out, "Input surface has no extent.");
            return M3TET_BAD_INPUT;
        }
        if (P.epsilon > 0.0) prm.eps_rel = P.epsilon / diag;

#ifdef FLOAT_TETWILD_USE_TBB
        unsigned int threads = std::max(1u, std::thread::hardware_concurrency());
        if (P.maxThreads > 0) threads = std::min<unsigned int>(threads, (unsigned int)P.maxThreads);
        prm.num_threads = threads;
        tbb::global_control parallelism(tbb::global_control::max_allowed_parallelism, threads);
        tbb::global_control stack(tbb::global_control::thread_stack_size, 64u * 1024u * 1024u);
        igl::default_num_threads((unsigned int)std::ceil(std::sqrt((double)threads)));
#else
        prm.num_threads = 1;
#endif

        std::vector<Vector3> points((size_t)nVerts);
        for (int32_t i = 0; i < nVerts; ++i)
            points[(size_t)i] << verts[3 * i], verts[3 * i + 1], verts[3 * i + 2];
        std::vector<Vector3i> faces((size_t)nTris);
        for (int32_t i = 0; i < nTris; ++i)
            faces[(size_t)i] << tris[3 * i], tris[3 * i + 1], tris[3 * i + 2];
        std::vector<int> tags(faces.size(), 0);

        GEO::Mesh sf;
        MeshIO::load_mesh(points, faces, sf, tags);

        Eigen::MatrixXd VO;
        Eigen::MatrixXi TO;
        const int rc = tetrahedralization(sf, prm, VO, TO);
        if (rc != 0)
        {
            SetMessage(out, "fTetWild failed to tetrahedralise the surface.");
            return M3TET_FAILED;
        }
        if (TO.rows() == 0)
        {
            SetMessage(out, "No tetrahedra lie inside the surface (is it closed?).");
            return M3TET_EMPTY_RESULT;
        }

        out->nVerts = (int32_t)VO.rows();
        out->nTets = (int32_t)TO.rows();
        out->verts = (double*)std::malloc(sizeof(double) * 3 * (size_t)out->nVerts);
        out->tets = (int32_t*)std::malloc(sizeof(int32_t) * 4 * (size_t)out->nTets);
        if (!out->verts || !out->tets)
        {
            m3tet_free(out);
            SetMessage(out, "Out of memory copying the tet mesh.");
            return M3TET_FAILED;
        }
        for (int32_t i = 0; i < out->nVerts; ++i)
            for (int k = 0; k < 3; ++k) out->verts[3 * i + k] = VO(i, k);
        // Positive orientation: (b-a) . ((c-a) x (d-a)) > 0. fTetWild's own
        // convention is the opposite, so check each tet and swap two corners
        // where needed (per tet, so a mixed output is also normalised).
        int32_t flipped = 0;
        for (int32_t i = 0; i < out->nTets; ++i)
        {
            int32_t* t = out->tets + 4 * i;
            for (int k = 0; k < 4; ++k) t[k] = (int32_t)TO(i, k);
            const double* a = out->verts + 3 * t[0];
            const double* b = out->verts + 3 * t[1];
            const double* c = out->verts + 3 * t[2];
            const double* d = out->verts + 3 * t[3];
            const double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const double v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            const double w[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
            const double det = u[0] * (v[1] * w[2] - v[2] * w[1]) - u[1] * (v[0] * w[2] - v[2] * w[0]) +
                               u[2] * (v[0] * w[1] - v[1] * w[0]);
            if (det < 0.0) { std::swap(t[2], t[3]); ++flipped; }
        }

        const double edge = P.edgeLength > 0.0 ? P.edgeLength : diag * prm.ideal_edge_length_rel;
        const double eps = P.epsilon > 0.0 ? P.epsilon : diag * prm.eps_rel;
        std::snprintf(out->message, sizeof(out->message), "%d tets, %d vertices (target edge %.4g, envelope %.4g)",
                      out->nTets, out->nVerts, edge, eps);
        (void)flipped;
        return M3TET_OK;
    }
    catch (const std::exception& e)
    {
        m3tet_free(out);
        std::snprintf(out->message, sizeof(out->message), "fTetWild exception: %s", e.what());
        return M3TET_EXCEPTION;
    }
    catch (...)
    {
        m3tet_free(out);
        SetMessage(out, "fTetWild threw an unknown exception.");
        return M3TET_EXCEPTION;
    }
}

void m3tet_free(M3TetResult* r)
{
    if (!r) return;
    std::free(r->verts);
    std::free(r->tets);
    r->verts = nullptr;
    r->tets = nullptr;
    r->nVerts = 0;
    r->nTets = 0;
}

} // extern "C"
