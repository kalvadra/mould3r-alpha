#pragma once
// ===========================================================================
// TetMeshRunner — the app side of 3D meshing: runs one mesh job in a worker
// process (Mould3r.exe --mesh-worker, see MeshWorker.h) behind a cancellable
// progress dialog, and turns what happened into a TetMeshOutcome.
//
//  * The worker is this same executable, launched directly (no shell, no
//    console window).
//  * Windows: it's placed in a kill-on-close job object, so it can never
//    outlive Mould3r — closing or crashing the app ends it too.
//  * Cancel terminates the worker immediately (fTetWild has no cancel hook;
//    a separate process makes cancel real).
//  * Job files live in the per-user local app-data folder
//    (%LOCALAPPDATA%\Mould3r\mesh on Windows), never next to the executable.
//    They're removed afterwards; on a crash the input is kept so the run can
//    be reproduced by hand.
// ===========================================================================

#include <wx/wx.h>

#include "TetMeshJob.h"

struct TetMeshOutcome
{
    enum class Kind
    {
        Ok,            // result.mesh holds the tet mesh
        Failed,        // the worker ran but reported an error (result.message)
        Cancelled,     // the user cancelled
        Crashed,       // the worker died without writing a result (exitCode)
        LaunchFailed   // couldn't write the job or start the worker (detail)
    };
    Kind              kind = Kind::LaunchFailed;
    TetMesh::JobResult result;     // from the worker (Ok / Failed)
    wxString          detail;      // human-readable explanation for Failed / Crashed / LaunchFailed
    int               exitCode = 0;
    double            wallSeconds = 0.0;
};

// Mesh `surface` in a worker process, modally, with a pulsing progress
// dialog titled `title` (Cancel stops the worker). Blocks until done.
TetMeshOutcome RunTetMeshJobModal(wxWindow* parent, const TetMesh::Surface& surface,
                                  const TetMesh::Params& params, const wxString& title);
