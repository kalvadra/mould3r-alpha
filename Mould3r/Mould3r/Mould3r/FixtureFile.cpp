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

    std::string line;
    while (std::getline(file, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == '[')
            continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));

        // Resolve relative paths against the fixture file's directory
        if (key == "modelA")
            out.modelAPath = ResolveRelative(value, baseDir);
        else if (key == "modelB")
            out.modelBPath = ResolveRelative(value, baseDir);
    }

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

    return true;
}