#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "FixtureFile.h"

// ---------------------------------------------------------------------------
// ProjectData — everything needed to fully restore a saved session.
//               GPU resources are NOT stored; they are rebuilt from this data.
// ---------------------------------------------------------------------------
struct ProjectObjectData
{
    std::string sourcePath;
    glm::vec3   pos{ 0.0f };
    float       yawDeg = 0.0f;
    float       pitchDeg = 0.0f;
    float       rollDeg = 0.0f;
    float       scale = 1.0f;
    bool        mirrorX = false;
    bool        mirrorZ = false;
};

struct ProjectSprueData
{
    bool      placed = false;
    glm::vec3 worldPos{ 0.0f };
    glm::vec3 pathStart{ 0.0f };
    glm::vec3 pathEnd{ 0.0f };
    glm::vec3 partingPos{ 0.0f };
    bool      hasPartingPoint = false;
    bool      isDirectInjection = false;
    float     radius = 2.5f;
    float     draftAngleDeg = 1.0f;
    float     coldSlugDepth = 5.0f;

    // The injection point that was active when the sprue was placed
    InjectionPoint injectionPoint;
};

// One control point of a feature's complex (authored) path. Mirrors the engine's
// PathNode but kept independent so the serialization layer carries no dependency
// on the geometry/GL headers. Used by vents (v3+) and runners (v4+).
struct ProjectPathNode
{
    glm::vec3 pos{ 0.0f };
    glm::vec3 dir{ 0.0f, 0.0f, 1.0f };
    float     handleLen = 0.0f;
    // Explicit independent tangent handles (offsets from pos). Older files omit
    // these tokens on the node line; defaults below preserve the earlier
    // symmetric behaviour (handles get re-derived from dir/handleLen on load).
    glm::vec3 handleIn{ 0.0f };
    glm::vec3 handleOut{ 0.0f };
    bool      handlesLinked = true;
    bool      handlesManual = false;
};

struct ProjectRunnerData
{
    glm::vec3 point{ 0.0f };

    // Complex (authored) path persistence (v4+). Same scheme as ProjectVentData:
    // isComplex=false → the historical point-only runner, re-routed Simple on
    // load (feed point -> point), so old files and simple runners need none of
    // the fields below. When true, `nodes` (>= 2, authored on the parting plane)
    // and `smooth` describe the path verbatim — node[0] is the sprue feed point,
    // nodes.back() is the endpoint (== point).
    bool      isComplex = false;
    bool      smooth = false;
    std::vector<ProjectPathNode> nodes;
};

struct ProjectGateData
{
    glm::vec3 pos{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };

    // Parent-object association (v2+). parentIndex < 0 = unparented; v1
    // files load with parentIndex defaulted to -1 so they round-trip as
    // before. localPos / localNormal are valid when parentIndex >= 0.
    int       parentIndex = -1;
    glm::vec3 localPos{ 0.0f };
    glm::vec3 localNormal{ 0.0f, 0.0f, 1.0f };
};

struct ProjectVentData
{
    glm::vec3 pos{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };

    // Parent-object association (v2+). See ProjectGateData.
    int       parentIndex = -1;
    glm::vec3 localPos{ 0.0f };
    glm::vec3 localNormal{ 0.0f, 0.0f, 1.0f };

    // Complex-path persistence (v3+). isComplex=false → the historical derived
    // Simple path, re-routed on load via ComputeVentPath (so old files and
    // simple vents need none of the fields below). When true, `nodes` (>= 2
    // entries, authored on the parting plane) and `smooth` fully describe the
    // path — it cannot be re-derived, so it must round-trip verbatim.
    bool      isComplex = false;
    bool      smooth = false;
    std::vector<ProjectPathNode> nodes;
};

// Ejectors carry only a world-space point at present — no normal (none of the
// snap surfaces share a normal concept), no parent association (sticky
// placement isn't wired up for ejectors yet, mirroring how the feature
// shipped). Diameter and length are global UI parameters and live on
// ProjectParameters, exactly like the other geometry features.
struct ProjectEjectorData
{
    glm::vec3 point{ 0.0f };
};

struct ProjectParameters
{
    float ventWidth = 2.0f;
    float ventLength = 5.0f;
    float ventOverrunStart = 0.5f;
    float ventOverrunEnd = 0.5f;

    float sprueDiameter = 5.0f;
    float sprueDraftAngle = 1.0f;
    float sprueColdSlugDepth = 5.0f;
    float sprueLength = 20.0f;

    float runnerDiameter = 5.0f;
    float runnerColdPlugDist = 5.0f;

    float gateDiameter = 3.0f;
    float gateDraftAngle = 1.0f;
    float gateOverrun = 0.0f;          // mm extension backward into the model
    float subRunnerDiameter = 5.0f;

    float ejectorDiameter = 3.0f;
    float ejectorLength = 25.0f;
};

struct ProjectData
{
    int         version = 1;
    std::string fixturePath;         // path to the .fixture file

    std::vector<ProjectObjectData> objects;

    ProjectParameters params;
    ProjectSprueData  sprue;

    std::vector<ProjectRunnerData>  runners;
    std::vector<ProjectGateData>    gates;
    std::vector<ProjectVentData>    vents;
    std::vector<ProjectEjectorData> ejectors;
};

// ---------------------------------------------------------------------------
// ProjectFile — load / save a .m3d project file (INI-style, matching the
//               existing FixtureFile convention).
// ---------------------------------------------------------------------------
class ProjectFile
{
public:
    static bool Load(const std::string& path,
        ProjectData& out,
        std::string& error);

    static bool Save(const std::string& path,
        const ProjectData& data,
        std::string& error);

private:
    static std::string GetDirectory(const std::string& path);
    static std::string MakeRelative(const std::string& absPath,
        const std::string& baseDir);
    static std::string ResolveRelative(const std::string& relPath,
        const std::string& baseDir);
};
