#pragma once
#include <string>

struct FixtureDefinition
{
    std::string modelAPath;   // always stored as absolute internally
    std::string modelBPath;
    std::string fixturePath;  // directory anchor for relative path resolution

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