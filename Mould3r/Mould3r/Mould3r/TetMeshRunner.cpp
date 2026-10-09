// TetMeshRunner.cpp — see TetMeshRunner.h.
#include "TetMeshRunner.h"   // wx first

#include <wx/process.h>
#include <wx/progdlg.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>  // windows.h with wx's macro guards (job object)
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <system_error>

#include "MeshWorker.h"      // kMeshWorkerSwitch

namespace fs = std::filesystem;

namespace
{
    // The worker process; OnTerminate is delivered through the event loop.
    class WorkerProcess : public wxProcess
    {
    public:
        WorkerProcess() : wxProcess(wxPROCESS_DEFAULT) {}
        void OnTerminate(int /*pid*/, int status) override
        {
            m_exitCode = status;
            m_done = true;
        }
        bool Done() const { return m_done; }
        int  ExitCode() const { return m_exitCode; }

    private:
        bool m_done = false;
        int  m_exitCode = 0;
    };

#ifdef __WXMSW__
    // One job object for the app's lifetime: every worker is assigned to it,
    // and KILL_ON_JOB_CLOSE ends them all when Mould3r exits (even by crash,
    // since the OS closes the handle).
    HANDLE WorkerJob()
    {
        static HANDLE job = []
        {
            HANDLE j = ::CreateJobObjectW(nullptr, nullptr);
            if (j)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
                li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                ::SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof(li));
            }
            return j;
        }();
        return job;
    }

    void AttachToWorkerJob(long pid)
    {
        HANDLE job = WorkerJob();
        if (!job || pid <= 0) return;
        HANDLE hp = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, (DWORD)pid);
        if (hp)
        {
            ::AssignProcessToJobObject(job, hp);
            ::CloseHandle(hp);
        }
    }
#endif

    // Common Windows exit codes of a crashed process, for the message.
    wxString DescribeExitCode(int code)
    {
        const unsigned u = (unsigned)code;
        switch (u)
        {
        case 0xC0000005u: return "access violation";
        case 0xC00000FDu: return "stack overflow";
        case 0xC0000409u: return "fast-fail / abort";
        case 0xC0000017u: return "out of memory";
        case 0xE06D7363u: return "unhandled C++ exception";
        default: break;
        }
        if (u >= 0xC0000000u) return "unhandled exception";
        return "exited without writing a result";
    }

    fs::path JobDirectory()
    {
        // Per-user local data ("%LOCALAPPDATA%\Mould3r" on Windows).
        fs::path dir = fs::path(wxStandardPaths::Get().GetUserLocalDataDir().ToStdWstring()) / L"mesh";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }
} // namespace

TetMeshOutcome RunTetMeshJobModal(wxWindow* parent, const TetMesh::Surface& surface,
                                  const TetMesh::Params& params, const wxString& title)
{
    TetMeshOutcome out;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    // ---- Job files ----------------------------------------------------------
    static std::atomic<unsigned> counter{ 0 };
    const fs::path dir = JobDirectory();
    const std::wstring stem = wxString::Format("job-%lu-%u", wxGetProcessId(), ++counter).ToStdWstring();
    const fs::path inPath = dir / (stem + L".in");
    const fs::path outPath = dir / (stem + L".out");
    std::error_code ec;
    fs::remove(outPath, ec);

    std::string err;
    if (!TetMesh::WriteJobInput(inPath, surface, params, err))
    {
        out.kind = TetMeshOutcome::Kind::LaunchFailed;
        out.detail = wxString::FromUTF8(err.c_str()) + "\n\n" + wxString(inPath.wstring());
        return out;
    }

    // ---- Launch this executable as the worker -------------------------------
    const wxString exe = wxStandardPaths::Get().GetExecutablePath();
    const wxString sw(kMeshWorkerSwitch), in(inPath.wstring()), outp(outPath.wstring());
    const wchar_t* argv[] = { exe.wc_str(), sw.wc_str(), in.wc_str(), outp.wc_str(), nullptr };

    auto* proc = new WorkerProcess();
    const long pid = wxExecute(argv, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE, proc);
    if (pid <= 0)
    {
        delete proc;   // never started, so wx holds no reference to it
        fs::remove(inPath, ec);
        out.kind = TetMeshOutcome::Kind::LaunchFailed;
        out.detail = "Couldn't start the meshing worker (" + exe + ").";
        return out;
    }
#ifdef __WXMSW__
    AttachToWorkerJob(pid);
#endif

    // ---- Wait, with Cancel ----------------------------------------------------
    bool cancelled = false;
    {
        wxProgressDialog prog(title, "Building the tetrahedral mesh...", 100, parent,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
        while (!proc->Done())
        {
            if (!cancelled && !prog.Pulse())
            {
                cancelled = true;
                wxProcess::Kill(pid, wxSIGKILL);
                prog.Pulse("Stopping...");
            }
            // Deliver the worker's termination (and keep the dialog painted).
            if (wxTheApp) wxTheApp->Yield(true);
            wxMilliSleep(50);
        }
    }
    out.exitCode = proc->ExitCode();
    delete proc;   // wx is done with it once OnTerminate has run
    out.wallSeconds = elapsed();

    // ---- Interpret ---------------------------------------------------------------
    if (cancelled)
    {
        out.kind = TetMeshOutcome::Kind::Cancelled;
        fs::remove(inPath, ec);
        fs::remove(outPath, ec);
        fs::path part = outPath;
        part += ".part";
        fs::remove(part, ec);
        return out;
    }

    if (!fs::exists(outPath, ec))
    {
        // No result: the worker died. Keep the input so the run can be
        // reproduced with "Mould3r.exe --mesh-worker <in> <out>".
        out.kind = TetMeshOutcome::Kind::Crashed;
        out.detail = wxString::Format("The meshing worker stopped unexpectedly (%s, exit code 0x%08X).",
                                      DescribeExitCode(out.exitCode), (unsigned)out.exitCode) +
                     "\n\nIts input was kept for diagnosis:\n" + wxString(inPath.wstring());
        return out;
    }

    if (!TetMesh::ReadJobOutput(outPath, out.result, err))
    {
        out.kind = TetMeshOutcome::Kind::Failed;
        out.detail = wxString::FromUTF8(err.c_str());
    }
    else if (out.result.status == TetMesh::StatusOk)
        out.kind = TetMeshOutcome::Kind::Ok;
    else
    {
        out.kind = TetMeshOutcome::Kind::Failed;
        out.detail = wxString::FromUTF8(out.result.message.c_str());
    }
    fs::remove(inPath, ec);
    fs::remove(outPath, ec);
    return out;
}
