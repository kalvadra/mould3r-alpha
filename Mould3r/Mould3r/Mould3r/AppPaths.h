#pragma once

// wx headers come first on purpose. wx/defs.h defines
// _CRT_SECURE_NO_DEPRECATE for MSVC, which only takes effect if it is seen
// before the first CRT header. Including <filesystem> ahead of it makes every
// strcpy/wcscat/_wfopen wrapper in wx/wxcrt.h a C4996 error under /sdl.
#include <wx/defs.h>
#include <wx/stdpaths.h>
#include <wx/string.h>

#include <filesystem>
#include <stdexcept>

// macOS bundles are immutable: icons live in Resources; editable data lives
// in Application Support. Windows keeps its existing portable layout.
namespace AppPaths
{
    // wxString -> filesystem path without a lossy narrow-string round trip.
    // Windows paths are UTF-16 natively; macOS paths are UTF-8 natively.
    // (std::filesystem::u8path is deprecated in C++20 and is a C4996 error
    // under the project's /sdl setting, so it isn't used here.)
    inline std::filesystem::path ToPath(const wxString& s)
    {
#ifdef _WIN32
        return std::filesystem::path(s.ToStdWstring());
#else
        return std::filesystem::path(s.utf8_string());
#endif
    }

    // filesystem path -> wxString, the inverse of ToPath. Use this rather than
    // wxString::FromUTF8(p.string()): on Windows p.string() is in the ANSI
    // code page, not UTF-8, so non-ASCII folders (e.g. a per-user install
    // under C:\Users\José\...) would come back corrupted.
    inline wxString ToWx(const std::filesystem::path& p)
    {
#ifdef _WIN32
        return wxString(p.wstring());
#else
        return wxString::FromUTF8(p.string());
#endif
    }

    inline std::filesystem::path Resources()
    {
#ifdef __APPLE__
        return ToPath(wxStandardPaths::Get().GetResourcesDir());
#else
        return ToPath(wxStandardPaths::Get().GetExecutablePath()).parent_path();
#endif
    }

    inline std::filesystem::path UserData()
    {
#ifdef __APPLE__
        auto dir = ToPath(wxStandardPaths::Get().GetUserDataDir());
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
