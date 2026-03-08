#include "FileImporter.h"

#include <algorithm>
#include <limits>

// OpenCascade
#include <opencascade/STEPControl_Reader.hxx>
#include <opencascade/IFSelect_ReturnStatus.hxx>

#include <opencascade/TopoDS.hxx>

#include <opencascade/TopoDS_Shape.hxx>
#include <opencascade/TopoDS_Face.hxx>

#include <opencascade/TopExp_Explorer.hxx>

#include <opencascade/BRepMesh_IncrementalMesh.hxx>
#include <opencascade/BRep_Tool.hxx>

#include <opencascade/Poly_Triangulation.hxx>
#include <opencascade/Poly_Array1OfTriangle.hxx> 

#include <opencascade/TopLoc_Location.hxx>
#include <opencascade/gp_Pnt.hxx>
#include <opencascade/gp_Trsf.hxx>

void FileImporter::UpdateAABB(glm::vec3& mn, glm::vec3& mx, const glm::vec3& p)
{
    mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
    mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
}

FileImporter::ImportResult FileImporter::ImportSTEP(const std::string& path,
    double linearDeflection,
    double angularDeflection)
{
    ImportResult result;

    STEPControl_Reader reader;
    const IFSelect_ReturnStatus stat = reader.ReadFile(path.c_str());
    if (stat != IFSelect_RetDone) {
        result.error = "STEP read failed (ReadFile).";
        return result;
    }

    reader.TransferRoots();
    TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull()) {
        result.error = "STEP transfer produced null shape.";
        return result;
    }

    // Tessellate (triangulate) the shape
    // Notes: The 4-arg overload allows both linear & angular deflection.
    // Some OCC versions differ; if this doesn’t compile, use the 2-arg constructor.
    BRepMesh_IncrementalMesh mesher(shape, linearDeflection, false, angularDeflection, true);

    // For now: build one combined mesh. (Later you can split by solids/labels/materials.)
    MeshData mesh;
    mesh.aabbMin = glm::vec3(std::numeric_limits<float>::infinity());
    mesh.aabbMax = glm::vec3(-std::numeric_limits<float>::infinity());

    // Constant color for now (CAD viewers often override per-part anyway)
    const glm::vec3 color(0.8f, 0.8f, 0.85f);

    // We'll append vertices and indices per face.
    // Each face triangulation has its own local node list; we map them into global indices.
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Face face = TopoDS::Face(ex.Current());

        TopLoc_Location loc;

        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull())
            continue;

        const gp_Trsf tr = loc.Transformation();

        // Base vertex index for this face in the combined buffer
        const uint32_t baseIndex = static_cast<uint32_t>(mesh.vertices.size() / 3);

        // Add nodes (1-based OCC arrays)
        for (int i = 1; i <= tri->NbNodes(); ++i)
        {
            gp_Pnt p = tri->Node(i);
            p.Transform(tr);

            glm::vec3 pos((float)p.X(), (float)p.Y(), (float)p.Z());
            UpdateAABB(mesh.aabbMin, mesh.aabbMax, pos);

            mesh.vertices.push_back(pos.x);
            mesh.vertices.push_back(pos.y);
            mesh.vertices.push_back(pos.z);

        }

        // Add triangles
        for (int t = 1; t <= tri->NbTriangles(); ++t)
        {
            Poly_Triangle triangle = tri->Triangle(t);

            int n1, n2, n3;
            triangle.Get(n1, n2, n3);

            mesh.indices.push_back(baseIndex + (uint32_t)(n1 - 1));
            mesh.indices.push_back(baseIndex + (uint32_t)(n2 - 1));
            mesh.indices.push_back(baseIndex + (uint32_t)(n3 - 1));
        }
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        result.error = "No triangulation data found. Try different mesh deflection or check file.";
        return result;
    }

    result.meshes.push_back(std::move(mesh));
    return result;
}
