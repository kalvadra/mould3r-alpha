#include "Mould3r.h"
#include "MainFrame.h"
#include "AppConfig.h"
#include "FixtureFile.h"
#include "MeshWorker.h"   // --mesh-worker: Mould3r relaunched as its own 3D-meshing worker
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
    // Worker mode first, before anything touches config or opens a window:
    // this process is a meshing worker the app launched (see MeshWorker.h).
    if (argc >= 4 && argv[1] == kMeshWorkerSwitch)
    {
        m_meshWorker = true;
        m_meshWorkerExitCode = RunMeshWorker(std::filesystem::path(argv[2].ToStdWstring()),
                                             std::filesystem::path(argv[3].ToStdWstring()));
        return true;   // OnRun hands the exit code back without an event loop
    }

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

int MyApp::OnRun()
{
    if (m_meshWorker)
        return m_meshWorkerExitCode;
    return wxApp::OnRun();
}
