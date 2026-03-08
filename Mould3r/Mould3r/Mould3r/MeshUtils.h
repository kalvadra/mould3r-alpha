#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

static void ComputeVertexNormals_Pos3(std::vector<float>& pos3,          // [px,py,pz]...
    const std::vector<uint32_t>& idx,
    std::vector<float>& outPosNorm6)  // [px,py,pz,nx,ny,nz]...
{
    const size_t vcount = pos3.size() / 3;
    std::vector<glm::vec3> normals(vcount, glm::vec3(0.0f));

    auto P = [&](uint32_t vi) -> glm::vec3 {
        return glm::vec3(pos3[vi * 3 + 0], pos3[vi * 3 + 1], pos3[vi * 3 + 2]);
        };

    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        uint32_t i0 = idx[i + 0], i1 = idx[i + 1], i2 = idx[i + 2];
        glm::vec3 p0 = P(i0), p1 = P(i1), p2 = P(i2);

        glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
        float len2 = glm::dot(fn, fn);
        if (len2 < 1e-20f) continue;

        // accumulate (area-weighted)
        normals[i0] += fn;
        normals[i1] += fn;
        normals[i2] += fn;
    }

    outPosNorm6.clear();
    outPosNorm6.reserve(vcount * 6);

    for (size_t vi = 0; vi < vcount; ++vi) {
        glm::vec3 n = normals[vi];
        float len2 = glm::dot(n, n);
        if (len2 > 1e-20f) n = glm::normalize(n);
        else n = glm::vec3(0, 1, 0);

        outPosNorm6.push_back(pos3[vi * 3 + 0]);
        outPosNorm6.push_back(pos3[vi * 3 + 1]);
        outPosNorm6.push_back(pos3[vi * 3 + 2]);
        outPosNorm6.push_back(n.x);
        outPosNorm6.push_back(n.y);
        outPosNorm6.push_back(n.z);
    }
}
