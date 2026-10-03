#pragma once
#include <wx/wx.h>

class MyApp : public wxApp
{
public:
	virtual bool OnInit() override;

	// Worker mode ("Mould3r.exe --mesh-worker <in> <out>", see MeshWorker.h):
	// OnInit runs the job instead of building the UI, and OnRun returns its
	// exit code without entering the event loop.
	virtual int OnRun() override;

private:
	bool m_meshWorker = false;
	int  m_meshWorkerExitCode = 0;
};