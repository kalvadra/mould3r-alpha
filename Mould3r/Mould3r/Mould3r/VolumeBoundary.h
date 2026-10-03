#pragma once
// ===========================================================================
// VolumeBoundary — boundary conditions for the 3D flow analysis: every face
// on the surface of the tet mesh gets a tag.
//
//   Inlet     where the melt enters the mesh. Cavity-only meshes: each gate
//             mouth (and a direct-injection sprue end) — the patch of the part
//             surface that the gate / sprue solid actually covers, found by
//             testing just outside each face for feed material. Full-shot
//             meshes: the sprue's entry cap, where the machine nozzle seats.
//   Vent      where air leaves: the patch around each vent mouth.
//   Parting   the wall faces the parting plane passes through: the parting
//             line, where air can also escape between the halves.
//   Wall      everything else — melt against mould steel (no slip).
//
// Inlet and vent faces are grouped into regions (one per gate / vent) so the
// solver can feed each gate its share of the flow from the 1D feed network
// and report per vent.
//
// Pure compute: no wx / GL. The caller supplies the geometry of the feed
// system (from Flow::FeedNetwork) and, for the gate footprint test, a
// point-in-feed-material predicate.
// ===========================================================================

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "TetMeshJob.h"

namespace Flow3D
{
    enum BoundaryTag : uint8_t
    {
        TagWall = 0,
        TagInlet = 1,
        TagVent = 2,
        TagParting = 3,
        TagInterior = 255   // not a boundary face
    };
    constexpr int kBoundaryTagCount = 4;
    const char* BoundaryTagName(uint8_t tag);   // "Wall", "Inlet", ...

    // Where melt enters. `dirOut` points out of the mesh toward the feed
    // (along the gate toward the runner; along the sprue toward the nozzle).
    struct InletSpec
    {
        std::string label;
        glm::dvec3  pos{ 0.0 };
        glm::dvec3  dirOut{ 0.0 };
        double      radiusMm = 0.0;   // gate / sprue radius at the mouth
        int         feedNode = -1;    // Flow::FeedNetwork node (the entry node)
    };

    // A vent mouth; `dirOut` points along the vent channel, away from the part.
    struct VentSpec
    {
        std::string label;
        glm::dvec3  pos{ 0.0 };
        glm::dvec3  dirOut{ 0.0 };
        double      halfWidthMm = 0.0;
        int         feedNode = -1;
    };

    struct BoundarySpec
    {
        // Full shot: a single inlet, the sprue's entry cap (the end face of
        // the sprue at `pos`, facing `dirOut`).
        bool sprueCap = false;
        std::vector<InletSpec> inlets;

        // Cavity only: true when a point just outside the cavity wall lies in
        // gate / sprue material — i.e. that bit of the wall is a gate mouth,
        // not steel. Faces are tested at their centroid pushed out by pushMm
        // (more than the mesh's surface envelope, less than a gate is deep).
        // Empty = locate the gate mouths by position only.
        std::function<bool(const glm::dvec3&)> coveredByFeed;
        double pushMm = 0.05;

        std::vector<VentSpec> vents;

        // Parting plane: normal `drawAxis` through `partingOffset` along it.
        glm::dvec3 drawAxis{ 0.0, 1.0, 0.0 };
        double     partingOffset = 0.0;
        double     tolMm = 1.0e-3;   // geometric tolerance (~ the envelope)
    };

    // One inlet (gate) or vent patch.
    struct BoundaryRegion
    {
        std::string label;
        uint8_t     tag = TagInlet;
        int         feedNode = -1;
        size_t      faces = 0;
        double      areaMm2 = 0.0;
        double      nominalAreaMm2 = 0.0;   // pi r^2 of the gate / sprue (inlets)
        glm::dvec3  centre{ 0.0 };          // area-weighted
        bool        byPosition = false;     // footprint test found nothing: placed by position
    };

    struct Boundary
    {
        std::vector<uint8_t> slotTag;       // 4 per tet (tet*4 + face): a tag or TagInterior
        std::vector<int16_t> slotRegion;    // 4 per tet: index into regions, -1 = none
        std::vector<BoundaryRegion> regions;
        size_t faces[kBoundaryTagCount] = {};
        double areaMm2[kBoundaryTagCount] = {};
        std::vector<std::string> warnings;  // one line each

        bool   Ready() const { return !slotTag.empty(); }
        int    Count(uint8_t tag) const;    // regions with this tag
    };

    // Point-in-solid test for a closed, outward-wound triangle surface (a
    // shot, a set of part surfaces): casts three rays and calls the point
    // inside when most of them first meet the surface from its inside. Holds
    // a ray grid, so it isn't copyable.
    class SolidTester
    {
    public:
        SolidTester();
        ~SolidTester();
        SolidTester(const SolidTester&) = delete;
        SolidTester& operator=(const SolidTester&) = delete;

        // xyz: 3 floats per vertex; indices: 3 per triangle; triNormals: the
        // outward normal of each triangle (need not be unit length).
        void Build(std::vector<float> xyz, std::vector<unsigned int> indices,
                   std::vector<glm::vec3> triNormals);
        bool Empty() const;
        bool Inside(const glm::dvec3& p) const;

    private:
        struct Impl;
        Impl* m = nullptr;
    };

    // Tag every boundary face of `mesh` (face adjacency `neighbours` from
    // TetMesh::TetFaceNeighbours).
    Boundary TagBoundary(const TetMesh::Mesh& mesh, const std::vector<int32_t>& neighbours,
                         const BoundarySpec& spec);
}
