#include "Mould3r.h"
#include "MainFrame.h"
#include "AppConfig.h"
#include "FixtureFile.h"
#include "wx/wx.h"

wxIMPLEMENT_APP(MyApp);

// App startup flow:
//   1. If a valid saved fixture exists on disk, load it and hand it to
//      MainFrame so the app boots directly into a populated scene.
//   2. Otherwise, build MainFrame with an empty FixtureDefinition so the
//      window comes up fully (ribbon + side panel + empty canvas), and
//      *then* — after the frame is visible — prompt the user to pick a
//      fixture. This makes the app feel like the main environment is the
//      home surface rather than a modal-over-nothing on launch.
bool MyApp::OnInit()
{
    const std::string lastFixture = AppConfig::LoadLastFixture();

    FixtureDefinition fixture;  // default-constructed == empty / invalid
    std::string error;

    // Happy path: saved fixture loads cleanly. Hand it to the frame directly.
    const bool haveSavedFixture =
        !lastFixture.empty() && FixtureFile::Load(lastFixture, fixture, error);

    MainFrame* frame = new MainFrame(haveSavedFixture ? fixture : FixtureDefinition{});
    frame->Show(true);

    // If there was nothing to load, ask the user now — with the main frame
    // already up behind the dialog for context. Done via CallAfter so we
    // yield control back to the event loop first and the window has a chance
    // to fully paint before the modal pops.
    if (!haveSavedFixture)
        frame->CallAfter([frame] { frame->PromptForFixtureIfMissing(); });

    return true;
}
