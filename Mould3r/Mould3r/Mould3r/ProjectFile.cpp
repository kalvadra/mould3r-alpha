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
    // v2: added parentIndex / localPos / localNormal to [gate.N] and [vent.N]
    // for sticky-placement (vents and gates track their parent objects through
    // transforms and patterning). v1 files round-trip cleanly: missing keys
    // load as parentIndex=-1 (unparented), preserving the old behaviour.
    // v3: complex (authored) vent paths (pathKind/smooth/node lines under
    // [vent.N]). v4: the same complex-path lines under [runner.N]. v5: the same
    // under [gate.N] for authored SUB-RUNNER routes (the gate frustum itself is
    // still rebuilt from the card fields, never serialized). The reader is
    // tolerant (presence of pathKind/node lines drives it, not the version
    // number), so older readers ignore the new lines and older files load as
    // simple — the version is informational.
    file << "version = 7\n";
    // Fixture: a Library fixture stores its .fixture path (as before). A
    // procedural fixture stores a kind + its parameters instead — no path, no
    // geometry (Dynamic re-fits the scene on load). Absence of fixture_kind
    // means Library, so pre-v7 files round-trip unchanged.
    if (data.fixtureKind == FixtureKind::Parametric)
    {
        file << "fixture_kind   = parametric\n";
        file << "fixture_size_x = " << data.fixtureParametric.sizeX << "\n";
        file << "fixture_size_y = " << data.fixtureParametric.sizeY << "\n";
        file << "fixture_size_z = " << data.fixtureParametric.sizeZ << "\n";
    }
    else if (data.fixtureKind == FixtureKind::Dynamic)
    {
        file << "fixture_kind        = dynamic\n";
        file << "fixture_clearance_x = " << data.fixtureDynamic.clearanceX << "\n";
        file << "fixture_clearance_y = " << data.fixtureDynamic.clearanceY << "\n";
        file << "fixture_clearance_z = " << data.fixtureDynamic.clearanceZ << "\n";
    }
    else if (!data.fixturePath.empty())
    {
        file << "fixture = " << MakeRelative(data.fixturePath, baseDir) << "\n";
    }

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
    file << "sprueOverrun     = " << data.params.sprueOverrun << "\n";
    file << "runnerDiameter   = " << data.params.runnerDiameter << "\n";
    file << "runnerColdPlugDist = " << data.params.runnerColdPlugDist << "\n";
    file << "gateDiameter     = " << data.params.gateDiameter << "\n";
    file << "gateDraftAngle   = " << data.params.gateDraftAngle << "\n";
    file << "subRunnerDiameter = " << data.params.subRunnerDiameter << "\n";
    file << "ejectorDiameter   = " << data.params.ejectorDiameter << "\n";
    file << "ejectorLength     = " << data.params.ejectorLength << "\n";
    file << "indexerRadius     = " << data.params.indexerRadius << "\n";
    file << "indexerExtraTolerance = " << data.params.indexerExtraTolerance << "\n";

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
        if (sp.injectionPoint.perimeter)
            file << "ipPerimeter = true\n";
    }

    // -- [runner.N] ----------------------------------------------------------
    for (int i = 0; i < (int)data.runners.size(); ++i)
    {
        const auto& rn = data.runners[i];
        file << "\n[runner." << i << "]\n";
        file << "pointX = " << rn.point.x << "\n";
        file << "pointY = " << rn.point.y << "\n";
        file << "pointZ = " << rn.point.z << "\n";

        // Complex (authored) path (v4+). Simple runners write only the point
        // above and re-route on load; a complex runner dumps its node list plus
        // the kind/smooth flags, identical in layout to the [vent.N] node lines.
        // Layout: "node = px py pz dx dy dz handleLen hInxyz hOutxyz linked manual".
        if (rn.isComplex)
        {
            file << "pathKind = complex\n";
            file << "smooth   = " << (rn.smooth ? "true" : "false") << "\n";
            for (const auto& nd : rn.nodes)
            {
                file << "node = "
                    << nd.pos.x << " " << nd.pos.y << " " << nd.pos.z << " "
                    << nd.dir.x << " " << nd.dir.y << " " << nd.dir.z << " "
                    << nd.handleLen << " "
                    << nd.handleIn.x << " " << nd.handleIn.y << " " << nd.handleIn.z << " "
                    << nd.handleOut.x << " " << nd.handleOut.y << " " << nd.handleOut.z << " "
                    << (nd.handlesLinked ? 1 : 0) << " "
                    << (nd.handlesManual ? 1 : 0) << "\n";
            }
        }
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
        // v2 sticky-placement fields. Always written; loader treats them
        // as optional so v1 files round-trip.
        file << "parentIndex  = " << gt.parentIndex << "\n";
        file << "localPosX    = " << gt.localPos.x << "\n";
        file << "localPosY    = " << gt.localPos.y << "\n";
        file << "localPosZ    = " << gt.localPos.z << "\n";
        file << "localNormalX = " << gt.localNormal.x << "\n";
        file << "localNormalY = " << gt.localNormal.y << "\n";
        file << "localNormalZ = " << gt.localNormal.z << "\n";

        // Complex (authored) SUB-RUNNER path (v5+). Simple gates write only the
        // point/normal/parent fields above and re-derive the straight sub-runner
        // on load; a complex gate dumps its sub-runner node list plus the
        // kind/smooth flags, identical in layout to the [runner.N] / [vent.N]
        // node lines. node[0] is the gate origin, nodes.back() the feed attach.
        // The gate frustum is NOT serialized — it rebuilds from the card fields.
        if (gt.isComplex)
        {
            file << "pathKind = complex\n";
            file << "smooth   = " << (gt.smooth ? "true" : "false") << "\n";
            for (const auto& nd : gt.nodes)
            {
                file << "node = "
                    << nd.pos.x << " " << nd.pos.y << " " << nd.pos.z << " "
                    << nd.dir.x << " " << nd.dir.y << " " << nd.dir.z << " "
                    << nd.handleLen << " "
                    << nd.handleIn.x << " " << nd.handleIn.y << " " << nd.handleIn.z << " "
                    << nd.handleOut.x << " " << nd.handleOut.y << " " << nd.handleOut.z << " "
                    << (nd.handlesLinked ? 1 : 0) << " "
                    << (nd.handlesManual ? 1 : 0) << "\n";
            }
        }
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
        file << "parentIndex  = " << vn.parentIndex << "\n";
        file << "localPosX    = " << vn.localPos.x << "\n";
        file << "localPosY    = " << vn.localPos.y << "\n";
        file << "localPosZ    = " << vn.localPos.z << "\n";
        file << "localNormalX = " << vn.localNormal.x << "\n";
        file << "localNormalY = " << vn.localNormal.y << "\n";
        file << "localNormalZ = " << vn.localNormal.z << "\n";

        // Complex (authored) path (v3+). Simple vents write nothing extra and
        // re-derive on load exactly as before. A complex path is dumped as its
        // node list — one line per node, in order — plus the kind/smooth flags.
        // Layout: "node = px py pz dx dy dz handleLen | hInxyz hOutxyz linked manual".
        // The trailing handle tokens are v4+; v3 readers / files stop after
        // handleLen and the defaults cover the rest.
        if (vn.isComplex)
        {
            file << "pathKind = complex\n";
            file << "smooth   = " << (vn.smooth ? "true" : "false") << "\n";
            for (const auto& nd : vn.nodes)
            {
                file << "node = "
                    << nd.pos.x << " " << nd.pos.y << " " << nd.pos.z << " "
                    << nd.dir.x << " " << nd.dir.y << " " << nd.dir.z << " "
                    << nd.handleLen << " "
                    << nd.handleIn.x << " " << nd.handleIn.y << " " << nd.handleIn.z << " "
                    << nd.handleOut.x << " " << nd.handleOut.y << " " << nd.handleOut.z << " "
                    << (nd.handlesLinked ? 1 : 0) << " "
                    << (nd.handlesManual ? 1 : 0) << "\n";
            }
        }
    }

    // -- [ejector.N] ---------------------------------------------------------
    // Just the world-space point � no normal (snap surfaces don't share a
    // normal concept) and no parent fields (sticky-placement isn't wired up
    // for ejectors yet). Older parsers will ignore unknown sections via the
    // Section::None fallback in the reader, so adding this is safe.
    for (int i = 0; i < (int)data.ejectors.size(); ++i)
    {
        const auto& ej = data.ejectors[i];
        file << "\n[ejector." << i << "]\n";
        file << "posX = " << ej.point.x << "\n";
        file << "posY = " << ej.point.y << "\n";
        file << "posZ = " << ej.point.z << "\n";
    }

    // -- [indexer.N] -----------------------------------------------------
    // Just the world-space point (always y=0). Mirrors [ejector.N]. Older
    // parsers ignore this section via the Section::None fallback.
    for (int i = 0; i < (int)data.indexers.size(); ++i)
    {
        const auto& idx = data.indexers[i];
        file << "\n[indexer." << i << "]\n";
        file << "posX = " << idx.point.x << "\n";
        file << "posY = " << idx.point.y << "\n";
        file << "posZ = " << idx.point.z << "\n";
    }

    // -- [insert.N] ----------------------------------------------------------
    // An imported body parented to object[parentIndex], with a local offset,
    // rotation and uniform scale. The body is re-imported from `path` on load,
    // exactly like [object.N]. Unknown to older parsers, which skip it via the
    // Section::None fallback.
    for (int i = 0; i < (int)data.inserts.size(); ++i)
    {
        const auto& in = data.inserts[i];
        file << "\n[insert." << i << "]\n";
        file << "path   = " << MakeRelative(in.sourcePath, baseDir) << "\n";
        file << "parent = " << in.parentIndex << "\n";
        file << "offX   = " << in.localOffset.x << "\n";
        file << "offY   = " << in.localOffset.y << "\n";
        file << "offZ   = " << in.localOffset.z << "\n";
        file << "rotX   = " << in.localRotDeg.x << "\n";
        file << "rotY   = " << in.localRotDeg.y << "\n";
        file << "rotZ   = " << in.localRotDeg.z << "\n";
        file << "scale  = " << in.localScale << "\n";
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
        None, Project, Parameters, Object, Sprue, Runner, Gate, Vent, Ejector,
        Indexer, Insert
    };
    Section currentSection = Section::None;

    // Pending items for numbered sections
    ProjectObjectData  pendingObj;     bool hasObj = false;
    ProjectRunnerData  pendingRun;     bool hasRun = false;
    ProjectGateData    pendingGate;    bool hasGate = false;
    ProjectVentData    pendingVent;    bool hasVent = false;
    ProjectEjectorData pendingEjector; bool hasEjector = false;
    ProjectIndexerData pendingIndexer; bool hasIndexer = false;
    ProjectInsertData  pendingInsert;  bool hasInsert = false;

    auto commitPending = [&]()
        {
            if (hasObj) { out.objects.push_back(pendingObj);      pendingObj = {};     hasObj = false; }
            if (hasRun) { out.runners.push_back(pendingRun);      pendingRun = {};     hasRun = false; }
            if (hasGate) { out.gates.push_back(pendingGate);       pendingGate = {};    hasGate = false; }
            if (hasVent) { out.vents.push_back(pendingVent);       pendingVent = {};    hasVent = false; }
            if (hasEjector) { out.ejectors.push_back(pendingEjector); pendingEjector = {}; hasEjector = false; }
            if (hasIndexer) { out.indexers.push_back(pendingIndexer); pendingIndexer = {}; hasIndexer = false; }
            if (hasInsert) { out.inserts.push_back(pendingInsert);   pendingInsert = {};  hasInsert = false; }
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
            else if (sec.rfind("ejector.", 0) == 0) { currentSection = Section::Ejector; hasEjector = true; }
            else if (sec.rfind("indexer.", 0) == 0) { currentSection = Section::Indexer; hasIndexer = true; }
            else if (sec.rfind("insert.", 0) == 0) { currentSection = Section::Insert; hasInsert = true; }
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
            else if (key == "fixture_kind")
            {
                if (val == "parametric")   out.fixtureKind = FixtureKind::Parametric;
                else if (val == "dynamic") out.fixtureKind = FixtureKind::Dynamic;
                else                       out.fixtureKind = FixtureKind::Library;
            }
            else if (key == "fixture_size_x") out.fixtureParametric.sizeX = ParseFloat(val, out.fixtureParametric.sizeX);
            else if (key == "fixture_size_y") out.fixtureParametric.sizeY = ParseFloat(val, out.fixtureParametric.sizeY);
            else if (key == "fixture_size_z") out.fixtureParametric.sizeZ = ParseFloat(val, out.fixtureParametric.sizeZ);
            else if (key == "fixture_clearance_x") out.fixtureDynamic.clearanceX = ParseFloat(val, out.fixtureDynamic.clearanceX);
            else if (key == "fixture_clearance_y") out.fixtureDynamic.clearanceY = ParseFloat(val, out.fixtureDynamic.clearanceY);
            else if (key == "fixture_clearance_z") out.fixtureDynamic.clearanceZ = ParseFloat(val, out.fixtureDynamic.clearanceZ);
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
            else if (key == "sprueOverrun")      p.sprueOverrun = ParseFloat(val, p.sprueOverrun);
            else if (key == "runnerDiameter")    p.runnerDiameter = ParseFloat(val, p.runnerDiameter);
            else if (key == "runnerColdPlugDist") p.runnerColdPlugDist = ParseFloat(val, p.runnerColdPlugDist);
            else if (key == "gateDiameter")      p.gateDiameter = ParseFloat(val, p.gateDiameter);
            else if (key == "gateDraftAngle")    p.gateDraftAngle = ParseFloat(val, p.gateDraftAngle);
            else if (key == "subRunnerDiameter") p.subRunnerDiameter = ParseFloat(val, p.subRunnerDiameter);
            else if (key == "ejectorDiameter")   p.ejectorDiameter = ParseFloat(val, p.ejectorDiameter);
            else if (key == "ejectorLength")     p.ejectorLength = ParseFloat(val, p.ejectorLength);
            else if (key == "indexerRadius")     p.indexerRadius = ParseFloat(val, p.indexerRadius);
            else if (key == "indexerExtraTolerance") p.indexerExtraTolerance = ParseFloat(val, p.indexerExtraTolerance);
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
            else if (key == "ipPerimeter")   sp.injectionPoint.perimeter = (val == "true" || val == "1" || val == "yes");
            break;
        }

        case Section::Runner:
            if (key == "pointX") pendingRun.point.x = ParseFloat(val, 0.0f);
            else if (key == "pointY") pendingRun.point.y = ParseFloat(val, 0.0f);
            else if (key == "pointZ") pendingRun.point.z = ParseFloat(val, 0.0f);
            // v4 complex-path keys. Absent in older files / simple runners, so
            // isComplex defaults false and the path is re-derived Simple on load.
            else if (key == "pathKind") pendingRun.isComplex = (val == "complex");
            else if (key == "smooth")   pendingRun.smooth = ParseBool(val);
            else if (key == "node")
            {
                // Same layout as the [vent.N] node line; the bracketed handle
                // tokens are optional (older lines stop after handleLen and the
                // DTO defaults stand — handles re-derive from dir/handleLen).
                std::istringstream iss(val);
                ProjectPathNode pn;
                iss >> pn.pos.x >> pn.pos.y >> pn.pos.z
                    >> pn.dir.x >> pn.dir.y >> pn.dir.z >> pn.handleLen;
                iss >> pn.handleIn.x >> pn.handleIn.y >> pn.handleIn.z
                    >> pn.handleOut.x >> pn.handleOut.y >> pn.handleOut.z;
                int linked = 1, manual = 0;
                if (iss >> linked) pn.handlesLinked = (linked != 0);
                if (iss >> manual) pn.handlesManual = (manual != 0);
                pendingRun.nodes.push_back(pn);
            }
            break;

        case Section::Gate:
            if (key == "posX")    pendingGate.pos.x = ParseFloat(val, 0.0f);
            else if (key == "posY")    pendingGate.pos.y = ParseFloat(val, 0.0f);
            else if (key == "posZ")    pendingGate.pos.z = ParseFloat(val, 0.0f);
            else if (key == "normalX") pendingGate.normal.x = ParseFloat(val, 0.0f);
            else if (key == "normalY") pendingGate.normal.y = ParseFloat(val, 1.0f);
            else if (key == "normalZ") pendingGate.normal.z = ParseFloat(val, 0.0f);
            // v2 sticky-placement keys. Missing in v1 files; defaults on
            // the struct (-1 / zero / +Z) preserve the old "world-anchored"
            // behaviour for those.
            else if (key == "parentIndex")  pendingGate.parentIndex = ParseInt(val, -1);
            else if (key == "localPosX")    pendingGate.localPos.x = ParseFloat(val, 0.0f);
            else if (key == "localPosY")    pendingGate.localPos.y = ParseFloat(val, 0.0f);
            else if (key == "localPosZ")    pendingGate.localPos.z = ParseFloat(val, 0.0f);
            else if (key == "localNormalX") pendingGate.localNormal.x = ParseFloat(val, 0.0f);
            else if (key == "localNormalY") pendingGate.localNormal.y = ParseFloat(val, 0.0f);
            else if (key == "localNormalZ") pendingGate.localNormal.z = ParseFloat(val, 1.0f);
            // v5 complex sub-runner keys. Absent in older files / simple gates,
            // so isComplex defaults false and the sub-runner re-derives Simple on
            // load. Same node-line layout as [runner.N] / [vent.N].
            else if (key == "pathKind") pendingGate.isComplex = (val == "complex");
            else if (key == "smooth")   pendingGate.smooth = ParseBool(val);
            else if (key == "node")
            {
                std::istringstream iss(val);
                ProjectPathNode pn;
                iss >> pn.pos.x >> pn.pos.y >> pn.pos.z
                    >> pn.dir.x >> pn.dir.y >> pn.dir.z >> pn.handleLen;
                iss >> pn.handleIn.x >> pn.handleIn.y >> pn.handleIn.z
                    >> pn.handleOut.x >> pn.handleOut.y >> pn.handleOut.z;
                int linked = 1, manual = 0;
                if (iss >> linked) pn.handlesLinked = (linked != 0);
                if (iss >> manual) pn.handlesManual = (manual != 0);
                pendingGate.nodes.push_back(pn);
            }
            break;

        case Section::Vent:
            if (key == "posX")    pendingVent.pos.x = ParseFloat(val, 0.0f);
            else if (key == "posY")    pendingVent.pos.y = ParseFloat(val, 0.0f);
            else if (key == "posZ")    pendingVent.pos.z = ParseFloat(val, 0.0f);
            else if (key == "normalX") pendingVent.normal.x = ParseFloat(val, 0.0f);
            else if (key == "normalY") pendingVent.normal.y = ParseFloat(val, 1.0f);
            else if (key == "normalZ") pendingVent.normal.z = ParseFloat(val, 0.0f);
            // v2 sticky-placement keys.
            else if (key == "parentIndex")  pendingVent.parentIndex = ParseInt(val, -1);
            else if (key == "localPosX")    pendingVent.localPos.x = ParseFloat(val, 0.0f);
            else if (key == "localPosY")    pendingVent.localPos.y = ParseFloat(val, 0.0f);
            else if (key == "localPosZ")    pendingVent.localPos.z = ParseFloat(val, 0.0f);
            else if (key == "localNormalX") pendingVent.localNormal.x = ParseFloat(val, 0.0f);
            else if (key == "localNormalY") pendingVent.localNormal.y = ParseFloat(val, 0.0f);
            else if (key == "localNormalZ") pendingVent.localNormal.z = ParseFloat(val, 1.0f);
            // v3 complex-path keys. Absent in older files / simple vents, so
            // isComplex defaults false and the path is re-derived as before.
            else if (key == "pathKind") pendingVent.isComplex = (val == "complex");
            else if (key == "smooth")   pendingVent.smooth = ParseBool(val);
            else if (key == "node")
            {
                // "px py pz dx dy dz handleLen [hInxyz hOutxyz linked manual]".
                // The bracketed tokens are v4+; on a v3 line those extractions
                // fail and the DTO defaults (zero handles, linked, not manual)
                // stand — the loader re-derives handles from dir/handleLen.
                std::istringstream iss(val);
                ProjectPathNode pn;
                iss >> pn.pos.x >> pn.pos.y >> pn.pos.z
                    >> pn.dir.x >> pn.dir.y >> pn.dir.z >> pn.handleLen;
                iss >> pn.handleIn.x >> pn.handleIn.y >> pn.handleIn.z
                    >> pn.handleOut.x >> pn.handleOut.y >> pn.handleOut.z;
                int linked = 1, manual = 0;
                if (iss >> linked) pn.handlesLinked = (linked != 0);
                if (iss >> manual) pn.handlesManual = (manual != 0);
                pendingVent.nodes.push_back(pn);
            }
            break;

        case Section::Ejector:
            // Just the world point. New keys (parent / local placement) can
            // slot in here later if sticky-placement is added; defaults on
            // the struct will preserve current behaviour for older files.
            if (key == "posX")    pendingEjector.point.x = ParseFloat(val, 0.0f);
            else if (key == "posY") pendingEjector.point.y = ParseFloat(val, 0.0f);
            else if (key == "posZ") pendingEjector.point.z = ParseFloat(val, 0.0f);
            break;

        case Section::Indexer:
            // Just the world point (always y=0). Mirrors Section::Ejector.
            if (key == "posX")    pendingIndexer.point.x = ParseFloat(val, 0.0f);
            else if (key == "posY") pendingIndexer.point.y = ParseFloat(val, 0.0f);
            else if (key == "posZ") pendingIndexer.point.z = ParseFloat(val, 0.0f);
            break;

        case Section::Insert:
            if (key == "path")       pendingInsert.sourcePath = ResolveRelative(val, baseDir);
            else if (key == "parent") pendingInsert.parentIndex = ParseInt(val, -1);
            else if (key == "offX")   pendingInsert.localOffset.x = ParseFloat(val, 0.0f);
            else if (key == "offY")   pendingInsert.localOffset.y = ParseFloat(val, 0.0f);
            else if (key == "offZ")   pendingInsert.localOffset.z = ParseFloat(val, 0.0f);
            else if (key == "rotX")   pendingInsert.localRotDeg.x = ParseFloat(val, 0.0f);
            else if (key == "rotY")   pendingInsert.localRotDeg.y = ParseFloat(val, 0.0f);
            else if (key == "rotZ")   pendingInsert.localRotDeg.z = ParseFloat(val, 0.0f);
            else if (key == "scale")  pendingInsert.localScale = ParseFloat(val, 1.0f);
            break;

        default:
            break;
        }
    }

    // Commit any trailing item
    commitPending();

    // Enforce the type-from-Y invariant on the active sprue's snapshotted
    // injection point. Done at end-of-load rather than at the ipType key
    // handler because the file's key order isn't guaranteed — ipType may
    // arrive before ipY, so deriving at parse time wouldn't see the final
    // y value. Older projects with a stale ipType (saved before the rule
    // existed, or hand-edited) get normalised here.
    if (out.sprue.placed)
    {
        // Perimeter injection points are always Radial by definition; their
        // stored local y can be non-zero (fixture pose), so skip the
        // y-derived type — otherwise a perimeter point loads as Axial and its
        // radial editing / geometry break.
        if (out.sprue.injectionPoint.perimeter)
            out.sprue.injectionPoint.type = InjectionType::Radial;
        else
            out.sprue.injectionPoint.type =
                InjectionPoint::TypeFor(out.sprue.injectionPoint.y);
    }

    return true;
}
