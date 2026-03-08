#pragma once
#include <string>
#include <vector>
#include <cstdint>

#include <glm/glm.hpp>

class FileImporter
{
public:
    struct MeshData
    {
        // Interleaved vertex buffer: position + color (for now)
        // You can extend to normals/uvs later.
        std::vector<float> vertices;      // [px,py,pz] repeated
        std::vector<uint32_t> indices;    // triangle indices

        glm::vec3 aabbMin{ 0.0f };
        glm::vec3 aabbMax{ 0.0f };

        std::vector<float> posNorm;       // [px,py,pz,nx,ny,nz] repeated
    };

    struct ImportResult
    {
        std::vector<MeshData> meshes;     // multiple meshes possible
        std::string error;                // non-empty on failure
        bool ok() const { return error.empty(); }
    };

public:
    // STEP -> one or more triangulated meshes.
    // linearDeflection: smaller = more triangles
    // angularDeflection: smaller = more triangles (in radians-ish OCC parameter)
    ImportResult ImportSTEP(const std::string& path,
        double linearDeflection = 0.1,
        double angularDeflection = 0.5);

private:
    static void UpdateAABB(glm::vec3& mn, glm::vec3& mx, const glm::vec3& p);
};
