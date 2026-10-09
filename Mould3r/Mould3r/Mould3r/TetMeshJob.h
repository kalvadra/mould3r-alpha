#pragma once
// ===========================================================================
// TetMeshJob — the data side of 3D (tetrahedral) meshing, shared by the app
// and the `--mesh-worker` process. Pure compute + file I/O: no wx, no GL,
// no fTetWild (that lives behind third_party/m3tetmesh, called only by
// MeshWorker in the worker process).
//
//   Surface   the closed triangle surface to fill (welded, mm)
//   Params    mesher settings (mirror of M3TetParams, so the app doesn't need
//             the mesher header)
//   Mesh      the result: vertices + positively oriented tets (mm)
//   Stats     what the app reports about a mesh (count, volume, quality)
//
// JOB FILES: the app writes <job>.in, launches "Mould3r.exe --mesh-worker
// <job>.in <job>.out" and reads <job>.out when the worker exits. Binary,
// native endianness (same machine), each file headed by a magic + version.
// The worker writes <job>.out.part and renames it, so a crashed worker never
// leaves a half-written .out behind — no .out means "the worker died".
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace TetMesh
{
    struct Params
    {
        double  edgeLength = 0.0;   // target tet edge (mm); <= 0 = mesher default (bbox diag / 20)
        double  epsilon = 0.0;      // surface envelope (mm); <= 0 = mesher default (bbox diag / 1000)
        double  stopEnergy = 10.0;  // optimisation stop energy
        int32_t maxPasses = 80;     // optimisation passes cap
        int32_t maxThreads = 0;     // 0 = all hardware threads
        int32_t coarsen = 0;        // 1 = coarsen where the envelope allows
    };

    struct Surface
    {
        std::vector<double>  verts;   // 3 per vertex
        std::vector<int32_t> tris;    // 3 per triangle
        size_t VertexCount() const { return verts.size() / 3; }
        size_t TriangleCount() const { return tris.size() / 3; }
    };

    struct Mesh
    {
        std::vector<double>  verts;   // 3 per vertex
        std::vector<int32_t> tets;    // 4 per tet, positive orientation
        size_t VertexCount() const { return verts.size() / 3; }
        size_t TetCount() const { return tets.size() / 4; }
        bool   Empty() const { return tets.empty(); }
    };

    // Status the worker reports in the job output (values match m3tetmesh.h,
    // plus the worker's own failures).
    enum Status : int32_t
    {
        StatusOk = 0,
        StatusBadInput = 1,
        StatusMesherFailed = 2,
        StatusEmptyResult = 3,
        StatusMesherException = 4,
        StatusMesherMissing = 10,   // m3tetmesh.dll not built / not found / wrong ABI
        StatusBadJobFile = 11,
    };

    struct JobResult
    {
        int32_t     status = StatusBadJobFile;
        std::string message;          // UTF-8, one line
        Mesh        mesh;
        double      workerSeconds = 0.0;   // time inside the mesher
    };

    struct Stats
    {
        size_t tets = 0, verts = 0;
        size_t boundaryFaces = 0;          // faces on the mesh surface
        double volumeMm3 = 0.0;            // sum of tet volumes
        double minDihedralDeg = 0.0;       // worst (smallest) dihedral angle
        double maxDihedralDeg = 0.0;       // worst (largest) dihedral angle
        size_t slivers = 0;                // tets with a dihedral < 5 degrees
        size_t nonPositive = 0;            // tets with volume <= 0 (should be 0)
        double meanEdgeMm = 0.0;           // mean tet edge length
    };

    // Weld a flat-shaded triangle buffer (vertex positions at `stride` floats
    // apart, e.g. 6 for position+normal) into a shared-vertex surface: exactly
    // coincident positions merge and triangles that collapse are dropped.
    Surface WeldSurface(const float* data, size_t vertexCount, size_t stride,
                        const uint32_t* indices, size_t indexCount);

    // Signed volume enclosed by a closed, outward-oriented surface (mm^3);
    // the reference the tet mesh volume is checked against.
    double SurfaceVolume(const Surface& s);

    Stats ComputeStats(const Mesh& m);

    // Corner triples of a tet's four faces, face f opposite corner f, ordered
    // so the face normal points OUT of a positively oriented tet.
    inline constexpr int kTetFace[4][3] = { { 1, 2, 3 }, { 0, 3, 2 }, { 0, 1, 3 }, { 0, 2, 1 } };

    // Per-tet quality: the smallest dihedral angle (degrees). A regular tet
    // has 70.5; under ~10 is a sliver that hurts a flow solve.
    std::vector<float> TetMinDihedral(const Mesh& m);

    // Face adjacency: 4 entries per tet, the tet across face f (kTetFace
    // order), or -1 where the face is on the mesh boundary.
    std::vector<int32_t> TetFaceNeighbours(const Mesh& m);

    // Export for outside viewers. VTU (ParaView, VTK XML unstructured grid,
    // binary appended) carries the per-tet min dihedral as cell data when
    // `minDihedral` matches the tet count; MSH is Gmsh ASCII format 2.2.
    //
    // Boundary conditions: with `slotTag` (4 per tet, tet*4 + face; 255 =
    // interior) the boundary faces are written as triangles after the tets.
    // VTU: cell data "boundary" = the tag (-1 on tets), so a Threshold on it
    // picks out e.g. the inlets. MSH: each tag is physical group tag + 1,
    // named from `tagNames` (the tets are group 100, "volume").
    // `pointData`: named per-node arrays (vertex count long) written as point data.
    bool WriteVtu(const std::filesystem::path& path, const Mesh& m,
                  const std::vector<float>* minDihedral, std::string& error,
                  const std::vector<uint8_t>* slotTag = nullptr,
                  const std::vector<std::pair<std::string, const std::vector<float>*>>& pointData = {});
    bool WriteGmshMsh(const std::filesystem::path& path, const Mesh& m, std::string& error,
                      const std::vector<uint8_t>* slotTag = nullptr,
                      const std::vector<std::string>& tagNames = {});

    bool WriteJobInput(const std::filesystem::path& path, const Surface& s, const Params& p, std::string& error);
    bool ReadJobInput(const std::filesystem::path& path, Surface& s, Params& p, std::string& error);

    // Writes `path`.part then renames it to `path`.
    bool WriteJobOutput(const std::filesystem::path& path, const JobResult& r, std::string& error);
    bool ReadJobOutput(const std::filesystem::path& path, JobResult& r, std::string& error);
}
