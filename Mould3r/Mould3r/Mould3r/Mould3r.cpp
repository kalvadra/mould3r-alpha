#include "Mould3r.h"
#include "MainFrame.h"
#include "StartupDialog.h"
#include "wx/wx.h"

bool MyApp::OnInit()
{
	StartupDialog startup(nullptr);
	if (startup.ShowModal() != wxID_OK)
		return false;   // user hit Cancel — exit cleanly

	MainFrame* frame = new MainFrame(startup.GetConfig());
	frame->Show(true);
	return true;
}
