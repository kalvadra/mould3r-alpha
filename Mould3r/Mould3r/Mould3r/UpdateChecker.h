// UpdateChecker.h
#pragma once
#include <wx/wx.h>
#include <functional>

// ---------------------------------------------------------------------------
// UpdateChecker — Tier 1 update check (notify-only).
//
// Fetches the JSON manifest from Mould3r::Version::UpdateManifestUrl,
// compares its "version" field against the running build, and reports one
// of three outcomes through a callback. It never downloads or runs
// anything; acting on an available update means opening the download page
// in the user's browser (the dialog's job, not this class's).
//
// Threading: wxWebRequest delivers its state events on the main thread, so
// the callback also fires on the main thread — callers can touch UI in it
// directly. One check per UpdateChecker instance; construct a fresh one
// per check rather than reusing.
//
// Timeouts: wxWebRequest (wx 3.2) exposes no timeout setting, so a wxTimer
// watchdog cancels the request after kTimeoutMs. The cancellation surfaces
// as a normal Error outcome ("timed out").
//
// Failure philosophy (this is the MANUAL check — Help menu / About):
// errors are reported honestly, because the user asked and deserves an
// answer. U4's silent startup check will wrap this class and simply drop
// Error outcomes on the floor; the distinction lives in the caller, not
// here.
// ---------------------------------------------------------------------------
class UpdateChecker : public wxEvtHandler
{
public:
    enum class Outcome
    {
        UpToDate,           // manifest reachable, running version >= published
        UpdateAvailable,    // manifest reachable, newer version published
        Error               // network / HTTP / parse failure — see message
    };

    struct Result
    {
        Outcome  outcome = Outcome::Error;
        wxString latestVersion;   // manifest "version" (valid unless Error)
        wxString downloadUrl;     // manifest "url" — the installer itself
        wxString notesUrl;        // manifest "notesUrl" — changelog page
        wxString message;         // human-readable detail (errors mostly)
    };

    using Callback = std::function<void(const Result&)>;

    UpdateChecker() = default;
    ~UpdateChecker() override;

    // Begins the async fetch. The callback fires exactly once, on the main
    // thread, unless Cancel() or destruction happens first. Returns false
    // (and does not retain the callback) if the web request couldn't even
    // be started — wxWebRequest backend missing from the wx build.
    bool Start(Callback onDone);

    // Abandons an in-flight check; the callback will not fire. Safe to
    // call when no check is running.
    void Cancel();

    // ---- Version comparison (public for reuse and for testability) -------
    // Parses "major.minor.patch" into the same packed form as
    // Mould3r::Version::Packed (1.2.3 -> 10203). A missing patch or minor
    // is treated as 0 ("0.6" == "0.6.0"). Returns -1 if the string doesn't
    // begin with a number — the caller must treat that manifest as invalid
    // rather than "newer" or "older".
    static int ParseVersionPacked(const wxString& version);

    // True if `remote` is strictly newer than the running build. A remote
    // version equal to or older than ours is "up to date" — never an offer
    // to downgrade.
    static bool IsNewerThanCurrent(const wxString& remote);

private:
    void OnWebRequestState(class wxWebRequestEvent& evt);
    void OnTimeout(wxTimerEvent&);
    void Finish(const Result& r);   // fires the callback exactly once

    Callback m_callback;
    wxTimer  m_watchdog{ this };
    bool     m_finished = false;

    // The in-flight request is held so Cancel()/destructor can abort it.
    // Stored as a pointer-to-impl to keep <wx/webrequest.h> (and its
    // wxUSE_WEBREQUEST gymnastics) out of this header.
    struct RequestHolder;
    RequestHolder* m_request = nullptr;

    static constexpr int kTimeoutMs = 10000;
};

// ---------------------------------------------------------------------------
// UpdateStartupPolicy — the AppConfig-backed rules for the AUTOMATIC startup
// check (U4). All updater state lives behind these six functions so the
// config keys appear in exactly one .cpp.
//
// Semantics:
//   * The auto check runs at most once per 24h, timed from the last ATTEMPT
//     (not the last success) — an offline user probes once per day instead
//     of on every launch.
//   * "Skip this version" stores the exact version string and suppresses
//     the banner for that version only; any later release re-notifies.
//   * None of this applies to the MANUAL check (Help menu / About), which
//     always runs, ignores the skip list, and never records an attempt.
//
// Keys (flat key=value in mould3r.cfg, camelCase like meshImportQuality):
//   updateAutoCheck    1/0, default 1
//   updateLastCheck    unix seconds of the last auto-check attempt
//   updateSkipVersion  exact version string the user chose to skip
// ---------------------------------------------------------------------------
namespace UpdateStartupPolicy
{
    bool AutoCheckEnabled();
    void SetAutoCheckEnabled(bool enabled);

    // Enabled AND >= 24h since the last recorded attempt. A last-attempt
    // timestamp in the future (clock rolled back) counts as due rather
    // than silencing the check until the clock catches up.
    bool DueForAutoCheck();
    void RecordCheckAttempt();

    bool IsVersionSkipped(const wxString& version);
    void SkipVersion(const wxString& version);
}
