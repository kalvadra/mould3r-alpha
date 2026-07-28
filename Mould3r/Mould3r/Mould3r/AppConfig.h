#pragma once
#include <string>

// <windows.h> defines LoadString as a macro expanding to LoadStringW/A,
// which mangles both this class's declaration and its call sites in any
// translation unit where Windows headers were included first (wx pulls
// them in via wrapwin.h). Undefining it HERE — before the class — keeps
// the declaration and every downstream call on the same, real name.
// (Only remaining hazard: including <windows.h> AFTER this header in the
// same .cpp would re-poison later call sites; include order app-headers-
// last avoids that, as every file in this project already does.)
#ifdef LoadString
#undef LoadString
#endif

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