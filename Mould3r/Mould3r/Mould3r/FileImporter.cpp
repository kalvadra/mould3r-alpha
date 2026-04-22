#include "FileImporter.h"
#include "MeshSimplify.h"
#include "MeshImportSettings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// OpenCascade
#include <opencascade/STEPControl_Reader.hxx>
#include <opencascade/IFSelect_ReturnStatus.hxx>

#include <opencascade/TopoDS.hxx>

#include <opencascade/TopoDS_Shape.hxx>
#include <opencascade/TopoDS_Face.hxx>
#include <opencascade/TopoDS_Shell.hxx>

#include <opencascade/TopExp_Explorer.hxx>

#include <opencascade/BRepMesh_IncrementalMesh.hxx>
#include <opencascade/BRep_Tool.hxx>

#include <opencascade/Poly_Triangulation.hxx>
#include <opencascade/Poly_Array1OfTriangle.hxx>

#include <opencascade/TopLoc_Location.hxx>
#include <opencascade/gp_Pnt.hxx>
#include <opencascade/gp_Trsf.hxx>
#include <opencascade/Precision.hxx>

#include <opencascade/BRepBuilderAPI_MakePolygon.hxx>
#include <opencascade/BRepBuilderAPI_MakeFace.hxx>
#include <opencascade/BRepBuilderAPI_MakeSolid.hxx>
#include <opencascade/BRepBuilderAPI_Sewing.hxx>

void FileImporter::UpdateAABB(glm::vec3& mn, glm::vec3& mx, const glm::vec3& p)
{
    mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
    mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
}

std::string FileImporter::ExtensionLower(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

// ---------------------------------------------------------------------------
// Build a faceted TopoDS_Shape (one face per triangle, sewn into a shell).
// ---------------------------------------------------------------------------
TopoDS_Shape FileImporter::BuildFacetedShape(const std::vector<float>& verts,
    const std::vector<uint32_t>& indices,
    const glm::vec3& aabbMin,
    const glm::vec3& aabbMax)
{
    if (indices.size() < 3 || verts.size() < 9) return TopoDS_Shape();

    // Pick a sewing tolerance scaled to the model size: tight enough to
    // preserve detail, loose enough to actually merge shared edges given
    // float<->double round-tripping.
    const glm::vec3 diag = aabbMax - aabbMin;
    const double diagLen = (double)std::max({ diag.x, diag.y, diag.z, 1.0e-6f });
    double tol = 1.0e-5 * diagLen;
    tol = std::max(tol, Precision::Confusion() * 10.0);
    tol = std::min(tol, 0.1);

    BRepBuilderAPI_Sewing sewing(tol);
    const double degenTol = Precision::Confusion();

    size_t addedFaces = 0;
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const uint32_t i0 = indices[i + 0];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        if (i0 == i1 || i1 == i2 || i0 == i2) continue;
        if ((size_t)i0 * 3 + 2 >= verts.size() ||
            (size_t)i1 * 3 + 2 >= verts.size() ||
            (size_t)i2 * 3 + 2 >= verts.size()) continue;

        gp_Pnt p0(verts[i0 * 3 + 0], verts[i0 * 3 + 1], verts[i0 * 3 + 2]);
        gp_Pnt p1(verts[i1 * 3 + 0], verts[i1 * 3 + 1], verts[i1 * 3 + 2]);
        gp_Pnt p2(verts[i2 * 3 + 0], verts[i2 * 3 + 1], verts[i2 * 3 + 2]);

        // Skip zero-length edges / zero-area triangles
        if (p0.Distance(p1) < degenTol ||
            p1.Distance(p2) < degenTol ||
            p2.Distance(p0) < degenTol) continue;

        try
        {
            BRepBuilderAPI_MakePolygon poly(p0, p1, p2, Standard_True);
            if (!poly.IsDone()) continue;

            BRepBuilderAPI_MakeFace mkFace(poly.Wire(), Standard_True);
            if (!mkFace.IsDone()) continue;

            sewing.Add(mkFace.Face());
            ++addedFaces;
        }
        catch (...) {
            // OCC throws Standard_Failure on degenerate input; skip.
            continue;
        }
    }

    if (addedFaces == 0) return TopoDS_Shape();

    try { sewing.Perform(); }
    catch (...) { return TopoDS_Shape(); }

    TopoDS_Shape sewn = sewing.SewedShape();
    if (sewn.IsNull()) return TopoDS_Shape();

    // If we got a single closed shell, promote to a solid so booleans have
    // volumetric semantics.
    if (sewn.ShapeType() == TopAbs_SHELL)
    {
        try
        {
            BRepBuilderAPI_MakeSolid mkSolid(TopoDS::Shell(sewn));
            if (mkSolid.IsDone())
            {
                // Use the inherited Shape() accessor (returns const TopoDS_Shape&)
                // rather than Solid() — in some OCC versions Solid() returns a
                // TopoDS_Solid by value, and the derived-to-base conversion
                // fails in the return-value context with MSVC.
                return mkSolid.Shape();
            }
        }
        catch (...) { /* fall through to returning the shell */ }
    }
    return sewn;
}

