#pragma once
#include <string>

class AppConfig
{
public:
    static std::string GetConfigPath();

    static std::string LoadLastFixture();
    static void        SaveLastFixture(const std::string& path);
};