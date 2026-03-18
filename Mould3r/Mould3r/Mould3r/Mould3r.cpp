#include "Mould3r.h"
#include "MainFrame.h"
#include "StartupDialog.h"
#include "wx/wx.h"

bool MyApp::OnInit()
{
    StartupDialog startup(nullptr);
    if (startup.ShowModal() != wxID_OK)
        return false;

    FixtureDefinition fixture = startup.GetFixture();

    MainFrame* frame = new MainFrame(fixture);
    frame->Show(true);
    return true;
}
