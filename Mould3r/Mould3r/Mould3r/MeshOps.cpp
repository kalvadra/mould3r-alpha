#include "MeshOps.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <unordered_map>
#include <limits>
#include <cmath>
#include <cassert>

static inline glm::vec3 ReadPos3(const std::vector<float>& pos3, uint32_t vi)
{
    const size_t base = (size_t)vi * 3u;
    return glm::vec3(pos3[base + 0], pos3[base + 1], pos3[base + 2]);
}

static inline void WritePosNorm(std::vector<float>& out, const glm::vec3& p, const glm::vec3& n)
{
    out.push_back(p.x); out.push_back(p.y); out.push_back(p.z);
    out.push_back(n.x); out.push_back(n.y); out.push_back(n.z);
}

CreaseSplitResult SplitByCreaseAngle_Pos3(
    const std::vector<float>& pos3,
    const std::vector<uint32_t>& indices,
    float creaseAngleDeg
)
{
    CreaseSplitResult result;

    if (pos3.empty() || indices.empty() || (pos3.size() % 3) != 0 || (indices.size() % 3) != 0)
        return result;

    const uint32_t vertexCount = (uint32_t)(pos3.size() / 3);
    const uint32_t triCount = (uint32_t)(indices.size() / 3);

    // --- 1) Compute face normals + weights (area-ish)
    std::vector<glm::vec3> faceN(triCount, glm::vec3(0, 0, 1));
    std::vector<float>     faceW(triCount, 0.0f);

    const float eps = 1e-20f;

    for (uint32_t t = 0; t < triCount; ++t)
    {
        const uint32_t i0 = indices[3u * t + 0];
        const uint32_t i1 = indices[3u * t + 1];
        const uint32_t i2 = indices[3u * t + 2];

        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            continue;

        const glm::vec3 p0 = ReadPos3(pos3, i0);
        const glm::vec3 p1 = ReadPos3(pos3, i1);
        const glm::vec3 p2 = ReadPos3(pos3, i2);

        const glm::vec3 e1 = p1 - p0;
        const glm::vec3 e2 = p2 - p0;

        const glm::vec3 c = glm::cross(e1, e2);
        const float len = glm::length(c);

        // len is 2*area of triangle
        if (len > eps)
        {
            faceN[t] = c / len;   // normalize
            faceW[t] = len;       // weight by area*2
        }
        else
        {
            // Degenerate triangle: keep a harmless normal; give it no weight
            faceN[t] = glm::vec3(0, 0, 1);
            faceW[t] = 0.0f;
        }
    }

    // --- 2) Build triangle adjacency per vertex
    std::vector<std::vector<uint32_t>> trisByVertex(vertexCount);
    trisByVertex.reserve(vertexCount);

    for (uint32_t t = 0; t < triCount; ++t)
    {
        const uint32_t i0 = indices[3u * t + 0];
        const uint32_t i1 = indices[3u * t + 1];
        const uint32_t i2 = indices[3u * t + 2];

        if (i0 < vertexCount) trisByVertex[i0].push_back(t);
        if (i1 < vertexCount) trisByVertex[i1].push_back(t);
        if (i2 < vertexCount) trisByVertex[i2].push_back(t);
    }

    // --- 3) Crease threshold
    const float rad = glm::radians(creaseAngleDeg);
    const float cosThreshold = std::cos(rad);

    // --- 4) For each vertex, cluster its incident triangles by normal similarity,
    //         then create one output vertex per cluster.
    //
    // We'll store a mapping: per original vertex v, map triangleId -> outputVertexIndex
    std::vector<std::unordered_map<uint32_t, uint32_t>> triToOutVertex(vertexCount);

    result.posNorm.reserve(pos3.size() * 2); // rough guess (may grow)
    result.indices.resize(indices.size());

    for (uint32_t v = 0; v < vertexCount; ++v)
    {
        const auto& incident = trisByVertex[v];
        if (incident.empty())
            continue;

        // Local indexing into incident list
        const uint32_t k = (uint32_t)incident.size();
        std::vector<uint8_t> visited(k, 0);

        const glm::vec3 pv = ReadPos3(pos3, v);

        // Greedy BFS clustering on the "similar normal" relation
        for (uint32_t seed = 0; seed < k; ++seed)
        {
            if (visited[seed])
                continue;

            // Start a new group
            std::vector<uint32_t> queue;
            queue.reserve(16);
            queue.push_back(seed);
            visited[seed] = 1;

            glm::vec3 sumN(0.0f);
            float     sumW = 0.0f;

            // Indices (into incident[]) that belong to this group
            std::vector<uint32_t> groupLocals;
            groupLocals.reserve(16);

            while (!queue.empty())
            {
                const uint32_t curLocal = queue.back();
                queue.pop_back();

                groupLocals.push_back(curLocal);

                const uint32_t triA = incident[curLocal];
                const glm::vec3 nA = faceN[triA];

                // Accumulate weighted normals for the group's final vertex normal
                const float wA = faceW[triA];
                sumN += nA * wA;
                sumW += wA;

                // Grow the group: any unvisited triangle whose normal is within threshold
                // to *this* triangle gets added.
                for (uint32_t j = 0; j < k; ++j)
                {
                    if (visited[j])
                        continue;

                    const uint32_t triB = incident[j];
                    const glm::vec3 nB = faceN[triB];

                    const float d = glm::dot(nA, nB);
                    if (d >= cosThreshold)
                    {
                        visited[j] = 1;
                        queue.push_back(j);
                    }
                }
            }

            glm::vec3 groupN(0, 0, 1);
            if (sumW > 0.0f && glm::length(sumN) > eps)
                groupN = glm::normalize(sumN);
            else
                groupN = glm::vec3(0, 0, 1); // fallback for all-degenerate cases

            // Create the output vertex for this group
            const uint32_t outIndex = (uint32_t)(result.posNorm.size() / 6);
            WritePosNorm(result.posNorm, pv, groupN);

            // Map all triangles in this group at vertex v to this output vertex index
            auto& map = triToOutVertex[v];
            for (uint32_t localIdx : groupLocals)
            {
                const uint32_t triId = incident[localIdx];
                map[triId] = outIndex;
            }
        }
    }

    // --- 5) Remap indices per triangle corner:
    // Each triangle corner (v, t) selects the split vertex for that triangle's group at v.
    for (uint32_t t = 0; t < triCount; ++t)
    {
        for (uint32_t c = 0; c < 3; ++c)
        {
            const uint32_t origV = indices[3u * t + c];
            if (origV >= vertexCount)
            {
                result.indices[3u * t + c] = 0;
                continue;
            }

            auto& map = triToOutVertex[origV];
            auto it = map.find(t);

            if (it != map.end())
            {
                result.indices[3u * t + c] = it->second;
            }
            else
            {
                // Should be rare (e.g., if something was out-of-range/degenerate)
                // Fallback: create a simple "up" normal vertex for this corner.
                const glm::vec3 pv = ReadPos3(pos3, origV);
                const uint32_t outIndex = (uint32_t)(result.posNorm.size() / 6);
                WritePosNorm(result.posNorm, pv, glm::vec3(0, 0, 1));
                result.indices[3u * t + c] = outIndex;
            }
        }
    }

    return result;
}