// ---------------------------------------------------------------------------
// STEP
// ---------------------------------------------------------------------------
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
    BRepMesh_IncrementalMesh mesher(shape, linearDeflection, false, angularDeflection, true);

    // For now: build one combined mesh. (Later you can split by solids/labels/materials.)
    MeshData mesh;
    mesh.aabbMin = glm::vec3(std::numeric_limits<float>::infinity());
    mesh.aabbMax = glm::vec3(-std::numeric_limits<float>::infinity());

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

    // STEP brings its own native BREP — no faceting needed.
    result.shape = shape;
    result.hasShape = true;
    result.shapeIsClosedSolid = (shape.ShapeType() == TopAbs_SOLID ||
                                 shape.ShapeType() == TopAbs_COMPSOLID ||
                                 shape.ShapeType() == TopAbs_COMPOUND);

    result.meshes.push_back(std::move(mesh));
    return result;
}

// ---------------------------------------------------------------------------
// STL
// ---------------------------------------------------------------------------
namespace {

    // Quantized vertex key for dedup (STL repeats every vertex 3+ times).
    struct VertexKey {
        int32_t x, y, z;
        bool operator==(const VertexKey& o) const noexcept
        {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct VertexKeyHash {
        size_t operator()(const VertexKey& k) const noexcept
        {
            // Mix the three ints. Good enough for our purposes.
            const uint64_t h =
                ((uint64_t)(uint32_t)k.x * 73856093u) ^
                ((uint64_t)(uint32_t)k.y * 19349663u) ^
                ((uint64_t)(uint32_t)k.z * 83492791u);
            return (size_t)h;
        }
    };

    // Dedup tolerance (quantization step) chosen to be tight; the sewing
    // tolerance in BuildFacetedShape handles any residual gaps.
    constexpr float kDedupQuantum = 1.0e-5f;

    inline VertexKey Quantize(float x, float y, float z) {
        return {
            (int32_t)std::lround(x / kDedupQuantum),
            (int32_t)std::lround(y / kDedupQuantum),
            (int32_t)std::lround(z / kDedupQuantum)
        };
    }

    // Append a vertex to verts, deduping via `lookup`. Returns its global index.
    inline uint32_t AddDeduped(std::vector<float>& verts,
        std::unordered_map<VertexKey, uint32_t, VertexKeyHash>& lookup,
        glm::vec3& aabbMin, glm::vec3& aabbMax,
        float x, float y, float z)
    {
        VertexKey k = Quantize(x, y, z);
        auto it = lookup.find(k);
        if (it != lookup.end()) return it->second;

        const uint32_t idx = (uint32_t)(verts.size() / 3);
        verts.push_back(x);
        verts.push_back(y);
        verts.push_back(z);

        aabbMin.x = std::min(aabbMin.x, x);
        aabbMin.y = std::min(aabbMin.y, y);
        aabbMin.z = std::min(aabbMin.z, z);
        aabbMax.x = std::max(aabbMax.x, x);
        aabbMax.y = std::max(aabbMax.y, y);
        aabbMax.z = std::max(aabbMax.z, z);

        lookup.emplace(k, idx);
        return idx;
    }

    // Detect binary STL: file size exactly matches 84 + 50 * triCount.
    // (ASCII STL rarely hits that size by accident.)
    bool IsBinarySTL(std::ifstream& in, std::uint64_t fileSize)
    {
        if (fileSize < 84) return false;
        in.seekg(80, std::ios::beg);
        uint32_t triCount = 0;
        in.read(reinterpret_cast<char*>(&triCount), sizeof(triCount));
        in.seekg(0, std::ios::beg);
        if (!in) return false;
        const std::uint64_t expected = 84ull + 50ull * (std::uint64_t)triCount;
        return expected == fileSize;
    }

    bool ParseBinarySTL(std::ifstream& in,
        std::vector<float>& verts, std::vector<uint32_t>& indices,
        glm::vec3& aabbMin, glm::vec3& aabbMax, std::string& error)
    {
        in.seekg(80, std::ios::beg);
        uint32_t triCount = 0;
        in.read(reinterpret_cast<char*>(&triCount), sizeof(triCount));
        if (!in) { error = "STL: failed to read triangle count."; return false; }

        std::unordered_map<VertexKey, uint32_t, VertexKeyHash> lookup;
        lookup.reserve((size_t)triCount * 3 / 2 + 16);
        verts.reserve((size_t)triCount * 3 * 3 / 2);
        indices.reserve((size_t)triCount * 3);

        for (uint32_t t = 0; t < triCount; ++t)
        {
            float n[3], v0[3], v1[3], v2[3];
            uint16_t attr = 0;

            in.read(reinterpret_cast<char*>(n), sizeof(n));
            in.read(reinterpret_cast<char*>(v0), sizeof(v0));
            in.read(reinterpret_cast<char*>(v1), sizeof(v1));
            in.read(reinterpret_cast<char*>(v2), sizeof(v2));
            in.read(reinterpret_cast<char*>(&attr), sizeof(attr));
            if (!in) { error = "STL: unexpected end of file."; return false; }

            const uint32_t a = AddDeduped(verts, lookup, aabbMin, aabbMax, v0[0], v0[1], v0[2]);
            const uint32_t b = AddDeduped(verts, lookup, aabbMin, aabbMax, v1[0], v1[1], v1[2]);
            const uint32_t c = AddDeduped(verts, lookup, aabbMin, aabbMax, v2[0], v2[1], v2[2]);
            if (a == b || b == c || a == c) continue;  // degenerate
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
        }
        return true;
    }

    bool ParseAsciiSTL(std::ifstream& in,
        std::vector<float>& verts, std::vector<uint32_t>& indices,
        glm::vec3& aabbMin, glm::vec3& aabbMax, std::string& error)
    {
        in.seekg(0, std::ios::beg);

        std::unordered_map<VertexKey, uint32_t, VertexKeyHash> lookup;

        std::string tok;
        float v[9];
        int vCount = 0;

        while (in >> tok)
        {
            if (tok == "vertex")
            {
                if (vCount >= 9) { vCount = 0; } // shouldn't happen
                if (!(in >> v[vCount] >> v[vCount + 1] >> v[vCount + 2])) {
                    error = "STL (ascii): malformed vertex.";
                    return false;
                }
                vCount += 3;
                if (vCount == 9)
                {
                    const uint32_t a = AddDeduped(verts, lookup, aabbMin, aabbMax, v[0], v[1], v[2]);
                    const uint32_t b = AddDeduped(verts, lookup, aabbMin, aabbMax, v[3], v[4], v[5]);
                    const uint32_t c = AddDeduped(verts, lookup, aabbMin, aabbMax, v[6], v[7], v[8]);
                    vCount = 0;
                    if (a != b && b != c && a != c)
                    {
                        indices.push_back(a);
                        indices.push_back(b);
                        indices.push_back(c);
                    }
                }
            }
            // Other tokens (solid, facet, normal, outer, loop, endloop, endfacet,
            // endsolid, names) are ignored; the vertex extractor handles everything.
        }
        return true;
    }

} // namespace

FileImporter::ImportResult FileImporter::ImportSTL(const std::string& path)
{
    ImportResult result;

    std::ifstream in(path, std::ios::binary);
    if (!in) { result.error = "STL: cannot open file."; return result; }

    in.seekg(0, std::ios::end);
    const std::uint64_t fileSize = (std::uint64_t)in.tellg();
    in.seekg(0, std::ios::beg);
    if (fileSize == 0) { result.error = "STL: empty file."; return result; }

    MeshData mesh;
    mesh.aabbMin = glm::vec3(std::numeric_limits<float>::infinity());
    mesh.aabbMax = glm::vec3(-std::numeric_limits<float>::infinity());

    const bool binary = IsBinarySTL(in, fileSize);
    bool ok = binary
        ? ParseBinarySTL(in, mesh.vertices, mesh.indices, mesh.aabbMin, mesh.aabbMax, result.error)
        : ParseAsciiSTL(in, mesh.vertices, mesh.indices, mesh.aabbMin, mesh.aabbMax, result.error);

    if (!ok) return result;
    if (mesh.indices.empty() || mesh.vertices.empty()) {
        result.error = "STL: no triangles found.";
        return result;
    }

    // Apply user-configured simplification before we build the BREP shape.
    // The sewed-and-solidified shape derived from a decimated mesh is what
    // drives boolean performance downstream, so we decimate first.
    const auto q = MeshImportSettings::GetQuality();
    const uint32_t target = MeshImportSettings::TargetTriangleCount(q);
    if (target > 0) {
        std::vector<float>    dv;
        std::vector<uint32_t> di;
        MeshSimplify::Decimate(mesh.vertices, mesh.indices, target, dv, di);
        if (!di.empty()) {
            mesh.vertices = std::move(dv);
            mesh.indices  = std::move(di);
            // Recompute AABB on the clustered geometry.
            mesh.aabbMin = glm::vec3(std::numeric_limits<float>::infinity());
            mesh.aabbMax = glm::vec3(-std::numeric_limits<float>::infinity());
            for (size_t i = 0, n = mesh.vertices.size() / 3; i < n; ++i) {
                const glm::vec3 p(mesh.vertices[i * 3 + 0],
                                  mesh.vertices[i * 3 + 1],
                                  mesh.vertices[i * 3 + 2]);
                UpdateAABB(mesh.aabbMin, mesh.aabbMax, p);
            }
        }
    }

    // Build BREP for booleans.
    result.shape = BuildFacetedShape(mesh.vertices, mesh.indices, mesh.aabbMin, mesh.aabbMax);
    result.hasShape = !result.shape.IsNull();
    result.shapeIsClosedSolid = result.hasShape &&
        result.shape.ShapeType() == TopAbs_SOLID;

    result.meshes.push_back(std::move(mesh));
    return result;
}

// ---------------------------------------------------------------------------
// OBJ
// ---------------------------------------------------------------------------
namespace {

