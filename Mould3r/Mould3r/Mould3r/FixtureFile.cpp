#include "FixtureFile.h"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

static std::string Trim(const std::string& s)
{
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string FixtureFile::GetDirectory(const std::string& path)
{
    return fs::path(path).parent_path().string();
}

std::string FixtureFile::MakeRelative(const std::string& absPath,
    const std::string& baseDir)
{
    try {
        return fs::relative(fs::path(absPath),
            fs::path(baseDir)).string();
    }
    catch (...) {
        // If relative calculation fails (e.g. different drives), fall back to absolute
        return absPath;
    }
}

std::string FixtureFile::ResolveRelative(const std::string& relPath,
    const std::string& baseDir)
{
    try {
        fs::path resolved = fs::path(baseDir) / fs::path(relPath);
        return fs::canonical(resolved).string();
    }
    catch (...) {
        // If resolution fails, return as-is
        return relPath;
    }
}

bool FixtureFile::Load(const std::string& path,
    FixtureDefinition& out,
    std::string& error)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        error = "Could not open file: " + path;
        return false;
    }

    const std::string baseDir = GetDirectory(path);
    out.fixturePath = path;
    out.injectionPoints.clear();

    // Section-aware parsing.
    // Sections: [fixture], [injection_point.N], [<feature>_defaults],
    //           [half_a_transform], [half_b_transform]
    // An injection_point block is committed to the list when the next section
    // header (or EOF) is encountered. Defaults sections write directly into
    // their corresponding struct on FixtureDefinition (no commit step needed
    // — there's at most one of each defaults section per file). The half
    // transform sections work the same way — at most one of each — and write
    // straight into out.halfATransform / halfBTransform.
    enum class Section
    {
        None,
        Fixture,
        InjectionPoint,
        VentDefaults,
        SprueDefaults,
        RunnerDefaults,
        GateDefaults,
        SubRunnerDefaults,
        EjectorDefaults,
        HalfATransform,
        HalfBTransform,
    };
    Section     currentSection = Section::None;
    InjectionPoint pendingPoint;
    bool           hasPending = false;

    auto commitPending = [&]()
        {
            if (hasPending)
            {
                out.injectionPoints.push_back(pendingPoint);
                pendingPoint = InjectionPoint{};
                hasPending = false;
            }
        };

    // Helper: parse a numeric value into an optional<float>. Silently
    // ignores malformed input (leaves the optional unset). Used by every
    // defaults section so a typo in one field doesn't take down the rest
    // of the file.
    auto parseOptFloat = [](const std::string& s, std::optional<float>& dst)
        {
            try { dst = std::stof(s); }
            catch (...) { /* leave dst unchanged */ }
        };

    // Helper: parse into a non-optional double, leaving the destination at
    // its prior value (the struct's default of 0/0/0/0/0/0/1) if parsing
    // fails. Used by the half-transform sections — same forgiving stance
    // as parseOptFloat above.
    auto parseDouble = [](const std::string& s, double& dst)
        {
            try { dst = std::stod(s); }
            catch (...) { /* leave dst unchanged */ }
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

            const std::string sectionName = Trim(line.substr(1, line.size() - 2));

            if (sectionName == "fixture")
            {
                currentSection = Section::Fixture;
            }
            else if (sectionName.rfind("injection_point.", 0) == 0)
            {
                currentSection = Section::InjectionPoint;
                hasPending = true;   // start accumulating a new point
            }
            else if (sectionName == "vent_defaults")
            {
                currentSection = Section::VentDefaults;
            }
            else if (sectionName == "sprue_defaults")
            {
                currentSection = Section::SprueDefaults;
            }
            else if (sectionName == "runner_defaults")
            {
                currentSection = Section::RunnerDefaults;
            }
            else if (sectionName == "gate_defaults")
            {
                currentSection = Section::GateDefaults;
            }
            else if (sectionName == "sub_runner_defaults")
            {
                currentSection = Section::SubRunnerDefaults;
            }
            else if (sectionName == "ejector_defaults")
            {
                currentSection = Section::EjectorDefaults;
            }
            else if (sectionName == "half_a_transform")
            {
                currentSection = Section::HalfATransform;
            }
            else if (sectionName == "half_b_transform")
            {
                currentSection = Section::HalfBTransform;
            }
            else
            {
                currentSection = Section::None;
            }
            continue;
        }

        // ---- Key = value ---------------------------------------------------
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));

        if (currentSection == Section::Fixture)
        {
            if (key == "modelA")
                out.modelAPath = ResolveRelative(value, baseDir);
            else if (key == "modelB")
                out.modelBPath = ResolveRelative(value, baseDir);
        }
        else if (currentSection == Section::InjectionPoint && hasPending)
        {
            if (key == "label")
            {
                pendingPoint.label = value;
            }
            else if (key == "type")
            {
                pendingPoint.type = (value == "axial")
                    ? InjectionType::Axial
                    : InjectionType::Radial;
            }
            else if (key == "x")
            {
                try { pendingPoint.x = std::stof(value); }
                catch (...) {}
            }
            else if (key == "y")
            {
                try { pendingPoint.y = std::stof(value); }
                catch (...) {}
            }
            else if (key == "z")
            {
                try { pendingPoint.z = std::stof(value); }
                catch (...) {}
            }
        }
        // ---- Per-feature defaults sections ---------------------------------
        // Each branch routes recognised keys into the corresponding optional
        // field on FixtureDefinition. Unknown keys are silently ignored so
        // forward-compat additions don't break older builds.
        else if (currentSection == Section::VentDefaults)
        {
            if (key == "type")          out.ventDefaults.type = value;
            else if (key == "length")        parseOptFloat(value, out.ventDefaults.length);
            else if (key == "width")         parseOptFloat(value, out.ventDefaults.width);
            else if (key == "overrun_start") parseOptFloat(value, out.ventDefaults.overrunStart);
            else if (key == "overrun_end")   parseOptFloat(value, out.ventDefaults.overrunEnd);
        }
        else if (currentSection == Section::SprueDefaults)
        {
            if (key == "type")             out.sprueDefaults.type = value;
            else if (key == "diameter")         parseOptFloat(value, out.sprueDefaults.diameter);
            else if (key == "draft_angle")      parseOptFloat(value, out.sprueDefaults.draftAngle);
            else if (key == "cold_slug_length") parseOptFloat(value, out.sprueDefaults.coldSlugLength);
            else if (key == "length")           parseOptFloat(value, out.sprueDefaults.length);
        }
        else if (currentSection == Section::RunnerDefaults)
        {
            if (key == "type")             out.runnerDefaults.type = value;
            else if (key == "diameter")         parseOptFloat(value, out.runnerDefaults.diameter);
            else if (key == "cold_slug_length") parseOptFloat(value, out.runnerDefaults.coldSlugLength);
        }
        else if (currentSection == Section::GateDefaults)
        {
            if (key == "type")        out.gateDefaults.type = value;
            else if (key == "diameter")    parseOptFloat(value, out.gateDefaults.diameter);
            else if (key == "draft_angle") parseOptFloat(value, out.gateDefaults.draftAngle);
        }
        else if (currentSection == Section::SubRunnerDefaults)
        {
            if (key == "type")     out.subRunnerDefaults.type = value;
            else if (key == "diameter") parseOptFloat(value, out.subRunnerDefaults.diameter);
        }
        else if (currentSection == Section::EjectorDefaults)
        {
            if (key == "type")     out.ejectorDefaults.type = value;
            else if (key == "diameter") parseOptFloat(value, out.ejectorDefaults.diameter);
            else if (key == "length")   parseOptFloat(value, out.ejectorDefaults.length);
        }
        // ---- Per-half pose sections ----------------------------------------
        // Same forgiving parsing as the defaults sections above — unknown
        // keys silently dropped, malformed numbers leave the destination at
        // its identity default. Both [half_a_transform] and [half_b_transform]
        // share the same key set; the only thing the section header decides
        // is which HalfTransform struct receives the writes.
        else if (currentSection == Section::HalfATransform ||
            currentSection == Section::HalfBTransform)
        {
            HalfTransform& t = (currentSection == Section::HalfATransform)
                ? out.halfATransform : out.halfBTransform;

            if (key == "position_x")      parseDouble(value, t.posX);
            else if (key == "position_y") parseDouble(value, t.posY);
            else if (key == "position_z") parseDouble(value, t.posZ);
            else if (key == "rotation_x") parseDouble(value, t.rotX);
            else if (key == "rotation_y") parseDouble(value, t.rotY);
            else if (key == "rotation_z") parseDouble(value, t.rotZ);
            else if (key == "scale")      parseDouble(value, t.scale);
        }
    }

    // Commit any point that reached EOF without a following section header
    commitPending();

    if (!out.IsValid())
    {
        error = "Fixture file is missing modelA or modelB entries.";
        return false;
    }

    return true;
}

