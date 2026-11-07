#include "FileImporter.h"
#include "MeshBoolean.h"
#include "FixtureFile.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <manifold/manifold.h>
#include <filesystem>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
static MeshBoolean::Mesh mesh(const manifold::Manifold& solid) {
    const auto gl = solid.GetMeshGL();
    MeshBoolean::Mesh m;
    m.verts = gl.vertProperties;
    m.indices = gl.triVerts;
    return m;
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "Expected fixture and test output directory");
        const std::filesystem::path output(argv[2]);
        std::filesystem::create_directories(output);
        FixtureDefinition fixture;
        std::string error;
        require(FixtureFile::Load(argv[1], fixture, error), error);
        // A real mould-style subtraction, then STEP export and application import.
        auto blank = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
        auto cavity = BRepPrimAPI_MakeBox(gp_Pnt(2,2,2), 6,6,6).Shape();
        BRepAlgoAPI_Cut cut(blank, cavity);
        require(cut.IsDone() && !cut.Shape().IsNull(), "OCCT boolean failed");
        const auto file = output / "mould-épreuve.step";
        STEPControl_Writer writer;
        require(writer.Transfer(cut.Shape(), STEPControl_AsIs) == IFSelect_RetDone, "STEP transfer failed");
        require(writer.Write(file.string().c_str()) == IFSelect_RetDone, "STEP write failed");
        FileImporter importer;
        auto imported = importer.ImportSTEP(file.string());
        require(imported.ok() && imported.hasShape && !imported.meshes.empty(), "Application STEP import failed: " + imported.error);
        GProp_GProps props;
        BRepGProp::VolumeProperties(imported.shape, props);
        require(std::abs(props.Mass() - 784.0) < 1e-4, "STEP round-trip changed solid volume");
        auto a = mesh(manifold::Manifold::Cube({10,10,10}));
        auto b = mesh(manifold::Manifold::Cube({6,6,6}).Translate({2,2,2}));
        MeshBoolean::Mesh result;
        require(MeshBoolean::Difference(a,b,result,error), "Mesh boolean failed: " + error);
        require(std::abs(MeshBoolean::Volume(result)-784.0) < 1e-3, "Mesh boolean volume incorrect");
        require(MeshBoolean::IsManifold(result), "Mesh subtraction is not manifold");
        std::cout << "PASS: fixture parse, OCCT cut, STEP export/import with UTF-8 path, mesh boolean and volume\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