    // Parse the first integer of an OBJ face token like "12", "12/34", "12/34/56", "12//56".
    // Supports negative (relative) indices. Returns false if no integer found.
    bool ParseObjFaceIndex(const std::string& tok, int& out)
    {
        if (tok.empty()) return false;
        size_t i = 0;
        if (tok[0] == '-' || tok[0] == '+') i = 1;
        size_t j = i;
        while (j < tok.size() && std::isdigit((unsigned char)tok[j])) ++j;
        if (j == i) return false;
        out = std::atoi(tok.substr(0, j).c_str());
        return true;
    }

} // namespace

FileImporter::ImportResult FileImporter::ImportOBJ(const std::string& path)
{
    ImportResult result;

    std::ifstream in(path);
    if (!in) { result.error = "OBJ: cannot open file."; return result; }

    // All positions from 'v' lines, 1-indexed in the OBJ; we store 0-indexed
    // contiguously as [x,y,z,x,y,z,...].
    std::vector<float> positions;
    positions.reserve(1 << 15);

    MeshData mesh;
    mesh.aabbMin = glm::vec3(std::numeric_limits<float>::infinity());
    mesh.aabbMax = glm::vec3(-std::numeric_limits<float>::infinity());

    // OBJ commonly shares vertex-position records across many faces, so we
    // can re-emit positions directly (no dedup needed for correctness) — but
    // we still want an index buffer that references them.
    // To keep the output shape consistent with STL, we just copy positions
    // into mesh.vertices as-is once we know which ones are used? For simplicity
    // (and because OBJ files rarely contain unused positions), we copy all
    // of them and let the index buffer reference 0-based positions.

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty()) continue;
        // Strip trailing '\r' (common for CRLF files on Unix)
        if (line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // Skip leading whitespace
        size_t p = 0;
        while (p < line.size() && std::isspace((unsigned char)line[p])) ++p;
        if (p >= line.size() || line[p] == '#') continue;

        // Tokenize
        std::istringstream ss(line.substr(p));
        std::string kw;
        ss >> kw;

        if (kw == "v")
        {
            float x = 0, y = 0, z = 0;
            if (!(ss >> x >> y >> z)) continue;
            positions.push_back(x);
            positions.push_back(y);
            positions.push_back(z);
            const glm::vec3 pt(x, y, z);
            UpdateAABB(mesh.aabbMin, mesh.aabbMax, pt);
        }
        else if (kw == "f")
        {
            // Collect all vertex indices on this face, then fan-triangulate.
            std::vector<uint32_t> faceVerts;
            faceVerts.reserve(4);
            std::string tok;
            while (ss >> tok)
            {
                int raw = 0;
                if (!ParseObjFaceIndex(tok, raw)) continue;
                const int nPos = (int)(positions.size() / 3);
                int idx;
                if (raw > 0)        idx = raw - 1;           // 1-based -> 0-based
                else if (raw < 0)   idx = nPos + raw;        // relative to end
                else                continue;                // 0 is invalid in OBJ
                if (idx < 0 || idx >= nPos) continue;
                faceVerts.push_back((uint32_t)idx);
            }
            if (faceVerts.size() < 3) continue;
            // Fan triangulation: (0, i, i+1)
            for (size_t i = 1; i + 1 < faceVerts.size(); ++i)
            {
                const uint32_t a = faceVerts[0];
                const uint32_t b = faceVerts[i];
                const uint32_t c = faceVerts[i + 1];
                if (a == b || b == c || a == c) continue;
                mesh.indices.push_back(a);
                mesh.indices.push_back(b);
                mesh.indices.push_back(c);
            }
        }
        // Ignore vt, vn, vp, o, g, s, usemtl, mtllib, etc.
    }

