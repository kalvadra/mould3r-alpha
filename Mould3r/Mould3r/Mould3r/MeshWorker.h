#pragma once
// ===========================================================================
// MeshWorker — the `--mesh-worker` mode of Mould3r.exe.
//
// To mesh, the app relaunches ITSELF:  Mould3r.exe --mesh-worker <in> <out>
// (see TetMeshRunner). Same executable, same signature and hash, so anything
// allowed to run Mould3r is allowed to run the worker — no second program for
// antivirus or application-control policies to flag. MyApp::OnInit routes
// that command line here before any window is created.
//
// The worker reads the job file, loads m3tetmesh.dll (fTetWild) from the
// executable's folder, meshes, writes the result file and exits. A crash or a
// hang inside fTetWild only ever costs this process: the app sees no result
// file (or kills the worker on Cancel) and reports it.
//
// Windows: crash dialogs (Windows Error Reporting, CRT abort/assert boxes)
// are suppressed for this process, so a failed mesh never pops a system
// "Mould3r has stopped working" window at the user.
// ===========================================================================

#include <filesystem>

// The command-line switch that selects worker mode.
inline constexpr const char* kMeshWorkerSwitch = "--mesh-worker";

// Returns the process exit code (TetMesh::Status of the run, or 11 if the job
// file couldn't be read / the result couldn't be written).
int RunMeshWorker(const std::filesystem::path& inPath, const std::filesystem::path& outPath);
