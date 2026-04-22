#pragma once
#include <cstdint>
#include <vector>

// Vertex-clustering mesh decimation.
//
// Uniform grid vertex clustering: partitions the mesh's AABB into an N^3 grid,
// collapses all vertices that fall into the same cell to a single representative
// (cell centroid), and drops any triangle whose three vertices reduce to fewer
// than three distinct representatives.
//
// Not as feature-preserving as quadric-error-metric decimation, but:
//   * Deterministic, bounded runtime (O(V + T) per pass).
//   * No third-party dependencies.
//   * Adequate for reducing dense scans/meshes before CAD booleans, which is
//     the use case here. Sharp edges may soften slightly; overall shape is
//     preserved.
//
// `targetTriCount == 0` means "don't simplify — return inputs unchanged".
// Output may miss the target by some margin; this is clustering, not exact
// edge-collapse.
namespace MeshSimplify
{
    void Decimate(const std::vector<float>& inVerts,       // [x,y,z,...]
                  const std::vector<uint32_t>& inIndices,  // triangle indices
                  uint32_t targetTriCount,
                  std::vector<float>& outVerts,
                  std::vector<uint32_t>& outIndices);
}
