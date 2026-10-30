#pragma once

#include <filesystem>
#include <stdexcept>
#include <wx/stdpaths.h>
#include <wx/string.h>

// macOS bundles are immutable: icons live in Resources; editable data lives
// in Application Support. Windows keeps its existing portable layout.
namespace AppPaths
{
    inline std::filesystem::path Resources()
    {
#ifdef __APPLE__
        return std::filesystem::u8path(wxStandardPaths::Get().GetResourcesDir().utf8_string());
#else
        return std::filesystem::u8path(wxStandardPaths::Get().GetExecutablePath().utf8_string()).parent_path();
#endif
    }

    inline std::filesystem::path UserData()
    {
#ifdef __APPLE__
        auto dir = std::filesystem::u8path(wxStandardPaths::Get().GetUserDataDir().utf8_string());
        std::filesystem::create_directories(dir);
        return dir;
#else
        return Resources();
#endif
    }

    inline std::filesystem::path Fixtures() { return UserData() / "fixtures"; }

    inline void PrepareUserData()
    {
#ifdef __APPLE__
        // Copy missing bundled examples once, without overwriting user edits.
        auto from = Resources() / "fixtures";
        auto to = Fixtures();
        std::filesystem::create_directories(to);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(from))
        {
            const auto dest = to / entry.path().lexically_relative(from);
            if (entry.is_directory()) std::filesystem::create_directories(dest);
            else if (entry.is_regular_file())
                std::filesystem::copy_file(entry.path(), dest,
                    std::filesystem::copy_options::skip_existing);
        }
#endif
    }
}