bool FixtureFile::Save(const std::string& path,
    const FixtureDefinition& def,
    std::string& error)
{
    std::ofstream file(path);
    if (!file.is_open())
    {
        error = "Could not write file: " + path;
        return false;
    }

    const std::string baseDir = GetDirectory(path);

    // Store paths relative to the fixture file location
    const std::string relA = MakeRelative(def.modelAPath, baseDir);
    const std::string relB = MakeRelative(def.modelBPath, baseDir);

    file << "[fixture]\n";
    file << "modelA = " << relA << "\n";
    file << "modelB = " << relB << "\n";

    // Write each injection point as its own numbered section
    for (int i = 0; i < (int)def.injectionPoints.size(); ++i)
    {
        const InjectionPoint& ip = def.injectionPoints[i];

        file << "\n[injection_point." << i << "]\n";
        file << "label = " << ip.label << "\n";
        file << "type  = " << (ip.type == InjectionType::Axial ? "axial" : "radial") << "\n";
        file << "x     = " << ip.x << "\n";
        file << "y     = " << ip.y << "\n";
        file << "z     = " << ip.z << "\n";
    }

    // ---- Defaults sections -------------------------------------------------
    // Only emit sections that have at least one field set, and within each
    // section only emit the set fields. This keeps round-tripping clean: a
    // file loaded with no defaults gets saved with no defaults, and a file
    // that overrode (say) only sprue diameter doesn't sprout phantom
    // overrides for every other sprue field on save.
    auto writeKey = [&](const char* key, const std::optional<std::string>& v)
        {
            if (v) file << key << " = " << *v << "\n";
        };
    auto writeKeyF = [&](const char* key, const std::optional<float>& v)
        {
            if (v) file << key << " = " << *v << "\n";
        };

    {
        const VentDefaults& d = def.ventDefaults;
        if (d.type || d.length || d.width || d.overrunStart || d.overrunEnd)
        {
            file << "\n[vent_defaults]\n";
            writeKey("type", d.type);
            writeKeyF("length", d.length);
            writeKeyF("width", d.width);
            writeKeyF("overrun_start", d.overrunStart);
            writeKeyF("overrun_end", d.overrunEnd);
        }
    }
    {
        const SprueDefaults& d = def.sprueDefaults;
        if (d.type || d.diameter || d.draftAngle || d.coldSlugLength || d.length)
        {
            file << "\n[sprue_defaults]\n";
            writeKey("type", d.type);
            writeKeyF("diameter", d.diameter);
            writeKeyF("draft_angle", d.draftAngle);
            writeKeyF("cold_slug_length", d.coldSlugLength);
            writeKeyF("length", d.length);
        }
    }
    {
        const RunnerDefaults& d = def.runnerDefaults;
        if (d.type || d.diameter || d.coldSlugLength)
        {
            file << "\n[runner_defaults]\n";
            writeKey("type", d.type);
            writeKeyF("diameter", d.diameter);
            writeKeyF("cold_slug_length", d.coldSlugLength);
        }
    }
    {
        const GateDefaults& d = def.gateDefaults;
        if (d.type || d.diameter || d.draftAngle)
        {
            file << "\n[gate_defaults]\n";
            writeKey("type", d.type);
            writeKeyF("diameter", d.diameter);
            writeKeyF("draft_angle", d.draftAngle);
        }
    }
    {
        const SubRunnerDefaults& d = def.subRunnerDefaults;
        if (d.type || d.diameter)
        {
            file << "\n[sub_runner_defaults]\n";
            writeKey("type", d.type);
            writeKeyF("diameter", d.diameter);
        }
    }
    {
        const EjectorDefaults& d = def.ejectorDefaults;
        if (d.type || d.diameter || d.length)
        {
            file << "\n[ejector_defaults]\n";
            writeKey("type", d.type);
            writeKeyF("diameter", d.diameter);
            writeKeyF("length", d.length);
        }
    }

    // ---- Per-half transform sections ---------------------------------------
    // Skip emission when a half is at identity — keeps hand-written fixture
    // files free of redundant zero-valued sections, and means a fixture
    // round-tripped through Load → Save without going through the editor
    // doesn't sprout new sections it didn't have on disk before.
    //
    // Precision: 9 digits is enough to round-trip an mm-scale double through
    // text without losing meaningful resolution, while staying readable. The
    // ostream's default of 6 would truncate sub-micron alignment work the
    // user did via Align Face.
    auto writeTransform = [&](const char* sectionName, const HalfTransform& t)
        {
            if (t.IsIdentity()) return;

            const std::streamsize savedPrec = file.precision();
            file.precision(9);

            file << "\n[" << sectionName << "]\n";
            file << "position_x = " << t.posX << "\n";
            file << "position_y = " << t.posY << "\n";
            file << "position_z = " << t.posZ << "\n";
            file << "rotation_x = " << t.rotX << "\n";
            file << "rotation_y = " << t.rotY << "\n";
            file << "rotation_z = " << t.rotZ << "\n";
            file << "scale      = " << t.scale << "\n";

            file.precision(savedPrec);
        };
    writeTransform("half_a_transform", def.halfATransform);
    writeTransform("half_b_transform", def.halfBTransform);

    return true;
}