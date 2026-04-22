#pragma once
#include <string>
#include <vector>
#include <cstdint>

#include <glm/glm.hpp>
#include <opencascade/TopoDS_Shape.hxx>

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
        TopoDS_Shape shape;               // BREP shape for booleans (faceted for mesh formats)
        bool hasShape = false;            // true when `shape` is usable
        bool shapeIsClosedSolid = false;  // true only when `shape` is a TopAbs_SOLID
        // (i.e. suitable for robust boolean ops)
        std::string error;                // non-empty on failure
        bool ok() const { return error.empty(); }
    };

public:
    // Dispatches based on file extension: .step/.stp, .stl, .obj (case-insensitive).
    // linearDeflection / angularDeflection are only used by STEP.
    ImportResult ImportAuto(const std::string& path,
        double linearDeflection = 0.1,
        double angularDeflection = 0.5);

    // STEP -> one or more triangulated meshes.
    // linearDeflection: smaller = more triangles
    // angularDeflection: smaller = more triangles (in radians-ish OCC parameter)
    ImportResult ImportSTEP(const std::string& path,
        double linearDeflection = 0.1,
        double angularDeflection = 0.5);

    // STL (binary or ASCII) -> triangulated mesh + faceted BREP.
    ImportResult ImportSTL(const std::string& path);

    // OBJ (vertex positions + triangular/polygonal faces) -> triangulated mesh
    // + faceted BREP. Texture coords and normals in the OBJ are ignored.
    ImportResult ImportOBJ(const std::string& path);

private:
    static void UpdateAABB(glm::vec3& mn, glm::vec3& mx, const glm::vec3& p);

    // Build a faceted TopoDS_Shape from an indexed triangle mesh:
    // one triangular face per triangle, sewn into a shell, and promoted
    // to a solid when the shell is closed. Returns a null shape on failure.
    static TopoDS_Shape BuildFacetedShape(const std::vector<float>& verts,
        const std::vector<uint32_t>& indices,
        const glm::vec3& aabbMin,
        const glm::vec3& aabbMax);

    static std::string ExtensionLower(const std::string& path);
};
