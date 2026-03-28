#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// InjectionPoint — a sprue/gate location defined relative to the fixture
// origin.  Two physical interpretations are supported:
//
//   Radial  — material injected radially inward (e.g. a side gate on the
//              parting line).
//   Axial   — material injected along the fixture axis (e.g. a top/bottom
//              sprue perpendicular to the parting plane).
// ---------------------------------------------------------------------------
enum class InjectionType { Radial, Axial };

struct InjectionPoint
{
    std::string   label;
    float         x = 0.0f;
    float         y = 0.0f;
    float         z = 0.0f;
    InjectionType type = InjectionType::Radial;
};

struct FixtureDefinition
{
    std::string modelAPath;   // always stored as absolute internally
    std::string modelBPath;
    std::string fixturePath;  // directory anchor for relative path resolution

    std::vector<InjectionPoint> injectionPoints;

    bool IsValid() const
    {
        return !modelAPath.empty() && !modelBPath.empty();
    }
};

class FixtureFile
{
public:
    static bool Load(const std::string& path,
        FixtureDefinition& out,
        std::string& error);

    static bool Save(const std::string& path,
        const FixtureDefinition& def,
        std::string& error);

private:
    // Get the directory component of a path
    static std::string GetDirectory(const std::string& path);

    // Make absPath relative to baseDir
    static std::string MakeRelative(const std::string& absPath,
        const std::string& baseDir);

    // Resolve a relative path against baseDir to get an absolute path
    static std::string ResolveRelative(const std::string& relPath,
        const std::string& baseDir);
};