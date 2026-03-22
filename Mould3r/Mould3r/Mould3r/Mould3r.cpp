#include "Mould3r.h"
#include "MainFrame.h"
#include "StartupDialog.h"
#include "AppConfig.h"
#include "wx/wx.h"

wxIMPLEMENT_APP(MyApp);

bool MyApp::OnInit()
{
    const std::string lastFixture = AppConfig::LoadLastFixture();

    FixtureDefinition fixture;
    std::string error;

    // If a valid default exists, skip the dialog
    if (!lastFixture.empty() && FixtureFile::Load(lastFixture, fixture, error))
    {
        MainFrame* frame = new MainFrame(fixture);
        frame->Show(true);
        return true;
    }

    // Otherwise show the selection dialog
    StartupDialog startup(nullptr);
    startup.PreSelectFixture(lastFixture);  // pre-select even if invalid, for convenience

    if (startup.ShowModal() != wxID_OK)
        return false;

    fixture = startup.GetFixture();
    AppConfig::SaveLastFixture(fixture.fixturePath);

    MainFrame* frame = new MainFrame(fixture);
    frame->Show(true);
    return true;
}
