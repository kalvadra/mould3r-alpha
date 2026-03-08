#include "Mould3r.h"
#include "MainFrame.h"

bool MyApp::OnInit()
{
	MainFrame* frame = new MainFrame();
	frame->Show(true);
	return true;
}
