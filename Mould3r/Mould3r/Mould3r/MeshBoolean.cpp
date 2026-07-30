#include "MeshBoolean.h"

#include <manifold/manifold.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace {

using manifold::Manifold;
using manifold::MeshGL;

// ---------------------------------------------------------------------------
// Conversions between our flat Mesh and Manifold's MeshGL (numProp == 3, i.e.
// position only — we carry no per-vertex properties across the boolean).
// ---------------------------------------------------------------------------

MeshGL ToMeshGL(const MeshBoolean::Mesh& m)
{
    MeshGL gl;
    gl.numProp = 3;                 // x,y,z only
    gl.vertProperties = m.verts;    // float positions, 3 per vertex
    gl.triVerts = m.indices;        // uint32 triples
    return gl;
}

MeshBoolean::Mesh FromMeshGL(const MeshGL& gl)
{
    MeshBoolean::Mesh out;
    const uint32_t np = gl.numProp > 0 ? gl.numProp : 3;   // first 3 are position
    const size_t   nv = gl.vertProperties.size() / np;

    out.verts.reserve(nv * 3);
    for (size_t v = 0; v < nv; ++v)
    {
        const size_t base = v * np;
        out.verts.push_back(gl.vertProperties[base + 0]);
        out.verts.push_back(gl.vertProperties[base + 1]);
        out.verts.push_back(gl.vertProperties[base + 2]);
    }
    out.indices = gl.triVerts;
    return out;
}

// A merge tolerance keyed to the mesh scale. FileImporter already dedups STL
// vertices on a 1e-5 quantum, so most input arrives welded; this catches
// residual near-coincident verts (and lets Merge collapse slivers) without
// being large enough to fuse genuinely distinct geometry.
float MergeTolerance(const MeshBoolean::Mesh& m)
{
    float lo[3] = {  std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max() };
    float hi[3] = { -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max() };
    const size_t nv = m.verts.size() / 3;
    for (size_t v = 0; v < nv; ++v)
        for (int k = 0; k < 3; ++k)
        {
            const float c = m.verts[v * 3 + k];
            lo[k] = std::min(lo[k], c);
            hi[k] = std::max(hi[k], c);
        }
    const float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    const float diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    return diag > 0.0f ? diag * 1.0e-6f : 0.0f;
}

// Build a Manifold from a mesh, welding coincident vertices first so lightly
// non-manifold input (residual duplicate verts, degenerate triangles) still has
// a chance. `mergedOut` reports whether Merge() actually changed anything.
// `tolScale` widens the weld tolerance (1.0 = the standard scale-relative
// epsilon); the Continue repair uses a larger scale to bridge bigger cracks.
Manifold BuildWelded(const MeshBoolean::Mesh& m, bool& mergedOut, float tolScale = 1.0f)
{
    MeshGL gl = ToMeshGL(m);
    gl.tolerance = MergeTolerance(m) * tolScale;
    mergedOut = gl.Merge();     // recompute merge vectors; true if it welded/collapsed
    return Manifold(gl);
}

