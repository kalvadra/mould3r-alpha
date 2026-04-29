#include "ProjectFile.h"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers (same as FixtureFile)
// ---------------------------------------------------------------------------
static std::string Trim(const std::string& s)
{
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static float ParseFloat(const std::string& s, float def)
{
    try { return std::stof(s); }
    catch (...) { return def; }
}

static int ParseInt(const std::string& s, int def)
{
    try { return std::stoi(s); }
    catch (...) { return def; }
}

static bool ParseBool(const std::string& s)
{
    return (s == "true" || s == "1" || s == "yes");
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
std::string ProjectFile::GetDirectory(const std::string& path)
{
    return fs::path(path).parent_path().string();
}

std::string ProjectFile::MakeRelative(const std::string& absPath,
    const std::string& baseDir)
{
    if (absPath.empty()) return "";
    try {
        return fs::relative(fs::path(absPath), fs::path(baseDir)).string();
    }
    catch (...) {
        return absPath;
    }
}

std::string ProjectFile::ResolveRelative(const std::string& relPath,
    const std::string& baseDir)
{
    if (relPath.empty()) return "";
    try {
        fs::path resolved = fs::path(baseDir) / fs::path(relPath);
        return fs::canonical(resolved).string();
    }
    catch (...) {
        return relPath;
    }
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
bool ProjectFile::Save(const std::string& path,
    const ProjectData& data,
    std::string& error)
{
    std::ofstream file(path);
    if (!file.is_open())
    {
        error = "Could not write file: " + path;
        return false;
    }

    const std::string baseDir = GetDirectory(path);

    // -- [project] -----------------------------------------------------------
    file << "[project]\n";
    file << "version = " << data.version << "\n";
    if (!data.fixturePath.empty())
        file << "fixture = " << MakeRelative(data.fixturePath, baseDir) << "\n";

    // -- [parameters] --------------------------------------------------------
    file << "\n[parameters]\n";
    file << "ventWidth        = " << data.params.ventWidth << "\n";
    file << "ventLength       = " << data.params.ventLength << "\n";
    file << "ventOverrunStart = " << data.params.ventOverrunStart << "\n";
    file << "ventOverrunEnd   = " << data.params.ventOverrunEnd << "\n";
    file << "sprueDiameter    = " << data.params.sprueDiameter << "\n";
    file << "sprueDraftAngle  = " << data.params.sprueDraftAngle << "\n";
    file << "sprueColdSlugDepth = " << data.params.sprueColdSlugDepth << "\n";
    file << "sprueLength      = " << data.params.sprueLength << "\n";
    file << "runnerDiameter   = " << data.params.runnerDiameter << "\n";
    file << "runnerColdPlugDist = " << data.params.runnerColdPlugDist << "\n";
    file << "gateDiameter     = " << data.params.gateDiameter << "\n";
    file << "gateDraftAngle   = " << data.params.gateDraftAngle << "\n";
    file << "subRunnerDiameter = " << data.params.subRunnerDiameter << "\n";

    // -- [object.N] ----------------------------------------------------------
    for (int i = 0; i < (int)data.objects.size(); ++i)
    {
        const auto& obj = data.objects[i];
        file << "\n[object." << i << "]\n";
        file << "path  = " << MakeRelative(obj.sourcePath, baseDir) << "\n";
        file << "posX  = " << obj.pos.x << "\n";
        file << "posY  = " << obj.pos.y << "\n";
        file << "posZ  = " << obj.pos.z << "\n";
        file << "yaw   = " << obj.yawDeg << "\n";
        file << "pitch = " << obj.pitchDeg << "\n";
        file << "roll  = " << obj.rollDeg << "\n";
        file << "scale = " << obj.scale << "\n";
        file << "mirrorX = " << (obj.mirrorX ? "true" : "false") << "\n";
        file << "mirrorZ = " << (obj.mirrorZ ? "true" : "false") << "\n";
    }

    // -- [sprue] -------------------------------------------------------------
    if (data.sprue.placed)
    {
        const auto& sp = data.sprue;
        file << "\n[sprue]\n";
        file << "worldPosX     = " << sp.worldPos.x << "\n";
        file << "worldPosY     = " << sp.worldPos.y << "\n";
        file << "worldPosZ     = " << sp.worldPos.z << "\n";
        file << "pathStartX    = " << sp.pathStart.x << "\n";
        file << "pathStartY    = " << sp.pathStart.y << "\n";
        file << "pathStartZ    = " << sp.pathStart.z << "\n";
        file << "pathEndX      = " << sp.pathEnd.x << "\n";
        file << "pathEndY      = " << sp.pathEnd.y << "\n";
        file << "pathEndZ      = " << sp.pathEnd.z << "\n";
        file << "partingPosX   = " << sp.partingPos.x << "\n";
        file << "partingPosY   = " << sp.partingPos.y << "\n";
        file << "partingPosZ   = " << sp.partingPos.z << "\n";
        file << "hasPartingPoint   = " << (sp.hasPartingPoint ? "true" : "false") << "\n";
        file << "isDirectInjection = " << (sp.isDirectInjection ? "true" : "false") << "\n";
        file << "radius        = " << sp.radius << "\n";
        file << "draftAngleDeg = " << sp.draftAngleDeg << "\n";
        file << "coldSlugDepth = " << sp.coldSlugDepth << "\n";

        // Injection point that was active
        file << "ipLabel = " << sp.injectionPoint.label << "\n";
        file << "ipType  = " << (sp.injectionPoint.type == InjectionType::Axial ? "axial" : "radial") << "\n";
        file << "ipX     = " << sp.injectionPoint.x << "\n";
        file << "ipY     = " << sp.injectionPoint.y << "\n";
        file << "ipZ     = " << sp.injectionPoint.z << "\n";
    }

    // -- [runner.N] ----------------------------------------------------------
    for (int i = 0; i < (int)data.runners.size(); ++i)
    {
        const auto& rn = data.runners[i];
        file << "\n[runner." << i << "]\n";
        file << "pointX = " << rn.point.x << "\n";
        file << "pointY = " << rn.point.y << "\n";
        file << "pointZ = " << rn.point.z << "\n";
    }

    // -- [gate.N] ------------------------------------------------------------
    for (int i = 0; i < (int)data.gates.size(); ++i)
    {
        const auto& gt = data.gates[i];
        file << "\n[gate." << i << "]\n";
        file << "posX    = " << gt.pos.x << "\n";
        file << "posY    = " << gt.pos.y << "\n";
        file << "posZ    = " << gt.pos.z << "\n";
        file << "normalX = " << gt.normal.x << "\n";
        file << "normalY = " << gt.normal.y << "\n";
        file << "normalZ = " << gt.normal.z << "\n";
    }

    // -- [vent.N] ------------------------------------------------------------
    for (int i = 0; i < (int)data.vents.size(); ++i)
    {
        const auto& vn = data.vents[i];
        file << "\n[vent." << i << "]\n";
        file << "posX    = " << vn.pos.x << "\n";
        file << "posY    = " << vn.pos.y << "\n";
        file << "posZ    = " << vn.pos.z << "\n";
        file << "normalX = " << vn.normal.x << "\n";
        file << "normalY = " << vn.normal.y << "\n";
        file << "normalZ = " << vn.normal.z << "\n";
    }

    return true;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
bool ProjectFile::Load(const std::string& path,
    ProjectData& out,
    std::string& error)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        error = "Could not open file: " + path;
        return false;
    }

    const std::string baseDir = GetDirectory(path);
    out = ProjectData{};   // reset

    // Section tracking
    enum class Section {
        None, Project, Parameters, Object, Sprue, Runner, Gate, Vent
    };
    Section currentSection = Section::None;

    // Pending items for numbered sections
    ProjectObjectData pendingObj;   bool hasObj = false;
    ProjectRunnerData pendingRun;   bool hasRun = false;
    ProjectGateData   pendingGate;  bool hasGate = false;
    ProjectVentData   pendingVent;  bool hasVent = false;

    auto commitPending = [&]()
        {
            if (hasObj) { out.objects.push_back(pendingObj);  pendingObj = {}; hasObj = false; }
            if (hasRun) { out.runners.push_back(pendingRun);  pendingRun = {}; hasRun = false; }
            if (hasGate) { out.gates.push_back(pendingGate);   pendingGate = {}; hasGate = false; }
            if (hasVent) { out.vents.push_back(pendingVent);   pendingVent = {}; hasVent = false; }
        };

    std::string line;
    while (std::getline(file, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;

        // ---- Section header ------------------------------------------------
        if (line[0] == '[')
        {
            commitPending();

            const std::string sec = Trim(line.substr(1, line.size() - 2));

            if (sec == "project")                      currentSection = Section::Project;
            else if (sec == "parameters")              currentSection = Section::Parameters;
            else if (sec == "sprue") { currentSection = Section::Sprue; out.sprue.placed = true; }
            else if (sec.rfind("object.", 0) == 0) { currentSection = Section::Object; hasObj = true; }
            else if (sec.rfind("runner.", 0) == 0) { currentSection = Section::Runner; hasRun = true; }
            else if (sec.rfind("gate.", 0) == 0) { currentSection = Section::Gate;   hasGate = true; }
            else if (sec.rfind("vent.", 0) == 0) { currentSection = Section::Vent;   hasVent = true; }
            else                                       currentSection = Section::None;

            continue;
        }

        // ---- Key = value ---------------------------------------------------
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));

        switch (currentSection)
        {
        case Section::Project:
            if (key == "version")      out.version = ParseInt(val, 1);
            else if (key == "fixture") out.fixturePath = ResolveRelative(val, baseDir);
            break;

        case Section::Parameters:
        {
            auto& p = out.params;
            if (key == "ventWidth")         p.ventWidth = ParseFloat(val, p.ventWidth);
            else if (key == "ventLength")        p.ventLength = ParseFloat(val, p.ventLength);
            else if (key == "ventOverrunStart")  p.ventOverrunStart = ParseFloat(val, p.ventOverrunStart);
            else if (key == "ventOverrunEnd")    p.ventOverrunEnd = ParseFloat(val, p.ventOverrunEnd);
            else if (key == "sprueDiameter")     p.sprueDiameter = ParseFloat(val, p.sprueDiameter);
            else if (key == "sprueDraftAngle")   p.sprueDraftAngle = ParseFloat(val, p.sprueDraftAngle);
            else if (key == "sprueColdSlugDepth") p.sprueColdSlugDepth = ParseFloat(val, p.sprueColdSlugDepth);
            else if (key == "sprueLength")       p.sprueLength = ParseFloat(val, p.sprueLength);
            else if (key == "runnerDiameter")    p.runnerDiameter = ParseFloat(val, p.runnerDiameter);
            else if (key == "runnerColdPlugDist") p.runnerColdPlugDist = ParseFloat(val, p.runnerColdPlugDist);
            else if (key == "gateDiameter")      p.gateDiameter = ParseFloat(val, p.gateDiameter);
            else if (key == "gateDraftAngle")    p.gateDraftAngle = ParseFloat(val, p.gateDraftAngle);
            else if (key == "subRunnerDiameter") p.subRunnerDiameter = ParseFloat(val, p.subRunnerDiameter);
            break;
        }

        case Section::Object:
            if (key == "path")  pendingObj.sourcePath = ResolveRelative(val, baseDir);
            else if (key == "posX")  pendingObj.pos.x = ParseFloat(val, 0.0f);
            else if (key == "posY")  pendingObj.pos.y = ParseFloat(val, 0.0f);
            else if (key == "posZ")  pendingObj.pos.z = ParseFloat(val, 0.0f);
            else if (key == "yaw")   pendingObj.yawDeg = ParseFloat(val, 0.0f);
            else if (key == "pitch") pendingObj.pitchDeg = ParseFloat(val, 0.0f);
            else if (key == "roll")  pendingObj.rollDeg = ParseFloat(val, 0.0f);
            else if (key == "scale") pendingObj.scale = ParseFloat(val, 1.0f);
            else if (key == "mirrorX") pendingObj.mirrorX = ParseBool(val);
            else if (key == "mirrorZ") pendingObj.mirrorZ = ParseBool(val);
            break;

        case Section::Sprue:
        {
            auto& sp = out.sprue;
            if (key == "worldPosX")     sp.worldPos.x = ParseFloat(val, 0.0f);
            else if (key == "worldPosY")     sp.worldPos.y = ParseFloat(val, 0.0f);
            else if (key == "worldPosZ")     sp.worldPos.z = ParseFloat(val, 0.0f);
            else if (key == "pathStartX")    sp.pathStart.x = ParseFloat(val, 0.0f);
            else if (key == "pathStartY")    sp.pathStart.y = ParseFloat(val, 0.0f);
            else if (key == "pathStartZ")    sp.pathStart.z = ParseFloat(val, 0.0f);
            else if (key == "pathEndX")      sp.pathEnd.x = ParseFloat(val, 0.0f);
            else if (key == "pathEndY")      sp.pathEnd.y = ParseFloat(val, 0.0f);
            else if (key == "pathEndZ")      sp.pathEnd.z = ParseFloat(val, 0.0f);
            else if (key == "partingPosX")   sp.partingPos.x = ParseFloat(val, 0.0f);
            else if (key == "partingPosY")   sp.partingPos.y = ParseFloat(val, 0.0f);
            else if (key == "partingPosZ")   sp.partingPos.z = ParseFloat(val, 0.0f);
            else if (key == "hasPartingPoint")   sp.hasPartingPoint = ParseBool(val);
            else if (key == "isDirectInjection") sp.isDirectInjection = ParseBool(val);
            else if (key == "radius")        sp.radius = ParseFloat(val, 2.5f);
            else if (key == "draftAngleDeg") sp.draftAngleDeg = ParseFloat(val, 1.0f);
            else if (key == "coldSlugDepth") sp.coldSlugDepth = ParseFloat(val, 5.0f);
            else if (key == "ipLabel")       sp.injectionPoint.label = val;
            else if (key == "ipType")        sp.injectionPoint.type = (val == "axial") ? InjectionType::Axial : InjectionType::Radial;
            else if (key == "ipX")           sp.injectionPoint.x = ParseFloat(val, 0.0f);
            else if (key == "ipY")           sp.injectionPoint.y = ParseFloat(val, 0.0f);
            else if (key == "ipZ")           sp.injectionPoint.z = ParseFloat(val, 0.0f);
            break;
        }

        case Section::Runner:
            if (key == "pointX") pendingRun.point.x = ParseFloat(val, 0.0f);
            else if (key == "pointY") pendingRun.point.y = ParseFloat(val, 0.0f);
            else if (key == "pointZ") pendingRun.point.z = ParseFloat(val, 0.0f);
            break;

        case Section::Gate:
            if (key == "posX")    pendingGate.pos.x = ParseFloat(val, 0.0f);
            else if (key == "posY")    pendingGate.pos.y = ParseFloat(val, 0.0f);
            else if (key == "posZ")    pendingGate.pos.z = ParseFloat(val, 0.0f);
            else if (key == "normalX") pendingGate.normal.x = ParseFloat(val, 0.0f);
            else if (key == "normalY") pendingGate.normal.y = ParseFloat(val, 1.0f);
            else if (key == "normalZ") pendingGate.normal.z = ParseFloat(val, 0.0f);
            break;

        case Section::Vent:
            if (key == "posX")    pendingVent.pos.x = ParseFloat(val, 0.0f);
            else if (key == "posY")    pendingVent.pos.y = ParseFloat(val, 0.0f);
            else if (key == "posZ")    pendingVent.pos.z = ParseFloat(val, 0.0f);
            else if (key == "normalX") pendingVent.normal.x = ParseFloat(val, 0.0f);
            else if (key == "normalY") pendingVent.normal.y = ParseFloat(val, 1.0f);
            else if (key == "normalZ") pendingVent.normal.z = ParseFloat(val, 0.0f);
            break;

        default:
            break;
        }
    }

    // Commit any trailing item
    commitPending();

    return true;
}
