#define _CRT_SECURE_NO_WARNINGS
#include "AppConfig.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <wx/stdpaths.h>
#include <wx/string.h>

namespace fs = std::filesystem;

namespace
{
    // Load the whole config file into a key->value map. Silent on errors
    // (missing file, unreadable entries) — returns an empty map.
    std::map<std::string, std::string> LoadAll()
    {
        std::map<std::string, std::string> kv;
        std::ifstream file(AppConfig::GetConfigPath());
        if (!file.is_open()) return kv;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (line.back() == '\r') line.pop_back();    // CRLF tolerance
            if (line.empty() || line[0] == '#') continue;

            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            kv.emplace(line.substr(0, eq), line.substr(eq + 1));
        }
        return kv;
    }

    void WriteAll(const std::map<std::string, std::string>& kv)
    {
        std::ofstream file(AppConfig::GetConfigPath(), std::ios::trunc);
        if (!file.is_open()) return;
        for (const auto& e : kv)
            file << e.first << '=' << e.second << '\n';
    }
}

std::string AppConfig::GetConfigPath()
{
    return (fs::path(wxStandardPaths::Get()
        .GetExecutablePath().ToStdString())
        .parent_path() / "mould3r.cfg").string();
}

std::string AppConfig::LoadString(const std::string& key, const std::string& fallback)
{
    const auto kv = LoadAll();
    const auto it = kv.find(key);
    return (it == kv.end()) ? fallback : it->second;
}

void AppConfig::SaveString(const std::string& key, const std::string& value)
{
    auto kv = LoadAll();        // preserve other keys
    kv[key] = value;
    WriteAll(kv);
}

int AppConfig::LoadInt(const std::string& key, int fallback)
{
    const std::string s = LoadString(key, "");
    if (s.empty()) return fallback;
    try { return std::stoi(s); }
    catch (...) { return fallback; }
}

void AppConfig::SaveInt(const std::string& key, int value)
{
    SaveString(key, std::to_string(value));
}

// Kept for source compatibility with existing callers.
std::string AppConfig::LoadLastFixture() { return LoadString("lastFixture", ""); }
void AppConfig::SaveLastFixture(const std::string& path) { SaveString("lastFixture", path); }
