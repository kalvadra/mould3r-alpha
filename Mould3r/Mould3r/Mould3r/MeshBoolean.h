#pragma once
//
// MeshBoolean — thin wrapper over the Manifold library for the mesh toolpath.
//
// The rest of Mould3r speaks in flat position buffers ([x,y,z] per vertex) plus
// uint32 triangle indices — exactly the shape of SceneObject::cpuVerts /
// cpuIndices, GLCanvas preview meshes, and FileImporter output. This module is
// the only place that knows about manifold::MeshGL / manifold::Manifold, so the
// dependency stays contained here and callers never include a Manifold header.
//
// Scope (M2): validity/repair + a boolean difference + a self-test. Nothing in
// here is wired into GenerateMould yet — that's M3.
//
#include <vector>
#include <cstdint>
#include <string>

namespace MeshBoolean {

// The exchange mesh: positions interleaved [x,y,z] (3 floats/vertex) and
// triangle indices (3 per triangle). Matches cpuVerts / cpuIndices verbatim so
// callers can hand those straight in and drop the result straight back.
struct Mesh
{
    std::vector<float>    verts;    // 3 floats per vertex: x,y,z
    std::vector<uint32_t> indices;  // 3 indices per triangle

    bool   empty()    const { return verts.empty() || indices.empty(); }
    size_t vertCount() const { return verts.size() / 3; }
    size_t triCount()  const { return indices.size() / 3; }
};

// Outcome of bringing a mesh onto the manifold (watertight) toolpath. `ok`
// means the mesh built a valid, non-empty 2-manifold solid — after a best-effort
// weld if one was needed. `wasRepaired` records whether that weld actually had
// to change anything (coincident verts merged / degenerate triangles collapsed).
// `mesh` is the cleaned, round-trippable geometry, valid only when `ok` is true.
// `message` is a short human-readable status suitable for a log line or dialog.
struct RepairResult
{
    bool        ok          = false;
    bool        wasRepaired = false;
    Mesh        mesh;
    std::string message;
};

// Check the mesh and, if needed, apply the ONLY repair this tool attempts:
// Manifold's Merge(), which welds coincident vertices and collapses degenerate
// triangles. It does NOT fill holes, resolve non-manifold edges, or fix
// self-intersections — those stay out of scope (agreed for the mesh toolpath),
// so a mesh with real topological holes comes back ok == false. Never throws.
RepairResult ValidateAndRepair(const Mesh& in);

// Convenience: true iff `in` is a valid, non-empty 2-manifold solid with no
// repair required. (ValidateAndRepair gives the fuller picture.)
bool IsManifold(const Mesh& in);

// Boolean difference: minuend − subtrahend. Both operands are validated /
// welded internally (best effort). Returns true and fills `out` on success;
// on any failure (an operand that couldn't be made manifold, an empty result,
// or a Manifold error) returns false, leaves `out` empty, and puts the reason
// in `error`. Never throws.
bool Difference(const Mesh& minuend, const Mesh& subtrahend,
                Mesh& out, std::string& error);

// Boolean union of many solids into one. Each part is validated/welded
// internally; parts that can't be made manifold are skipped rather than
// aborting the whole union. Returns true and fills `out` when at least one
// part unions into a non-empty solid; false with a reason in `error`
// otherwise. Never throws.
bool Union(const std::vector<Mesh>& parts, Mesh& out, std::string& error);

// Volume of a closed triangle mesh, in the mesh's units cubed (mm^3 for
// Mould3r geometry), via the divergence-theorem tetrahedron sum. Assumes a
// closed surface (e.g. a Union / Difference result); returns the absolute
// value so winding can't flip the sign. Cheap — no Manifold build.
double Volume(const Mesh& mesh);

// M2 smoke test: two overlapping cubes, A − B, exercising the full round trip
// (our Mesh -> MeshGL -> Manifold -> boolean -> MeshGL -> our Mesh -> re-validate).
// Returns true on pass; `report` gets per-step detail either way. Safe to call
// at any time — handy to wire to a debug menu item while bringing M2 up.
bool SelfTest(std::string& report);

} // namespace MeshBoolean
