#pragma once
#include <optional>
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

// ---------------------------------------------------------------------------
// Per-feature default overrides parsed from the fixture file's optional
// [<feature>_defaults] sections. Each field uses std::optional so the
// fixture can specify exactly which defaults it cares about — anything left
// unset falls back to the application's hardcoded UI defaults at apply
// time. Distance fields are stored in millimetres; angle fields in degrees,
// matching the file format and the rest of the data model.
// ---------------------------------------------------------------------------
struct VentDefaults
{
    std::optional<std::string> type;
    std::optional<float>       length;
    std::optional<float>       width;
    std::optional<float>       overrunStart;
    std::optional<float>       overrunEnd;
};

struct SprueDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
    std::optional<float>       draftAngle;
    std::optional<float>       coldSlugLength;
    std::optional<float>       length;
};

struct RunnerDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
    std::optional<float>       coldSlugLength;
};

struct GateDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
    std::optional<float>       draftAngle;
};

struct SubRunnerDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
};

struct EjectorDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
    std::optional<float>       length;
};

struct FixtureDefinition
{
    std::string modelAPath;   // always stored as absolute internally
    std::string modelBPath;
    std::string fixturePath;  // directory anchor for relative path resolution

    std::vector<InjectionPoint> injectionPoints;

    // Optional per-feature defaults. All fields default-construct to empty
    // optionals — i.e. "no override". MainFrame::ApplyFixtureDefaults walks
    // these and writes only the specified values into the side-panel UI.
    VentDefaults      ventDefaults;
    SprueDefaults     sprueDefaults;
    RunnerDefaults    runnerDefaults;
    GateDefaults      gateDefaults;
    SubRunnerDefaults subRunnerDefaults;
    EjectorDefaults   ejectorDefaults;

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