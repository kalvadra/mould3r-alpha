// UpdateChecker.cpp
#include "UpdateChecker.h"

#include <wx/webrequest.h>
#include <nlohmann/json.hpp>

#include "Version.h"
#include "AppConfig.h"

#include <ctime>
#include <cstdlib>

// NOTE: AppConfig.h itself undefines the <windows.h> LoadString macro
// before declaring the class — see the comment there. That header must
// stay included AFTER the wx/Windows includes above for the guard to
// cover this file's call sites, which it is.

// ---------------------------------------------------------------------------
// RequestHolder — keeps the wxWebRequest handle out of the header.
// wxWebRequest is a ref-counted value type; holding it keeps the transfer
// alive, and Cancel() on it aborts the underlying WinHTTP operation.
// ---------------------------------------------------------------------------
struct UpdateChecker::RequestHolder
{
    wxWebRequest request;
};

UpdateChecker::~UpdateChecker()
{
    Cancel();
    delete m_request;
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------
bool UpdateChecker::Start(Callback onDone)
{
#if wxUSE_WEBREQUEST
    // Session is process-global. GetDefault() lazily creates it with the
    // platform's default backend (WinHTTP on Windows) on first call and
    // reports failure through IsOpened() — so that one check covers "no
    // backend in this build" without naming any backend explicitly.
    wxWebSession& session = wxWebSession::GetDefault();
    if (!session.IsOpened())
        return false;

    wxWebRequest req = session.CreateRequest(
        this, wxString::FromUTF8(Mould3r::Version::UpdateManifestUrl));
    if (!req.IsOk())
        return false;

    // Belt-and-braces against stale CDN/proxy caches: ask for a fresh copy.
    // (Server-side Cache-Control on the manifest is the primary control.)
    req.SetHeader("Cache-Control", "no-cache");
    req.SetHeader("Accept", "application/json");

    m_callback = std::move(onDone);
    m_finished = false;

    delete m_request;
    m_request = new RequestHolder{ req };

    Bind(wxEVT_WEBREQUEST_STATE, &UpdateChecker::OnWebRequestState, this);
    Bind(wxEVT_TIMER, &UpdateChecker::OnTimeout, this);

    m_watchdog.StartOnce(kTimeoutMs);
    req.Start();
    return true;
#else
    (void)onDone;
    return false;
#endif
}

void UpdateChecker::Cancel()
{
    m_watchdog.Stop();
    m_finished = true;          // suppress any late events' callback
    m_callback = nullptr;
#if wxUSE_WEBREQUEST
    if (m_request && m_request->request.IsOk())
        m_request->request.Cancel();
#endif
}

// ---------------------------------------------------------------------------
// Finish — single exit point; guarantees the callback fires at most once
// even if a state event and the watchdog race.
// ---------------------------------------------------------------------------
void UpdateChecker::Finish(const Result& r)
{
    if (m_finished)
        return;
    m_finished = true;
    m_watchdog.Stop();

    if (m_callback)
    {
        // Move out first so the callback can't be re-entered. NOTE: the
        // callback must NOT destroy this UpdateChecker synchronously —
        // Finish() is reached from OnWebRequestState / OnTimeout, which
        // still have `this` on the stack after cb() returns. Owners that
        // want to dispose of the checker from the callback must defer
        // (wxWindow::CallAfter) — MainFrame's startup check does exactly
        // that. The dialog is safe by construction: it keeps the checker
        // alive until the dialog itself is destroyed.
        Callback cb = std::move(m_callback);
        m_callback = nullptr;
        cb(r);
    }
}

void UpdateChecker::OnTimeout(wxTimerEvent&)
{
#if wxUSE_WEBREQUEST
    if (m_request && m_request->request.IsOk())
        m_request->request.Cancel();   // surfaces as State_Cancelled below
#endif
    Result r;
    r.outcome = Outcome::Error;
    r.message = "The update server did not respond within 10 seconds.";
    Finish(r);
}

// ---------------------------------------------------------------------------
// OnWebRequestState — the whole response pipeline lives here: transport
// outcome -> HTTP status -> JSON parse -> version compare.
// ---------------------------------------------------------------------------
void UpdateChecker::OnWebRequestState(wxWebRequestEvent& evt)
{
#if wxUSE_WEBREQUEST
    switch (evt.GetState())
    {
    case wxWebRequest::State_Completed:
        break;                          // fall through to processing below

    case wxWebRequest::State_Failed:
    {
        Result r;
        r.outcome = Outcome::Error;
        r.message = "Could not reach the update server.\n"
            "Check your internet connection and try again.";
        Finish(r);
        return;
    }

    case wxWebRequest::State_Cancelled:
        // Either our watchdog (which already Finish()ed with a timeout
        // message) or an explicit Cancel() (callback suppressed). Nothing
        // further to report either way.
        return;

    default:
        return;                         // Active / Idle — not terminal
    }

    // ---- HTTP status --------------------------------------------------------
    const int status = evt.GetResponse().GetStatus();
    if (status != 200)
    {
        Result r;
        r.outcome = Outcome::Error;
        r.message = wxString::Format(
            "The update server returned an unexpected response (HTTP %d).",
            status);
        Finish(r);
        return;
    }

    // ---- Parse --------------------------------------------------------------
    // Defensive throughout: a truncated body, an HTML error page, or a
    // schema surprise must land in Error, never in a crash or a bogus
    // "update available".
    Result r;
    try
    {
        const std::string body = evt.GetResponse().AsString().utf8_string();
        const nlohmann::json manifest = nlohmann::json::parse(body);

        const std::string version = manifest.value("version", "");
        if (version.empty() || ParseVersionPacked(version) < 0)
        {
            r.outcome = Outcome::Error;
            r.message = "The update information was malformed.";
            Finish(r);
            return;
        }

        r.latestVersion = wxString::FromUTF8(version);
        r.downloadUrl = wxString::FromUTF8(manifest.value("url", ""));
        r.notesUrl = wxString::FromUTF8(manifest.value("notesUrl", ""));

        r.outcome = IsNewerThanCurrent(r.latestVersion)
            ? Outcome::UpdateAvailable
            : Outcome::UpToDate;
    }
    catch (const nlohmann::json::exception&)
    {
        r = Result{};
        r.outcome = Outcome::Error;
        r.message = "The update information could not be read.";
    }
    Finish(r);
#endif
}

// ---------------------------------------------------------------------------
// Version comparison
// ---------------------------------------------------------------------------
int UpdateChecker::ParseVersionPacked(const wxString& version)
{
    // Accept "1", "1.2", "1.2.3"; anything not starting with a digit is
    // invalid. Trailing junk after the third component is ignored, which
    // tolerates a future "0.6.0-beta1" without misreading its numbers.
    int major = -1, minor = 0, patch = 0;
    if (wxSscanf(version, "%d.%d.%d", &major, &minor, &patch) < 1)
        return -1;
    if (major < 0 || minor < 0 || patch < 0)
        return -1;

    // Same packing (and the same 0-99 per-component assumption) as
    // Mould3r::Version::Packed — the two must stay comparable.
    return major * 10000 + minor * 100 + patch;
}

bool UpdateChecker::IsNewerThanCurrent(const wxString& remote)
{
    const int remotePacked = ParseVersionPacked(remote);
    if (remotePacked < 0)
        return false;                   // invalid never counts as newer
    return remotePacked > Mould3r::Version::Packed;
}

// ===========================================================================
// UpdateStartupPolicy
// ===========================================================================
namespace UpdateStartupPolicy
{
    namespace
    {
        constexpr const char* kKeyAutoCheck = "updateAutoCheck";
        constexpr const char* kKeyLastCheck = "updateLastCheck";
        constexpr const char* kKeySkipVersion = "updateSkipVersion";

        constexpr long long kThrottleSeconds = 24LL * 60 * 60;
    }

    bool AutoCheckEnabled()
    {
        return AppConfig::LoadInt(kKeyAutoCheck, 1) != 0;
    }

    void SetAutoCheckEnabled(bool enabled)
    {
        AppConfig::SaveInt(kKeyAutoCheck, enabled ? 1 : 0);
    }

    bool DueForAutoCheck()
    {
        if (!AutoCheckEnabled())
            return false;

        // Stored as a decimal string, not through LoadInt: unix seconds
        // overflow int in 2038, and this code has no reason to inherit
        // that deadline.
        const std::string s = AppConfig::LoadString(kKeyLastCheck, "0");
        const long long last = std::strtoll(s.c_str(), nullptr, 10);
        const long long now = static_cast<long long>(std::time(nullptr));

        if (last > now)
            return true;    // clock rolled back — treat as due, and the
        // attempt will re-write a sane timestamp
        return (now - last) >= kThrottleSeconds;
    }

    void RecordCheckAttempt()
    {
        const long long now = static_cast<long long>(std::time(nullptr));
        AppConfig::SaveString(kKeyLastCheck, std::to_string(now));
    }

    bool IsVersionSkipped(const wxString& version)
    {
        const std::string skipped = AppConfig::LoadString(kKeySkipVersion, "");
        return !skipped.empty() && version.utf8_string() == skipped;
    }

    void SkipVersion(const wxString& version)
    {
        AppConfig::SaveString(kKeySkipVersion, version.utf8_string());
    }
}