    if (positions.empty() || mesh.indices.empty())
    {
        result.error = "OBJ: no geometry found (need at least 'v' and 'f' records).";
        return result;
    }

    mesh.vertices = std::move(positions);

    // Apply user-configured simplification before we build the BREP shape
    // (same rationale as the STL path — decimate first for boolean speed).
    {
        const auto q = MeshImportSettings::GetQuality();
        const uint32_t target = MeshImportSettings::TargetTriangleCount(q);
        if (target > 0) {
            std::vector<float>    dv;
            std::vector<uint32_t> di;
            MeshSimplify::Decimate(mesh.vertices, mesh.indices, target, dv, di);
            if (!di.empty()) {
                mesh.vertices = std::move(dv);
                mesh.indices  = std::move(di);
                mesh.aabbMin = glm::vec3(std::numeric_limits<float>::infinity());
                mesh.aabbMax = glm::vec3(-std::numeric_limits<float>::infinity());
                for (size_t i = 0, n = mesh.vertices.size() / 3; i < n; ++i) {
                    const glm::vec3 p(mesh.vertices[i * 3 + 0],
                                      mesh.vertices[i * 3 + 1],
                                      mesh.vertices[i * 3 + 2]);
                    UpdateAABB(mesh.aabbMin, mesh.aabbMax, p);
                }
            }
        }
    }

    // Build BREP for booleans.
    result.shape = BuildFacetedShape(mesh.vertices, mesh.indices, mesh.aabbMin, mesh.aabbMax);
    result.hasShape = !result.shape.IsNull();
    result.shapeIsClosedSolid = result.hasShape &&
        result.shape.ShapeType() == TopAbs_SOLID;

    result.meshes.push_back(std::move(mesh));
    return result;
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------
FileImporter::ImportResult FileImporter::ImportAuto(const std::string& path,
    double linearDeflection,
    double angularDeflection)
{
    const std::string ext = ExtensionLower(path);
    if (ext == "step" || ext == "stp")
        return ImportSTEP(path, linearDeflection, angularDeflection);
    if (ext == "stl")
        return ImportSTL(path);
    if (ext == "obj")
        return ImportOBJ(path);

    ImportResult result;
    result.error = "Unsupported file extension: '." + ext +
        "'. Supported formats: STEP (.step/.stp), STL (.stl), OBJ (.obj).";
    return result;
}
