#define _CRT_SECURE_NO_WARNINGS
#include "AppConfig.h"
#include <fstream>
#include <filesystem>
#include <wx/stdpaths.h>
#include <wx/string.h>

namespace fs = std::filesystem;

std::string AppConfig::GetConfigPath()
{
    return (fs::path(wxStandardPaths::Get()
        .GetExecutablePath().ToStdString())
        .parent_path() / "mould3r.cfg").string();
}

std::string AppConfig::LoadLastFixture()
{
    std::ifstream file(GetConfigPath());
    if (!file.is_open()) return "";

    std::string line;
    while (std::getline(file, line))
    {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);

        if (key == "lastFixture")
            return value;
    }

    return "";
}

void AppConfig::SaveLastFixture(const std::string& path)
{
    std::ofstream file(GetConfigPath());
    if (!file.is_open()) return;

    file << "lastFixture=" << path << "\n";
}