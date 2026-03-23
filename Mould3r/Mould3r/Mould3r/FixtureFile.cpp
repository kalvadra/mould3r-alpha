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
    // Sections: [fixture], [injection_point.N]
    // An injection_point block is committed to the list when the next section
    // header (or EOF) is encountered.
    enum class Section { None, Fixture, InjectionPoint };
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

    return true;
}