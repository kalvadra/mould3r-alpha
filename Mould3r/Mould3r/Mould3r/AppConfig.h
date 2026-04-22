#pragma once
#include <string>

class AppConfig
{
public:
    static std::string GetConfigPath();

    static std::string LoadLastFixture();
    static void        SaveLastFixture(const std::string& path);

    // Generic key/value access — the config file is a simple `key=value`
    // flat store. Save is read-modify-write so keys you didn't touch are
    // preserved. Unknown keys return the provided default.
    static std::string LoadString(const std::string& key, const std::string& fallback = "");
    static void        SaveString(const std::string& key, const std::string& value);
    static int         LoadInt(const std::string& key, int fallback = 0);
    static void        SaveInt(const std::string& key, int value);
};