const char* BasicShapeReason(const MeshBoolean::Mesh& m)
{
    if (m.verts.empty() || m.indices.empty()) return "mesh is empty";
    if (m.verts.size() % 3 != 0)              return "vertex buffer is not a multiple of 3";
    if (m.indices.size() % 3 != 0)            return "index buffer is not a multiple of 3";
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace MeshBoolean {

RepairResult ValidateAndRepair(const Mesh& in)
{
    RepairResult r;

    if (const char* why = BasicShapeReason(in))
    {
        r.message = std::string("Not a usable mesh: ") + why + ".";
        return r;
    }

    // Pass 1: standard weld. If the mesh is a solid after merging coincident
    // vertices, we're done.
    bool merged = false;
    Manifold man = BuildWelded(in, merged);

    // Pass 2 (the "simple repair"): if the standard weld left holes, widen the
    // tolerance to bridge small cracks (e.g. an STL whose seam verts don't quite
    // coincide). Kept modest so it closes seams without fusing real features;
    // this is the not-guaranteed repair the user opts into via Continue.
    bool aggressive = false;
    if (man.Status() != Manifold::Error::NoError || man.IsEmpty())
    {
        bool merged2 = false;
        Manifold man2 = BuildWelded(in, merged2, 100.0f);   // ~diag * 1e-4
        if (man2.Status() == Manifold::Error::NoError && !man2.IsEmpty())
        {
            man = man2;
            merged = merged2;
            aggressive = true;
        }
    }

    if (man.Status() != Manifold::Error::NoError)
    {
        std::ostringstream os;
        os << "Mesh is not a watertight solid (Manifold error code "
           << static_cast<int>(man.Status())
           << ") even after welding coincident vertices.";
        r.message = os.str();
        return r;
    }
    if (man.IsEmpty())
    {
        r.message = "Mesh collapsed to nothing after cleanup.";
        return r;
    }

    r.ok = true;
    r.wasRepaired = merged || aggressive;
    r.mesh = FromMeshGL(man.GetMeshGL());
    r.message = aggressive
        ? "Repaired: welded near-coincident vertices (widened tolerance) into a watertight solid."
        : (merged ? "Repaired: welded coincident vertices into a watertight solid."
                  : "Mesh is a watertight solid.");
    return r;
}

bool IsManifold(const Mesh& in)
{
    if (BasicShapeReason(in)) return false;
    bool merged = false;
    Manifold man = BuildWelded(in, merged);
    // A mesh that becomes a valid solid after welding coincident vertices IS
    // watertight — welding duplicated/coincident verts is normalization, not a
    // repair. (STL is triangle soup; even our own clean exports need a weld on
    // the way back in.) Only genuine defects that survive the weld — holes,
    // non-manifold edges — leave Status != NoError, which is what should
    // trigger the import warning.
    return man.Status() == Manifold::Error::NoError && !man.IsEmpty();
}

bool Difference(const Mesh& minuend, const Mesh& subtrahend,
                Mesh& out, std::string& error)
{
    out = Mesh{};
    error.clear();

    if (const char* why = BasicShapeReason(minuend))
    {
        error = std::string("Minuend unusable: ") + why + ".";
        return false;
    }
    if (const char* why = BasicShapeReason(subtrahend))
    {
        error = std::string("Subtrahend unusable: ") + why + ".";
        return false;
    }

    bool ma = false, mb = false;
    Manifold a = BuildWelded(minuend, ma);
    Manifold b = BuildWelded(subtrahend, mb);

    if (a.Status() != Manifold::Error::NoError)
    {
        std::ostringstream os;
        os << "Minuend is not a watertight solid (error code "
           << static_cast<int>(a.Status()) << ").";
        error = os.str();
        return false;
    }
    if (b.Status() != Manifold::Error::NoError)
    {
        std::ostringstream os;
        os << "Subtrahend is not a watertight solid (error code "
           << static_cast<int>(b.Status()) << ").";
        error = os.str();
        return false;
    }

    Manifold result = a - b;

    if (result.Status() != Manifold::Error::NoError)
    {
        std::ostringstream os;
        os << "Boolean difference failed (error code "
           << static_cast<int>(result.Status()) << ").";
        error = os.str();
        return false;
    }
    if (result.IsEmpty())
    {
        error = "Boolean difference produced an empty solid "
                "(the subtrahend may fully enclose the minuend).";
        return false;
    }

    out = FromMeshGL(result.GetMeshGL());
    return true;
}

bool Union(const std::vector<Mesh>& parts, Mesh& out, std::string& error)
{
    out = Mesh{};
    error.clear();

    std::vector<Manifold> solids;
    solids.reserve(parts.size());
    for (const Mesh& p : parts)
    {
        if (BasicShapeReason(p)) continue;
        bool merged = false;
        Manifold m = BuildWelded(p, merged);
        if (m.Status() == Manifold::Error::NoError && !m.IsEmpty())
            solids.push_back(std::move(m));
    }

    if (solids.empty())
    {
        error = "No valid solids to union.";
        return false;
    }

    Manifold acc = solids[0];
    for (size_t i = 1; i < solids.size(); ++i)
        acc = acc + solids[i];          // Manifold boolean union

    if (acc.Status() != Manifold::Error::NoError || acc.IsEmpty())
    {
        error = "Union produced no valid solid.";
        return false;
    }

    out = FromMeshGL(acc.GetMeshGL());
    return true;
}

double Volume(const Mesh& mesh)
{
    if (mesh.indices.size() < 3 || mesh.verts.size() < 9) return 0.0;
    const size_t nv = mesh.verts.size() / 3;

    auto P = [&](uint32_t idx, int k) -> double {
        return (double)mesh.verts[(size_t)idx * 3 + (size_t)k];
    };

    // Sum of signed tetrahedra (origin, v0, v1, v2): V = (1/6) Σ v0·(v1×v2).
    double v6 = 0.0;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
    {
        const uint32_t i0 = mesh.indices[t + 0];
        const uint32_t i1 = mesh.indices[t + 1];
        const uint32_t i2 = mesh.indices[t + 2];
        if (i0 >= nv || i1 >= nv || i2 >= nv) continue;

        const double ax = P(i0, 0), ay = P(i0, 1), az = P(i0, 2);
        const double bx = P(i1, 0), by = P(i1, 1), bz = P(i1, 2);
        const double cx = P(i2, 0), cy = P(i2, 1), cz = P(i2, 2);

        const double crx = by * cz - bz * cy;
        const double cry = bz * cx - bx * cz;
        const double crz = bx * cy - by * cx;
        v6 += ax * crx + ay * cry + az * crz;
    }
    return std::abs(v6) / 6.0;
}

// ---------------------------------------------------------------------------
// SelfTest — a hand-built cube minus an overlapping hand-built cube. Both cubes
// are indexed with shared corners and consistent CCW-outward winding, so they
// arrive already manifold; the test proves the whole pipeline end to end and
// that the carved result re-validates as a solid.
// ---------------------------------------------------------------------------
bool SelfTest(std::string& report)
{
    auto makeCube = [](float cx, float cy, float cz, float h) -> Mesh
    {
        Mesh m;
        // 8 corners of an axis-aligned cube centred at (cx,cy,cz), half-size h.
        const float xs[2] = { cx - h, cx + h };
        const float ys[2] = { cy - h, cy + h };
        const float zs[2] = { cz - h, cz + h };
        // corner order: bit0=x, bit1=y, bit2=z
        for (int i = 0; i < 8; ++i)
        {
            m.verts.push_back(xs[(i >> 0) & 1]);
            m.verts.push_back(ys[(i >> 1) & 1]);
            m.verts.push_back(zs[(i >> 2) & 1]);
        }
        auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d)
        {
            m.indices.push_back(a); m.indices.push_back(b); m.indices.push_back(c);
            m.indices.push_back(a); m.indices.push_back(c); m.indices.push_back(d);
        };
        // Six faces, wound CCW as seen from outside.
        quad(0, 2, 6, 4);   // -x
        quad(1, 5, 7, 3);   // +x
        quad(0, 4, 5, 1);   // -y
        quad(2, 3, 7, 6);   // +y
        quad(0, 1, 3, 2);   // -z
        quad(4, 6, 7, 5);   // +z
        return m;
    };

    std::ostringstream os;
    bool pass = true;

    Mesh a = makeCube(0.0f, 0.0f, 0.0f, 5.0f);           // 10mm cube at origin
    Mesh b = makeCube(5.0f, 5.0f, 5.0f, 5.0f);           // overlaps one corner

    os << "input A: " << a.vertCount() << " verts, " << a.triCount() << " tris\n";
    os << "input B: " << b.vertCount() << " verts, " << b.triCount() << " tris\n";

    RepairResult va = ValidateAndRepair(a);
    os << "validate A: " << (va.ok ? "OK" : "FAIL") << " — " << va.message << "\n";
    pass = pass && va.ok;

    Mesh diff;
    std::string err;
    const bool okDiff = Difference(a, b, diff, err);
    os << "A - B: " << (okDiff ? "OK" : "FAIL");
    if (!okDiff) os << " — " << err;
    os << "\n";
    pass = pass && okDiff;

    if (okDiff)
    {
        os << "result: " << diff.vertCount() << " verts, " << diff.triCount() << " tris\n";
        RepairResult vr = ValidateAndRepair(diff);
        os << "re-validate result: " << (vr.ok ? "OK" : "FAIL") << " — " << vr.message << "\n";
        pass = pass && vr.ok && diff.triCount() > 0;
    }

    os << (pass ? "SELF-TEST PASSED" : "SELF-TEST FAILED");
    report = os.str();
    return pass;
}

} // namespace MeshBoolean
