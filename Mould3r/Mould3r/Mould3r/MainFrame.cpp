#include <wx/filedlg.h>
#include <wx/bmpbndl.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/dnd.h>
#include <wx/graphics.h>   // wxGraphicsContext — rounded-rect paint for icon tool buttons
#include <wx/dcbuffer.h>   // wxAutoBufferedPaintDC — flicker-free repaint on hover/toggle
#include <wx/simplebook.h> // wxSimplebook — the Prepare/Preview perspective pager
#include <wx/spinctrl.h>   // wxSpinCtrlDouble — fine-tune fields in the insert editor
#include <wx/statline.h>   // wxStaticLine — section separators in the insert editor
#include <memory>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>   // windows.h with the wx-safe macro guards —
// SetWindowRgn for the CanvasToast pill shape
#endif

#include "MainFrame.h"
#include "GLCanvas.h"
#include "PreviewPanel.h"    // embedded post-cut mould preview perspective
#include "FixtureEditor.h"
#include "CreateFixtureDialog.h"
#include "ProceduralFixtureDialog.h"   // re-open dims/clearances for Edit Fixture
#include "RotateDialog.h"
#include "TranslateDialog.h"
#include "ScaleDialog.h"
#include "PrecisionPlaceDialog.h"
#include "GridSettingsDialog.h"
#include "AboutDialog.h"       // Help -> About Mould3r
#include "UpdateCheckDialog.h" // Help -> Check for Updates...
#include "UpdateChecker.h"     // background startup check + startup policy (U4)
#include "Version.h"           // Mould3r::Version — website fallback for the
                               // update banner's View action
#include "AppConfig.h"
#include "MeshImportSettings.h"
#include "RoundedButton.h"     // rounded button for sidebar / toolbar action buttons
#include "SplitButton.h"       // split action+dropdown button for Export mode
#include "PerspectiveButton.h" // flat tab-style perspective switch
#include "VentEditToolbar.h"   // Part 5: floating complex-vent-path toolbar
#include "SprueEditToolbar.h"  // Edit Sprue floating toolbar
#include "WindowEffects.h"     // DWM corner rounding for the main frame
#include "style.h"

// ---------------------------------------------------------------------------
// Ribbon colour aliases (shorthand into the Style namespace)
// ---------------------------------------------------------------------------
static const wxColour& kRibbonBg = Style::AppBg;
static const wxColour& kBtnHover = Style::BtnHover;
static const wxColour& kTextDefault = Style::TextPrimary;
static const wxColour& kTextActive = Style::TextActive;

// ---------------------------------------------------------------------------
// SVG asset paths (relative to the executable directory)
// ---------------------------------------------------------------------------
static const wxString kAppIconSvg = "res/logos/logo-icon-nobackground.svg";
static const wxString kRibbonLogoSvg = "";

// ---------------------------------------------------------------------------
// Chevron SVG icons (relative to the executable directory)
// ---------------------------------------------------------------------------
static const wxString kChevronDownSvg = "res/icons/chevron-down.svg";
static const wxString kChevronRightSvg = "res/icons/chevron-right.svg";

// ---------------------------------------------------------------------------
// Settings-panel layout constants — keep dimension fields and type dropdowns
// aligned in a consistent right-hand control column.
// ---------------------------------------------------------------------------
static const int kFieldWidth = 90;     // text-entry width (px)
static const int kUnitWidth = 28;     // fixed unit-label column (px)
static const int kFieldGap = 4;      // gap between field and unit label
static const int kCtrlColWidth = kFieldWidth + kFieldGap + kUnitWidth;  // total

// ---------------------------------------------------------------------------
// LoadSvgBundle — loads an SVG file and returns a wxBitmapBundle at the
// requested size.  Relative paths are anchored to the executable directory.
// If recolorWhite is true, common fill/stroke colours are replaced with white.
// Returns an invalid bundle if the path is empty or the file can't be read.
// ---------------------------------------------------------------------------
static wxBitmapBundle LoadSvgBundle(const wxString& svgPath,
    const wxSize& size,
    bool recolorWhite = false)
{
    if (svgPath.IsEmpty())
        return wxBitmapBundle();

    wxFileName fn(svgPath);
    if (fn.IsRelative())
    {
        wxFileName exeDir(wxStandardPaths::Get().GetExecutablePath());
        fn.MakeAbsolute(exeDir.GetPath());
    }

    wxFile file(fn.GetFullPath());
    if (!file.IsOpened())
        return wxBitmapBundle();

    wxString svg;
    file.ReadAll(&svg);

    if (recolorWhite)
    {
        svg.Replace("currentColor", "white");
        svg.Replace("\"black\"", "\"white\"");
        svg.Replace("\"#000000\"", "\"white\"");
        svg.Replace("\"#000\"", "\"white\"");
    }

    const wxScopedCharBuffer utf8 = svg.utf8_str();
    return wxBitmapBundle::FromSVG(utf8.data(), size);
}

// ---------------------------------------------------------------------------
// CanvasToast — a small self-painted hint pill pinned to the bottom-centre of
// the 3D viewport.
//
// Why: a few modes are signalled only by the cursor (PlaceInsert swaps to a
// hand, meaning "click a body to parent to"), which is easy to miss. This says
// it in words, without stealing focus or needing to be dismissed.
//
// Construction rules match VentEditToolbar: a SIBLING of the canvas raised
// above it (a true child of a wxGLCanvas can be overdrawn by SwapBuffers), one
// HWND, no nested native controls, everything painted here. Defined in this
// .cpp rather than its own file pair so the .vcxproj needs no new entries —
// same arrangement as InsertEditDialog further down.
//
// NOT time-limited: it appears when a mode raises it and goes away when that
// mode ends. A guidance hint that fades out mid-task is worse than one that
// stays put, and the mode is the natural lifetime. Swapping in an auto-fade is
// a wxTimer away if that turns out to be the wrong call.
//
// The pill is opaque and does swallow clicks in its own footprint. It sits low
// and is only as wide as its text, so it rarely covers anything you need to
// click; if it ever does, WS_EX_TRANSPARENT on the HWND makes it click-through.
// ---------------------------------------------------------------------------
static constexpr int kToastH = 38;
static constexpr int kToastPadX = 20;

class CanvasToast : public wxPanel
{
public:
    explicit CanvasToast(wxWindow* parent);

    // Set the message, resize to fit it, show and raise. Re-showing the message
    // already on screen is a no-op, so it is safe to call every mode change.
    // The CALLER re-pins us afterwards (GLCanvas::RepositionCanvasToast) — the
    // size just changed and the canvas owns the centring.
    void ShowMessage(const wxString& msg);
    void HideToast();

private:
    void OnPaint(wxPaintEvent&);
    void ApplyPillShape();

    wxString m_msg;
};

CanvasToast::CanvasToast(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(200, kToastH),
        wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // we paint everything ourselves
    SetBackgroundColour(Style::CardBg);
    Bind(wxEVT_PAINT, &CanvasToast::OnPaint, this);
    Hide();   // nothing to say until a mode raises us
}

// Clip the HWND to a pill so the corners are genuinely cut away and the
// viewport shows through, rather than the card colour squaring off outside the
// drawn outline. wxWindow has no SetShape (that is wxNonOwnedWindow, top-level
// only), so this goes native — same technique as VentEditToolbar. SetWindowRgn
// takes ownership of the region on success, so it must not be deleted then.
void CanvasToast::ApplyPillShape()
{
#ifdef __WXMSW__
    HWND hwnd = (HWND)GetHWND();
    if (!hwnd) return;

    const wxSize sz = GetSize();
    if (sz.x <= 0 || sz.y <= 0) return;

    // +1 because CreateRoundRectRgn treats right/bottom as exclusive; without
    // it the region shaves the last row and column and eats the 1px border.
    HRGN rgn = ::CreateRoundRectRgn(0, 0, sz.x + 1, sz.y + 1, sz.y, sz.y);
    if (!rgn) return;

    if (!::SetWindowRgn(hwnd, rgn, TRUE))
        ::DeleteObject(rgn);
#endif
}

void CanvasToast::ShowMessage(const wxString& msg)
{
    if (IsShown() && msg == m_msg) return;
    m_msg = msg;

    // Size to the text. wxClientDC measures with this window's font, which is
    // the same one OnPaint draws with.
    int tw = 0, th = 0;
    {
        wxClientDC dc(this);
        dc.SetFont(GetFont());
        dc.GetTextExtent(m_msg, &tw, &th);
    }
    SetSize(tw + kToastPadX * 2, kToastH);
    ApplyPillShape();   // re-clip: the width just changed

    if (!IsShown()) Show();
    Raise();            // keep above the GL surface
    Refresh(false);
}

void CanvasToast::HideToast()
{
    if (IsShown()) Hide();
}

// ---------------------------------------------------------------------------
// UpdateBanner — the U4 non-modal "an update is available" card, pinned to
// the bottom-right of the viewport.
//
// Same construction rules as CanvasToast / VentEditToolbar above: a SIBLING
// of the canvas raised above it, one HWND, no nested native controls,
// everything painted here, corners cut with the CreateRoundRectRgn/
// SetWindowRgn technique. Defined in this .cpp so the .vcxproj needs no new
// entries.
//
// Three affordances, all self-hit-tested:
//   "View Download Page"  -> browser (via callback), then dismiss
//   "Skip this version"   -> suppress this version permanently (callback)
//   x (top-right)         -> dismiss; the 24h throttle means the reminder
//                            returns on a later launch, not next launch
//
// Deliberately NOT auto-fading and NOT foreground-stealing: it appears once
// per throttle window, sits out of the way, and waits. Startup is the one
// moment the user definitely isn't mid-edit, but they may still be reaching
// for a tool — a banner that vanishes on its own timer punishes reading it
// later.
// ---------------------------------------------------------------------------
static constexpr int kBannerW = 332;
static constexpr int kBannerH = 78;
static constexpr int kBannerPad = 16;
static constexpr double kBannerCorner = 8.0;

class UpdateBanner : public wxPanel
{
public:
    explicit UpdateBanner(wxWindow* parent);

    // Fill in the content and show. targetUrl is resolved by the caller
    // (notes -> installer -> website) so this class knows nothing about
    // manifests.
    void ShowUpdate(const wxString& latestVersion);

    std::function<void()> onView;
    std::function<void()> onSkip;
    std::function<void()> onDismiss;

private:
    void OnPaint(wxPaintEvent&);
    void OnMouse(wxMouseEvent&);
    void ApplyRoundedShape();

    // Hit regions, recomputed each paint (text metrics live there).
    wxRect m_rView, m_rSkip, m_rClose;
    int    m_hover = -1;              // 0 view, 1 skip, 2 close

    wxString m_headline;
};

UpdateBanner::UpdateBanner(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(kBannerW, kBannerH),
        wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(Style::CardBg);
    Bind(wxEVT_PAINT, &UpdateBanner::OnPaint, this);
    Bind(wxEVT_MOTION, &UpdateBanner::OnMouse, this);
    Bind(wxEVT_LEFT_UP, &UpdateBanner::OnMouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &UpdateBanner::OnMouse, this);
    Hide();
}

void UpdateBanner::ApplyRoundedShape()
{
#ifdef __WXMSW__
    HWND hwnd = (HWND)GetHWND();
    if (!hwnd) return;

    const wxSize sz = GetSize();
    if (sz.x <= 0 || sz.y <= 0) return;

    // +1: CreateRoundRectRgn treats right/bottom as exclusive (see
    // CanvasToast::ApplyPillShape). SetWindowRgn owns the region on success.
    const int d = (int)(kBannerCorner * 2);
    HRGN rgn = ::CreateRoundRectRgn(0, 0, sz.x + 1, sz.y + 1, d, d);
    if (!rgn) return;

    if (!::SetWindowRgn(hwnd, rgn, TRUE))
        ::DeleteObject(rgn);
#endif
}

void UpdateBanner::ShowUpdate(const wxString& latestVersion)
{
    m_headline = wxString::Format("Mould3r %s is available", latestVersion);
    ApplyRoundedShape();
    if (!IsShown()) Show();
    Raise();
    Refresh(false);
}

void UpdateBanner::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(Style::CardBg));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    const wxSize sz = GetSize();

    // Card face + hairline border, inset half a pixel so the stroke isn't
    // clipped by the window region.
    gc->SetBrush(wxBrush(Style::CardBg));
    gc->SetPen(wxPen(Style::Divider, 1));
    gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1.0, sz.y - 1.0, kBannerCorner);

    const wxFont headFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI");
    const wxFont actFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI");

    // ---- headline ----------------------------------------------------------
    gc->SetFont(headFont, Style::TextPrimary);
    gc->DrawText(m_headline, kBannerPad, 16.0);

    // ---- close x -----------------------------------------------------------
    m_rClose = wxRect(sz.x - 30, 10, 20, 20);
    gc->SetFont(headFont,
        m_hover == 2 ? Style::TextPrimary : Style::TextMuted);
    {
        // Wide literal: a narrow "\u2715" is encoded through the execution
        // charset and garbles without /utf-8; L"" is charset-independent.
        static const wxString kCloseGlyph = L"\u2715";
        double tw = 0, th = 0, d1, d2;
        gc->GetTextExtent(kCloseGlyph, &tw, &th, &d1, &d2);
        gc->DrawText(kCloseGlyph,
            m_rClose.x + (m_rClose.width - tw) / 2.0,
            m_rClose.y + (m_rClose.height - th) / 2.0);
    }

    // ---- actions -----------------------------------------------------------
    const double actY = sz.y - 30.0;

    gc->SetFont(actFont, m_hover == 0 ? Style::TextPrimary : Style::BtnPlace);
    {
        double tw = 0, th = 0, d1, d2;
        gc->GetTextExtent("View Download Page", &tw, &th, &d1, &d2);
        gc->DrawText("View Download Page", kBannerPad, actY);
        m_rView = wxRect(kBannerPad - 4, (int)actY - 4,
            (int)tw + 8, (int)th + 8);

        gc->SetFont(actFont,
            m_hover == 1 ? Style::TextSubtle : Style::TextMuted);
        double sw = 0, sh = 0;
        gc->GetTextExtent("Skip this version", &sw, &sh, &d1, &d2);
        const double skipX = kBannerPad + tw + 22.0;
        gc->DrawText("Skip this version", skipX, actY);
        m_rSkip = wxRect((int)skipX - 4, (int)actY - 4,
            (int)sw + 8, (int)sh + 8);
    }
}

void UpdateBanner::OnMouse(wxMouseEvent& e)
{
    if (e.Leaving())
    {
        if (m_hover != -1) { m_hover = -1; Refresh(false); }
        return;
    }

    const wxPoint p = e.GetPosition();
    int over = -1;
    if (m_rView.Contains(p))  over = 0;
    else if (m_rSkip.Contains(p))  over = 1;
    else if (m_rClose.Contains(p)) over = 2;

    if (e.Moving() || e.Dragging())
    {
        if (over != m_hover) { m_hover = over; Refresh(false); }
        SetCursor(over >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
        return;
    }

    if (e.LeftUp() && over >= 0)
    {
        // Dismiss-first so a slow callback (browser launch) can't show a
        // half-dead banner; the callbacks are owned by MainFrame and safe
        // to run after Hide().
        Hide();
        if (over == 0 && onView)    onView();
        else if (over == 1 && onSkip)    onSkip();
        else if (over == 2 && onDismiss) onDismiss();
    }
}

void CanvasToast::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(Style::CardBg));
    dc.Clear();

    wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxSize sz = GetClientSize();
    const double radius = (sz.y - 1.0) / 2.0;   // full pill

    gc->SetBrush(wxBrush(Style::CardBg));
    gc->SetPen(wxPen(Style::Divider, 1));
    gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1.0, sz.y - 1.0, radius);

    gc->SetFont(GetFont(), Style::TextPrimary);
    double tw, th;
    gc->GetTextExtent(m_msg, &tw, &th);
    gc->DrawText(m_msg, (sz.x - tw) / 2.0, (sz.y - th) / 2.0);

    delete gc;
}

// ---------------------------------------------------------------------------
// Drag-and-drop target for the 3D viewport.
// Accepts STEP/STL/OBJ files dropped onto the canvas and routes them through
// the same import path as the File -> Import Model menu item, so a dropped
// file gets the same progress dialog, faceting, normal/crease processing and
// non-manifold warnings as a file picked through the dialog.
// Mixed drops (some supported, some not) are partitioned: the user gets a
// single up-front warning listing the unsupported files, then the supported
// ones import sequentially.
// ---------------------------------------------------------------------------
namespace {

    bool IsSupportedModelExt(const wxString& path)
    {
        const wxString ext = wxFileName(path).GetExt().Lower();
        return ext == "step" || ext == "stp" || ext == "stl" || ext == "obj";
    }

    class ModelFileDropTarget : public wxFileDropTarget
    {
    public:
        explicit ModelFileDropTarget(MainFrame* frame) : m_frame(frame) {}

        bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override
        {
            if (!m_frame || filenames.IsEmpty())
                return false;

            // Partition into supported and unsupported by extension before doing
            // any work, so the user is warned about unreadable files up front
            // rather than after several long imports have already run.
            wxArrayString accepted;
            wxArrayString rejected;
            accepted.reserve(filenames.size());
            for (const wxString& f : filenames) {
                if (IsSupportedModelExt(f))
                    accepted.Add(f);
                else
                    rejected.Add(wxFileName(f).GetFullName());
            }

            if (!rejected.IsEmpty()) {
                wxString msg = "The following file(s) cannot be imported "
                    "(only STEP, STL and OBJ are supported):\n\n";
                for (const wxString& name : rejected)
                    msg << "    " << name << "\n";
                wxMessageBox(msg, "Unsupported file type",
                    wxOK | wxICON_WARNING, m_frame);
            }

            if (accepted.IsEmpty())
                return false;

            GLCanvas* canvas = m_frame->GetCanvas();
            if (!canvas)
                return false;

            // ImportFile shows its own progress dialog, so multi-file drops will
            // display one progress per file in sequence.
            for (const wxString& path : accepted)
                canvas->ImportFile(path.ToStdString());

            return true;
        }

    private:
        MainFrame* m_frame;  // not owned
    };

}  // namespace

// ---------------------------------------------------------------------------
// MainFrame
// ---------------------------------------------------------------------------
MainFrame::MainFrame(const FixtureDefinition& fixture)
    : wxFrame(nullptr, wxID_ANY, "Mould3r - New Project",
        wxDefaultPosition, wxSize(1200, 800))
    , m_fixtureDef(fixture)
{
    // ---- Window icon from SVG -----------------------------------------------
    {
        wxBitmapBundle iconBundle = LoadSvgBundle(kAppIconSvg, wxSize(32, 32));
        if (iconBundle.IsOk())
        {
            wxIcon icon;
            icon.CopyFromBitmap(iconBundle.GetBitmapFor(this));
            SetIcon(icon);
        }
    }

    // ---- Menu bars (per perspective) ---------------------------------------
    // Both are built up front and swapped by SetPerspective. The frame starts
    // in the Prepare perspective below, so attach its bar now.
    m_prepareMenuBar = BuildPrepareMenuBar();
    m_previewMenuBar = BuildPreviewMenuBar();
    SetMenuBar(m_prepareMenuBar);

    Bind(wxEVT_MENU, &MainFrame::OnExit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::OnImport, this, ID_Import);
    Bind(wxEVT_MENU, &MainFrame::OnCreateFixture, this, ID_CreateFixture);
    Bind(wxEVT_MENU, &MainFrame::OnChangeFixture, this, ID_ChangeFixture);
    Bind(wxEVT_MENU, &MainFrame::OnEditFixture, this, ID_EditFixture);
    Bind(wxEVT_UPDATE_UI, &MainFrame::OnUpdateEditFixture, this, ID_EditFixture);
    Bind(wxEVT_MENU, &MainFrame::OnSaveProject, this, ID_SaveProject);
    Bind(wxEVT_MENU, &MainFrame::OnLoadProject, this, ID_LoadProject);
    Bind(wxEVT_MENU, &MainFrame::OnNewProject, this, ID_NewProject);
    Bind(wxEVT_MENU, &MainFrame::OnSetMetric, this, ID_UnitMetric);
    Bind(wxEVT_MENU, &MainFrame::OnSetImperial, this, ID_UnitImperial);

    // Grid menu: the consolidated settings dialog.
    Bind(wxEVT_MENU, &MainFrame::OnGridSettings, this, ID_GridSettings);
    Bind(wxEVT_MENU, &MainFrame::OnAbout, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::OnCheckForUpdates, this, ID_CheckForUpdates);
    Bind(wxEVT_MENU, &MainFrame::OnToggleAutoUpdateCheck, this, ID_AutoUpdateCheck);

    // Mesh quality radio items just persist the chosen preset; the next
    // import picks it up via MeshImportSettings::GetQuality().
    Bind(wxEVT_MENU,
        [](wxCommandEvent&) { MeshImportSettings::SetQuality(MeshImportSettings::Quality::Off); },
        ID_MeshQualityOff);
    Bind(wxEVT_MENU,
        [](wxCommandEvent&) { MeshImportSettings::SetQuality(MeshImportSettings::Quality::Draft); },
        ID_MeshQualityDraft);
    Bind(wxEVT_MENU,
        [](wxCommandEvent&) { MeshImportSettings::SetQuality(MeshImportSettings::Quality::Normal); },
        ID_MeshQualityNormal);
    Bind(wxEVT_MENU,
        [](wxCommandEvent&) { MeshImportSettings::SetQuality(MeshImportSettings::Quality::High); },
        ID_MeshQualityHigh);

    // ---- Layout: ribbon on top, canvas below --------------------------------
    auto* root = new wxPanel(this, wxID_ANY);
    root->SetBackgroundColour(kRibbonBg);

    auto* vSizer = new wxBoxSizer(wxVERTICAL);

    wxPanel* ribbon = CreateRibbon(root);

    // 1-px separator line
    auto* sep = new wxPanel(root, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    sep->SetBackgroundColour(Style::Accent);

    // ---- Perspective pager -------------------------------------------------
    // A wxSimplebook stacks the two workflow perspectives; only one is shown at
    // a time. Page 0 = Prepare (left panel + main canvas), page 1 = Preview
    // (the embedded PreviewPanel). The ribbon's Prepare/Preview buttons drive
    // the selection via SetPerspective.
    m_book = new wxSimplebook(root, wxID_ANY);
    m_book->SetBackgroundColour(kRibbonBg);

    // ---- Prepare page: left panel + main canvas ----------------------------
    m_preparePage = new wxPanel(m_book, wxID_ANY);
    m_preparePage->SetBackgroundColour(kRibbonBg);
    auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);
    m_canvas = new GLCanvas(m_preparePage);
    m_leftPanel = CreateLeftPanel(m_preparePage);

    // Drag-and-drop import: drop STEP/STL/OBJ files onto the 3D viewport
    // to import them. wxWidgets takes ownership of the drop target.
    m_canvas->SetDropTarget(new ModelFileDropTarget(this));

    // Register the scene-mutation hook. Any time the canvas mutates in a
    // way that would stale a generated mould — object transforms, feature
    // place / remove, sprue placement — it fires this callback, which
    // bumps the mould state from Clean to Dirty. NeverGenerated stays
    // NeverGenerated (mutating an unbuilt mould isn't meaningful — we
    // wait for an actual Generate run to leave that state). Project-level
    // mutations (import, fixture swap, load) handle their own state reset
    // directly in the relevant handlers below.
    m_canvas->SetOnSceneMutated([this] {
        if (m_mouldState == MouldState::Clean)
            m_mouldState = MouldState::Dirty;
    });

    // ---- Part 5: floating complex-vent-path toolbar ------------------------
    // Created as a SIBLING of the canvas (child of the Prepare page) and raised
    // above it, then pinned over the viewport by the canvas. A true child of a
    // wxGLCanvas can be overdrawn by SwapBuffers on some drivers; a raised
    // sibling clips reliably. Shown only while a vent is being edited.
    m_ventEditToolbar = new VentEditToolbar(m_preparePage);
    m_ventEditToolbar->SetOnTool([this](PathEditTool t) {
        if (m_canvas) m_canvas->SetPathEditTool(t);
    });
    // Place... — Precision Place the selected node. The canvas vets eligibility,
    // and the cell is only enabled when GetSelectedPlaceableNode() is valid.
    m_ventEditToolbar->SetOnPlaceNode([this]() {
        if (!m_canvas) return;
        const int node = m_canvas->GetSelectedPlaceableNode();
        if (node >= 0) PrecisionPlaceEditNode(node);
    });
    m_ventEditToolbar->SetOnSmooth([this](bool on) {
        if (!m_canvas) return;
        if (m_canvas->IsEditingRunner()) m_canvas->SetEditRunnerSmooth(on);
        else if (m_canvas->IsEditingGate())   m_canvas->SetEditGateSmooth(on);
        else                                  m_canvas->SetEditVentSmooth(on);
    });
    m_ventEditToolbar->SetOnToggleComplex([this](bool wantComplex) {
        if (!m_canvas) return;
        if (m_canvas->IsEditingRunner())
        {
            if (wantComplex) m_canvas->ConvertEditRunnerToComplex();
            else             m_canvas->ConvertEditRunnerToSimple();
        }
        else if (m_canvas->IsEditingGate())
        {
            if (wantComplex) m_canvas->ConvertEditGateToComplex();
            else             m_canvas->ConvertEditGateToSimple();
        }
        else
        {
            if (wantComplex) m_canvas->ConvertEditVentToComplex();
            else             m_canvas->ConvertEditVentToSimple();
        }
    });
    m_canvas->SetPathToolbar(m_ventEditToolbar);

    // ---- Edit Sprue floating toolbar ---------------------------------------
    // Same sibling-of-the-canvas arrangement as the vent toolbar; the two never
    // show together (their modes are exclusive) and share the top-centre slot.
    m_sprueEditToolbar = new SprueEditToolbar(m_preparePage);
    m_sprueEditToolbar->SetOnTool([this](SprueEditTool t) {
        if (m_canvas) m_canvas->SetSprueEditTool(t);
    });
    m_canvas->SetSprueToolbar(m_sprueEditToolbar);

    // One canvas hook drives BOTH edit toolbars; each shows itself only in its
    // own mode and hides otherwise.
    m_canvas->SetOnPathEditChanged([this] {
        UpdateVentEditToolbar();
        UpdateSprueEditToolbar();
    });
    UpdateVentEditToolbar();    // initial (hidden) state
    UpdateSprueEditToolbar();   // initial (hidden) state

    // ---- Bottom-centre hint overlay ----------------------------------------
    // Same sibling-of-the-canvas arrangement as the toolbar above. Registered
    // with the canvas so it stays pinned through viewport resizes; content and
    // visibility are driven from SetActiveTool.
    m_canvasToast = new CanvasToast(m_preparePage);
    m_canvas->SetCanvasToast(m_canvasToast);

    contentSizer->Add(m_leftPanel, 0, wxEXPAND);
    contentSizer->Add(m_canvas, 1, wxEXPAND);
    m_preparePage->SetSizer(contentSizer);

    // ---- Preview page: the embedded preview perspective --------------------
    m_previewPanel = new PreviewPanel(m_book);

    m_book->AddPage(m_preparePage, "Prepare");
    m_book->AddPage(m_previewPanel, "Preview");
    m_book->SetSelection(0);

    vSizer->Add(ribbon, 0, wxEXPAND);
    vSizer->Add(sep, 0, wxEXPAND);
    vSizer->Add(m_book, 1, wxEXPAND);

    root->SetSizer(vSizer);

    // Reflect the initial (Prepare) perspective: page, menu bar, ribbon
    // buttons, and the Generate/Export visibility are all set consistently.
    SetPerspective(Perspective::Prepare);

    // Frame sizer
    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(root, 1, wxEXPAND);
    SetSizer(frameSizer);

    // Start with Select active
    SetActiveTool(TransformMode::Select);

    // Load models from startup config (may be absent on first launch — the
    // app now opens an empty frame and prompts for a fixture afterward via
    // PromptForFixtureIfMissing()).
    if (fixture.IsValid())
    {
        LoadFixtureIntoScene(fixture);

        // Set the active injection point (first in the list for now)
        m_canvas->SetInjectionPoints(fixture.injectionPoints);
        m_canvas->SetAllowPerimeterInjection(fixture.allowPerimeterInjection);
        if (!fixture.injectionPoints.empty())
            m_canvas->SetActiveInjectionPoint(fixture.injectionPoints[0]);

        // Apply any per-feature default overrides the fixture supplied.
        // Safe here because CreateLeftPanel ran above, so all field/choice
        // pointers are populated.
        ApplyFixtureDefaults(fixture);
    }

    // Win11 DWM corner rounding for the main frame — matches the rest
    // of the app's window family.
    WindowEffects::ApplyRoundedCorners(this);

    // ---- Background update check (U4) --------------------------------------
    // Throttled to once per 24h and gated on the user preference; both live
    // in UpdateStartupPolicy. The 3s delay keeps the network request out of
    // the launch path entirely — GL init, fixture load and the startup
    // dialog all settle first, and a slow DNS lookup can't lengthen any of
    // them. Everything after this point is silent unless a genuinely newer,
    // non-skipped version is published.
    if (UpdateStartupPolicy::DueForAutoCheck())
    {
        m_startupUpdateTimer.SetOwner(this, ID_StartupUpdateTimer);
        Bind(wxEVT_TIMER,
            [this](wxTimerEvent&) { StartStartupUpdateCheck(); },
            ID_StartupUpdateTimer);
        m_startupUpdateTimer.StartOnce(3000);
    }
}

// ---------------------------------------------------------------------------
// StartStartupUpdateCheck — the silent background check. Every failure path
// is a no-op by design: errors, timeouts, malformed manifests, and
// up-to-date all end here without a single pixel changing. Only "newer
// version, not skipped" produces UI.
// ---------------------------------------------------------------------------
void MainFrame::StartStartupUpdateCheck()
{
    // The attempt is recorded up front (not on success), so an offline
    // machine probes once per throttle window instead of on every launch.
    UpdateStartupPolicy::RecordCheckAttempt();

    m_startupChecker = std::make_unique<UpdateChecker>();

    const bool started = m_startupChecker->Start(
        [this](const UpdateChecker::Result& r)
    {
        if (r.outcome == UpdateChecker::Outcome::UpdateAvailable
            && !UpdateStartupPolicy::IsVersionSkipped(r.latestVersion))
        {
            // Same preference chain as the manual dialog: changelog,
            // then installer, then the website.
            wxString target = r.notesUrl;
            if (target.empty()) target = r.downloadUrl;
            if (target.empty()) target = Mould3r::Version::Website;

            ShowUpdateBanner(r.latestVersion, target);
        }

        // The checker must not be destroyed from inside its own
        // callback (Finish() still has frames on the stack) — defer.
        CallAfter([this] { m_startupChecker.reset(); });
    });

    if (!started)
        m_startupChecker.reset();   // no backend — silently give up
}

// ---------------------------------------------------------------------------
// ShowUpdateBanner — lazily builds the banner (child of m_preparePage,
// sibling of the canvas, same overlay family as the toast and the path
// toolbar) and pins it bottom-right.
// ---------------------------------------------------------------------------
void MainFrame::ShowUpdateBanner(const wxString& latestVersion,
    const wxString& targetUrl)
{
    if (!m_updateBanner)
    {
        m_updateBanner = new UpdateBanner(m_preparePage);

        // Keep it pinned through resizes. The toast and path toolbar are
        // repinned by GLCanvas because they track the canvas rect; the
        // banner tracks the PAGE's bottom-right corner, so the page's own
        // size event is the right hook and GLCanvas stays untouched.
        m_preparePage->Bind(wxEVT_SIZE, [this](wxSizeEvent& e)
        {
            PositionUpdateBanner();
            e.Skip();
        });
    }

    m_updateBanner->onView = [this, targetUrl]
    {
        wxLaunchDefaultBrowser(targetUrl);
    };
    m_updateBanner->onSkip = [latestVersion]
    {
        UpdateStartupPolicy::SkipVersion(latestVersion);
    };
    m_updateBanner->onDismiss = [] { /* 24h throttle is the snooze */ };

    m_updateBanner->ShowUpdate(latestVersion);
    PositionUpdateBanner();
}

void MainFrame::PositionUpdateBanner()
{
    if (!m_updateBanner || !m_preparePage)
        return;

    const wxSize page = m_preparePage->GetClientSize();
    const wxSize sz = m_updateBanner->GetSize();
    m_updateBanner->SetPosition(wxPoint(
        page.x - sz.x - 16,
        page.y - sz.y - 16));
    m_updateBanner->Raise();
}

// ---------------------------------------------------------------------------
// OnToggleAutoUpdateCheck — persists the preference and mirrors the checked
// state onto both menu bars (each Help menu is a separate instance).
// ---------------------------------------------------------------------------
void MainFrame::OnToggleAutoUpdateCheck(wxCommandEvent& evt)
{
    const bool enabled = evt.IsChecked();
    UpdateStartupPolicy::SetAutoCheckEnabled(enabled);

    if (m_prepareMenuBar && m_prepareMenuBar->FindItem(ID_AutoUpdateCheck))
        m_prepareMenuBar->Check(ID_AutoUpdateCheck, enabled);
    if (m_previewMenuBar && m_previewMenuBar->FindItem(ID_AutoUpdateCheck))
        m_previewMenuBar->Check(ID_AutoUpdateCheck, enabled);
}

// ---------------------------------------------------------------------------
// Destructor — the frame auto-destroys whichever menu bar is currently
// attached; the other (detached) one is ours to free.
// ---------------------------------------------------------------------------
MainFrame::~MainFrame()
{
    // Destroy the modeless insert editor first: it's parented to this frame, so
    // leaving it for the base wxFrame teardown would fire its close-notify into
    // an already-destructed MainFrame. (Helper, not inlined: InsertEditDialog is
    // an incomplete type here — its definition is further down the .cpp.)
    DestroyInsertEditor();

    wxMenuBar* attached = GetMenuBar();
    if (m_prepareMenuBar && m_prepareMenuBar != attached)
    {
        delete m_prepareMenuBar;
        m_prepareMenuBar = nullptr;
    }
    if (m_previewMenuBar && m_previewMenuBar != attached)
    {
        delete m_previewMenuBar;
        m_previewMenuBar = nullptr;
    }
}

// ---------------------------------------------------------------------------
// BuildPrepareMenuBar — the full editing menu set (File / Fixture / Units /
// Import). Reflects persisted settings in the radio state.
// ---------------------------------------------------------------------------
wxMenuBar* MainFrame::BuildPrepareMenuBar()
{
    auto* fileMenu = new wxMenu();
    fileMenu->Append(ID_NewProject, "New Project...\tCtrl+N");
    fileMenu->Append(ID_LoadProject, "Open Project...\tCtrl+O");
    fileMenu->Append(ID_SaveProject, "Save Project...\tCtrl+S");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT, "Exit\tAlt+F4");

    auto* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");

    // Fixture menu — top-level so the two fixture actions surface together
    // rather than hiding under File. Create opens the FixtureEditor (the
    // floating authoring window); Change opens the StartupDialog picker
    // for swapping between already-saved fixtures.
    auto* fixtureMenu = new wxMenu();
    fixtureMenu->Append(ID_CreateFixture, "Create Fixture...");
    fixtureMenu->Append(ID_ChangeFixture, "Change Fixture...");
    fixtureMenu->Append(ID_EditFixture, "Edit Fixture Dimensions...");
    menuBar->Append(fixtureMenu, "&Fixture");

    // Grid menu — edit the ground-plane grid's shape, size and spacing.
    menuBar->Append(BuildGridMenu(), "&Grid");

    auto* unitsMenu = new wxMenu();
    unitsMenu->AppendRadioItem(ID_UnitMetric, "Metric (mm)");
    unitsMenu->AppendRadioItem(ID_UnitImperial, "Imperial (in)");
    unitsMenu->Check(ID_UnitMetric, true);
    menuBar->Append(unitsMenu, "&Units");

    // ---- Import menu: action + mesh-simplification settings ----------------
    // Settings are applied to STL/OBJ imports only; STEP tessellation is
    // governed by its own deflection parameters.
    auto* importMenu = new wxMenu();
    importMenu->Append(ID_Import, "Import Model...\tCtrl+I");
    importMenu->AppendSeparator();
    wxMenuItem* meshHeader =
        importMenu->Append(wxID_ANY, "Mesh Simplification (STL/OBJ):");
    meshHeader->Enable(false);  // visual label only
    importMenu->AppendRadioItem(ID_MeshQualityOff, "Off (no simplification)");
    importMenu->AppendRadioItem(ID_MeshQualityDraft, "Draft (~2,000 triangles)");
    importMenu->AppendRadioItem(ID_MeshQualityNormal, "Normal (~10,000 triangles)");
    importMenu->AppendRadioItem(ID_MeshQualityHigh, "High (~50,000 triangles)");
    menuBar->Append(importMenu, "&Import");

    // Reflect the persisted setting in the radio state.
    {
        const auto q = MeshImportSettings::GetQuality();
        importMenu->Check(ID_MeshQualityOff, q == MeshImportSettings::Quality::Off);
        importMenu->Check(ID_MeshQualityDraft, q == MeshImportSettings::Quality::Draft);
        importMenu->Check(ID_MeshQualityNormal, q == MeshImportSettings::Quality::Normal);
        importMenu->Check(ID_MeshQualityHigh, q == MeshImportSettings::Quality::High);
    }

    menuBar->Append(BuildHelpMenu(), "&Help");

    return menuBar;
}

// ---------------------------------------------------------------------------
// BuildGridMenu — Shape submenu (Rectangular / Circular radio) plus the two
// "Change..." actions. The shape governs which fields the size dialog shows,
// so it's a persistent choice reflected here from m_gridSettings.
// ---------------------------------------------------------------------------
wxMenu* MainFrame::BuildGridMenu()
{
    // All grid settings live in one consolidated dialog (shape / size /
    // spacing / major divisions), since the fields are related and some
    // predicate others.
    auto* gridMenu = new wxMenu();
    gridMenu->Append(ID_GridSettings, "Grid Settings...");
    return gridMenu;
}

// ---------------------------------------------------------------------------
// BuildHelpMenu — built fresh per call rather than shared, because a wxMenu
// is owned by the wxMenuBar it's appended to; handing the same pointer to
// both the Prepare and Preview bars would double-delete it on shutdown.
// Both bars bind to the same wxID_ABOUT handler, so the two instances stay
// behaviourally identical.
//
// ---------------------------------------------------------------------------
wxMenu* MainFrame::BuildHelpMenu()
{
    auto* helpMenu = new wxMenu();
    helpMenu->Append(ID_CheckForUpdates, "Check for Updates...");

    // Built fresh per menu bar, so each instance reads its checked state
    // from config here; the toggle handler re-syncs BOTH bars by id so the
    // two can't disagree after a toggle.
    auto* autoItem = helpMenu->AppendCheckItem(ID_AutoUpdateCheck,
        "Check for Updates Automatically");
    autoItem->Check(UpdateStartupPolicy::AutoCheckEnabled());

    helpMenu->AppendSeparator();
    helpMenu->Append(wxID_ABOUT, "About Mould3r...");
    return helpMenu;
}

// ---------------------------------------------------------------------------
// BuildPreviewMenuBar — minimal for now (File -> Exit). Grows as preview-
// specific actions (e.g. a View menu for the debug overlays) are added.
// ---------------------------------------------------------------------------
wxMenuBar* MainFrame::BuildPreviewMenuBar()
{
    auto* fileMenu = new wxMenu();
    fileMenu->Append(wxID_EXIT, "Exit\tAlt+F4");

    auto* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    menuBar->Append(BuildHelpMenu(), "&Help");
    return menuBar;
}

// ---------------------------------------------------------------------------
// SetPerspective — switch the active workflow perspective: flip the book page,
// swap in the matching menu bar, recolour the ribbon buttons, and (for Preview)
// flush any pending GL upload now that the page is visible.
// ---------------------------------------------------------------------------
void MainFrame::SetPerspective(Perspective which)
{
    m_perspective = which;

    if (m_book)
        m_book->SetSelection(which == Perspective::Preview ? 1 : 0);

    SetMenuBar(which == Perspective::Preview ? m_previewMenuBar
        : m_prepareMenuBar);

    // Primary action is perspective-specific: Generate Mould in Prepare,
    // Export in Preview. They share the top-right slot, so show one and hide
    // the other, then relayout the ribbon so the visible one sits flush right.
    const bool preview = (which == Perspective::Preview);
    if (m_btnGenerate) m_btnGenerate->Show(!preview);
    if (m_btnExport)   m_btnExport->Show(preview);
    if (m_btnGenerate && m_btnGenerate->GetParent())
        m_btnGenerate->GetParent()->Layout();

    UpdatePerspectiveButtons();

    if (which == Perspective::Preview && m_previewPanel)
    {
        // Keep the preview's ground-plane grid in step with the Prepare
        // perspective. The Grid menu lives only in Prepare, so settings can't
        // change while Preview is showing — syncing on entry is sufficient.
        m_previewPanel->SetGridSettings(m_gridSettings);
        m_previewPanel->FlushIfDirty();
    }
}

void MainFrame::OnPerspectivePrepare(wxCommandEvent&)
{
    SetPerspective(Perspective::Prepare);
}

void MainFrame::OnPerspectivePreview(wxCommandEvent&)
{
    SetPerspective(Perspective::Preview);
}

// ---------------------------------------------------------------------------
// Recolour the ribbon perspective buttons so the active one reads as selected.
// ---------------------------------------------------------------------------
void MainFrame::UpdatePerspectiveButtons()
{
    const bool preview = (m_perspective == Perspective::Preview);
    if (m_btnPrepare) m_btnPrepare->SetActive(!preview);
    if (m_btnPreview) m_btnPreview->SetActive(preview);
}


// ---------------------------------------------------------------------------
// CreateRibbon  – horizontal panel with labelled tool groups
// ---------------------------------------------------------------------------
wxPanel* MainFrame::CreateRibbon(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 48));
    panel->SetBackgroundColour(kRibbonBg);

    auto* hSizer = new wxBoxSizer(wxHORIZONTAL);

    hSizer->AddSpacer(12);

    // ---- App logo (SVG, replaces text title) --------------------------------
    {
        wxBitmapBundle logoBndle = LoadSvgBundle(kRibbonLogoSvg, wxSize(120, 28));
        if (logoBndle.IsOk())
        {
            auto* logo = new wxStaticBitmap(panel, wxID_ANY,
                logoBndle.GetBitmapFor(panel));
            hSizer->Add(logo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
        }
    }

    // ---- Perspective switch (Prepare / Preview) ----------------------------
    // Flat tab strip leading the ribbon: two adjacent square tabs. The active
    // one fills with the lighter card colour + a bottom accent line; the
    // inactive blends into the bar. Full bar height, no gap between them.
    // SetPerspective drives the active state via UpdatePerspectiveButtons.
    auto makePerspectiveBtn = [&](int id, const wxString& label) -> PerspectiveButton*
    {
        auto* b = new PerspectiveButton(panel, id, label,
            wxDefaultPosition, wxSize(120, -1));
        b->SetBackgroundColour(Style::CardBg);   // active-tab fill
        b->SetForegroundColour(*wxWHITE);
        b->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
        return b;
    };
    m_btnPrepare = makePerspectiveBtn(ID_PerspectivePrepare, "Prepare");
    m_btnPreview = makePerspectiveBtn(ID_PerspectivePreview, "Preview");
    hSizer->Add(m_btnPrepare, 0, wxEXPAND | wxLEFT, 8);
    hSizer->Add(m_btnPreview, 0, wxEXPAND);

    hSizer->AddStretchSpacer();

    // ---- Primary action (right-aligned) ------------------------------------
    // Perspective-specific: "Generate Mould" shows in the Prepare perspective,
    // the Export split button shows in Preview. They occupy the same top-right
    // slot (only one is ever visible — SetPerspective toggles them). Generate
    // is a plain green pill; Export is a split button whose dropdown zone picks
    // the mode. Both are added after the stretch spacer, so whichever is shown
    // sits flush right.
    auto* btnGenerate = new RoundedButton(panel, ID_GenerateMould, "Generate Mould",
        wxDefaultPosition, wxSize(130, 32), wxBORDER_NONE);
    btnGenerate->SetBackgroundColour(Style::BtnGenerate);
    btnGenerate->SetForegroundColour(*wxWHITE);
    btnGenerate->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    m_btnGenerate = btnGenerate;
    hSizer->Add(btnGenerate, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    // Export split button. Action zone runs the current mode; dropdown zone
    // picks it. Menu-item order is fixed and mirrored by MainFrame::ExportMode:
    //   index 0 -> Mould, index 1 -> Shot body.
    auto* btnExport = new SplitButton(panel, ID_Export, "Export Mould",
        wxDefaultPosition, wxSize(170, 32), wxBORDER_NONE);
    btnExport->SetBackgroundColour(Style::BtnGenerate);
    btnExport->SetForegroundColour(*wxWHITE);
    btnExport->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    btnExport->SetMenuItems({ "Mould", "Shot body" });
    btnExport->SetSelection(0);
    m_btnExport = btnExport;
    btnExport->Hide();   // Prepare is the initial perspective
    hSizer->Add(btnExport, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    panel->SetSizer(hSizer);

    // ---- Bind events -------------------------------------------------------
    Bind(wxEVT_BUTTON, &MainFrame::OnImport, this, ID_Import);
    Bind(wxEVT_BUTTON, &MainFrame::OnPerspectivePrepare, this, ID_PerspectivePrepare);
    Bind(wxEVT_BUTTON, &MainFrame::OnPerspectivePreview, this, ID_PerspectivePreview);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolTranslate, this, ID_ToolTranslate);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolRotate, this, ID_ToolRotate);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolScale, this, ID_ToolScale);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolPattern, this, ID_ToolPattern);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolPrecisionPlace, this, ID_ToolPrecisionPlace);
    Bind(wxEVT_BUTTON, &MainFrame::OnToolCenter, this, ID_ToolCenter);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolAlignFace, this, ID_ToolAlignFace);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolAlignMidplane, this, ID_ToolAlignMidplane);
    Bind(wxEVT_BUTTON, &MainFrame::OnToolPlaceVent, this, ID_ToolPlaceVent);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearVentPoints, this, ID_ClearVentPoints);
    Bind(wxEVT_BUTTON, &MainFrame::OnPlaceSprue, this, ID_PlaceSprue);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearSprue, this, ID_ClearSprue);
    Bind(wxEVT_BUTTON, &MainFrame::OnEditSprue, this, ID_EditSprue);
    Bind(wxEVT_BUTTON, &MainFrame::OnPlaceRunner, this, ID_PlaceRunner);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearRunners, this, ID_ClearRunners);
    Bind(wxEVT_BUTTON, &MainFrame::OnPlaceGate, this, ID_PlaceGate);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearGates, this, ID_ClearGates);
    Bind(wxEVT_BUTTON, &MainFrame::OnPlaceEjector, this, ID_PlaceEjector);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearEjectors, this, ID_ClearEjectors);
    Bind(wxEVT_BUTTON, &MainFrame::OnPlaceInsert, this, ID_PlaceInsert);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearInserts, this, ID_ClearInserts);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveVent, this, ID_RemoveVent);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveSprue, this, ID_RemoveSprue);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveRunner, this, ID_RemoveRunner);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveGate, this, ID_RemoveGate);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveEjector, this, ID_RemoveEjector);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveInsert, this, ID_RemoveInsert);
    Bind(wxEVT_BUTTON, &MainFrame::OnEditVent, this, ID_EditVent);
    Bind(wxEVT_BUTTON, &MainFrame::OnEditRunner, this, ID_EditRunner);
    Bind(wxEVT_BUTTON, &MainFrame::OnEditGate, this, ID_EditGate);
    Bind(wxEVT_BUTTON, &MainFrame::OnEditEjector, this, ID_EditEjector);
    Bind(wxEVT_BUTTON, &MainFrame::OnEditInsert, this, ID_EditInsert);
    Bind(wxEVT_BUTTON, &MainFrame::OnGenerateMould, this, ID_GenerateMould);
    Bind(wxEVT_BUTTON, &MainFrame::OnExport, this, ID_Export);
    Bind(wxEVT_CHOICE, &MainFrame::OnExportModeChanged, this, ID_Export);

    return panel;
}

wxPanel* MainFrame::CreateSidePanel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(220, -1));
    panel->SetBackgroundColour(kRibbonBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Section label helper ----------------------------------------------
    auto addSection = [&](const wxString& text)
    {
        auto* lbl = new wxStaticText(panel, wxID_ANY, text);
        lbl->SetForegroundColour(Style::Accent);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        sizer->Add(lbl, 0, wxLEFT | wxTOP, 12);

        auto* line = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        line->SetBackgroundColour(Style::Divider);
        sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    };

    // ---- Path row helper ---------------------------------------------------
    auto addPathRow = [&](const wxString& label, wxTextCtrl*& ctrl, int browseId)
    {
        auto* lbl = new wxStaticText(panel, wxID_ANY, label);
        lbl->SetForegroundColour(kTextDefault);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        sizer->Add(lbl, 0, wxLEFT | wxTOP, 12);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        ctrl = new wxTextCtrl(panel, wxID_ANY, "",
            wxDefaultPosition, wxSize(140, 24), wxTE_READONLY);
        ctrl->SetBackgroundColour(Style::InputBg);
        ctrl->SetForegroundColour(kTextDefault);

        auto* browse = new RoundedButton(panel, browseId, "...",
            wxDefaultPosition, wxSize(28, 24));
        browse->SetBackgroundColour(Style::InputBg);
        browse->SetForegroundColour(kTextDefault);

        row->Add(ctrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    };

    // ---- Export section ----------------------------------------------------
    addSection("EXPORT");
    addPathRow("Output folder:", m_exportPath, ID_BrowseExport);

    sizer->AddStretchSpacer();

    panel->SetSizer(sizer);

    // ---- Binds -------------------------------------------------------------
    Bind(wxEVT_BUTTON, &MainFrame::OnBrowseExport, this, ID_BrowseExport);

    return panel;
}
// ---------------------------------------------------------------------------
// SetActiveTool – mutually exclusive toggle + notify canvas
//
// Drives every toggle-style ribbon button's visual state from the canonical
// TransformMode. This is the single sync point: button clicks, Escape, and
// canvas-internal mode transitions (e.g. vent placement completing) all
// route through here, so it's enough to rebuild the visuals once.
// ---------------------------------------------------------------------------
void MainFrame::SetActiveTool(TransformMode mode)
{
    // Map TransformMode -> the command ID of the toggle button that should
    // appear active. Modes without an associated toggle button (Select,
    // Center, Remove*, Edit*, ...) yield -1, which clears all buttons.
    int activeId = -1;
    switch (mode)
    {
    case TransformMode::Translate:     activeId = ID_ToolTranslate;     break;
    case TransformMode::Rotate:        activeId = ID_ToolRotate;        break;
    case TransformMode::Scale:         activeId = ID_ToolScale;         break;
    case TransformMode::Pattern:       activeId = ID_ToolPattern;       break;
    case TransformMode::PlaceVent:     activeId = ID_ToolPlaceVent;     break;
    case TransformMode::PlaceRunner:   activeId = ID_PlaceRunner;       break;
    case TransformMode::PlaceGate:     activeId = ID_PlaceGate;         break;
    case TransformMode::PlaceEjector:  activeId = ID_PlaceEjector;      break;
    case TransformMode::PlaceInsert:   activeId = ID_PlaceInsert;       break;
    case TransformMode::AlignFace:     activeId = ID_ToolAlignFace;     break;
    case TransformMode::AlignMidplane: activeId = ID_ToolAlignMidplane; break;
    default:                                                            break;
    }

    for (auto& kv : m_toolBtnSetters)
        kv.second(kv.first == activeId);

    if (m_canvas)
        m_canvas->SetTransformMode(mode);

    UpdateCanvasToast(mode);
}

// ---------------------------------------------------------------------------
// UpdateCanvasToast — show the bottom-centre hint for modes whose only other
// cue is the cursor shape, and clear it for everything else.
//
// Hung off SetActiveTool because that is the single sync point every mode
// change routes through — ribbon buttons, Escape, and the canvas-internal
// transitions that call back into the frame. Adding another mode here is a
// one-line case.
// ---------------------------------------------------------------------------
void MainFrame::UpdateCanvasToast(TransformMode mode)
{
    if (!m_canvasToast) return;

    if (mode == TransformMode::PlaceInsert)
        m_canvasToast->ShowMessage("Select body to add insert to");
    else
        m_canvasToast->HideToast();

    // ShowMessage resizes to the text, so the centring has to be redone.
    if (m_canvas) m_canvas->RepositionCanvasToast();
}

// ---------------------------------------------------------------------------
// Ribbon button handlers
// ---------------------------------------------------------------------------

void MainFrame::OnToolSelect(wxCommandEvent&) { SetActiveTool(TransformMode::Select); }

void MainFrame::OnToolTranslate(wxCommandEvent&)
{
    SetActiveTool(TransformMode::Select);

    if (!m_canvas) return;

    TranslateDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
    {
        return;
    }

    const TranslateValues v = dlg.GetValues();
    m_canvas->ApplyTranslation(v.x, v.y, v.z);
}

void MainFrame::OnToolRotate(wxCommandEvent&)
{
    // Keep the button in its previous visual state — it's a dialog, not a mode toggle
    SetActiveTool(TransformMode::Select);

    if (!m_canvas) return;

    RotateDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
    {
        return;
    }

    const RotateValues v = dlg.GetValues();
    m_canvas->ApplyRotation(v.x, v.y, v.z);
}

void MainFrame::OnToolScale(wxCommandEvent&)
{
    SetActiveTool(TransformMode::Select);

    if (!m_canvas) return;

    ScaleDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
    {
        return;
    }

    const ScaleValues v = dlg.GetValues();
    m_canvas->ApplyScale(v.uniform);
}

// ---------------------------------------------------------------------------
// Pattern — opens the PatternDialog. UI scaffolding only for now; the
// canvas-side pattern application will be wired up in a follow-up change.
// ---------------------------------------------------------------------------
void MainFrame::OnToolPattern(wxCommandEvent&)
{
    // Dialog tool, not a placement-mode toggle — clear any active toggle so
    // the button doesn't appear stuck on after the dialog closes (matches the
    // Translate/Rotate/Scale convention).
    SetActiveTool(TransformMode::Select);

    if (!m_canvas) return;

    PatternDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
        return;

    const PatternValues v = dlg.GetValues();

    switch (v.type)
    {
    case PatternValues::Type::Circular:
        m_canvas->ApplyCircularPattern(v.number, v.overrideRadius, v.radius,
            v.rotateCopies);
        break;

    case PatternValues::Type::Grid:
        m_canvas->ApplyGridPattern(v.numberH, v.numberV,
            v.mirrorH, v.mirrorV,
            v.overrideLengthWidth, v.length, v.width);
        break;
    }
}

// ---------------------------------------------------------------------------
// Precision Place — opens a dialog for an absolute XZ target and moves the
// selection there. A dialog tool (not a placement-mode toggle), so the button
// clears its toggle the moment the dialog opens, matching Translate/Rotate/
// Scale/Pattern. Also reachable by double-clicking a body on the canvas.
// ---------------------------------------------------------------------------
void MainFrame::OnToolPrecisionPlace(wxCommandEvent&)
{
    SetActiveTool(TransformMode::Select);
    PrecisionPlaceSelected();
}

void MainFrame::PrecisionPlaceSelected()
{
    if (!m_canvas) return;

    // Pre-fill the dialog with the selection's current XZ so the fields show
    // where the object is now. Falls back to (0, 0) when nothing is selected
    // (e.g. the button was pressed with an empty selection); the post-OK
    // HasSelection guard then makes the dialog a no-op, matching the other
    // transform tools.
    float curX = 0.0f, curZ = 0.0f;
    m_canvas->GetSelectionCenterXZ(curX, curZ);

    PrecisionPlaceDialog dlg(this, curX, curZ);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
        return;

    const PrecisionPlaceValues v = dlg.GetValues();
    m_canvas->MoveSelectionToXZ(v.x, v.z);
}

// ---------------------------------------------------------------------------
// PrecisionPlaceEditNode — same flow as PrecisionPlaceSelected, but for a
// single path node: pre-fill from the node's current XZ, then write the typed
// values back. The canvas owns eligibility and the invariants (staying inside
// the mould, keeping rf.point on the endpoint), so a rejected value simply
// leaves the node where it was.
// ---------------------------------------------------------------------------
void MainFrame::PrecisionPlaceEditNode(int nodeIdx)
{
    if (!m_canvas) return;

    float curX = 0.0f, curZ = 0.0f;
    if (!m_canvas->GetEditNodeXZ(nodeIdx, curX, curZ))
        return;   // not an eligible node — nothing to place

    PrecisionPlaceDialog dlg(this, curX, curZ);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const PrecisionPlaceValues v = dlg.GetValues();
    m_canvas->MoveEditNodeToXZ(nodeIdx, v.x, v.z);
}

void MainFrame::OnToolCenter(wxCommandEvent&)
{
    if (!m_canvas) return;

    if (!m_canvas->HasSelection())
    {
        return;
    }

    m_canvas->CenterSelectedObject();
}

// ---------------------------------------------------------------------------
// Align Face / Align Midplane — UI scaffolding only.
// Behaviour will be wired up in a follow-up change. The buttons toggle their
// own visual state inside makeToolBtn, so for now these handlers can stay
// empty without affecting the ribbon's appearance.
// ---------------------------------------------------------------------------
void MainFrame::OnToolAlignFace(wxCommandEvent&)
{
    // Toggle: if already in AlignFace mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::AlignFace)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::AlignFace);
}

void MainFrame::OnToolAlignMidplane(wxCommandEvent&)
{
    // Toggle: if already in AlignMidplane mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::AlignMidplane)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::AlignMidplane);
}

void MainFrame::OnToolPlaceVent(wxCommandEvent&)
{
    // Toggle: if already in PlaceVent mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::PlaceVent)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::PlaceVent);
}

void MainFrame::OnClearVentPoints(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearVentPoints();
}

void MainFrame::OnPlaceSprue(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->PlaceSprue();
}

void MainFrame::OnClearSprue(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearSprue();
}

void MainFrame::OnPlaceRunner(wxCommandEvent&)
{
    // Toggle: if already in PlaceRunner mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::PlaceRunner)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::PlaceRunner);
}

void MainFrame::OnClearRunners(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearRunnerPoints();
}

void MainFrame::OnPlaceGate(wxCommandEvent&)
{
    // Toggle: if already in PlaceGate mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::PlaceGate)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::PlaceGate);
}

void MainFrame::OnClearGates(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearGatePoints();
}

void MainFrame::OnRemoveVent(wxCommandEvent&)
{
    // Toggle: if already in RemoveVent mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveVent)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveVent);
}

void MainFrame::OnRemoveSprue(wxCommandEvent&)
{
    // Toggle: if already in RemoveSprue mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveSprue)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveSprue);
}

void MainFrame::OnRemoveRunner(wxCommandEvent&)
{
    // Toggle: if already in RemoveRunner mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveRunner)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveRunner);
}

void MainFrame::OnRemoveGate(wxCommandEvent&)
{
    // Toggle: if already in RemoveGate mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveGate)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveGate);
}

void MainFrame::OnEditVent(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::EditVent)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::EditVent);
}

// ---------------------------------------------------------------------------
// UpdateVentEditToolbar — sync the floating toolbar to the canvas's current
// vent-edit state. Bound to the canvas path-edit-changed hook, so it fires on
// mode changes, vent (de)selection, Simple<->Complex conversion, smooth toggle,
// and node add / remove.
// ---------------------------------------------------------------------------
void MainFrame::UpdateVentEditToolbar()
{
    if (!m_canvas || !m_ventEditToolbar) return;

    if (m_canvas->IsEditingVent())
    {
        // Visible as soon as Edit Vent mode is entered — even with nothing
        // selected — so Add Node (which snaps onto any path) is reachable and
        // the tools are discoverable. Per-cell enabling reflects the selection.
        m_ventEditToolbar->SetLabels("EDIT VENT PATH", "Select a vent path");
        m_ventEditToolbar->Configure(
            m_canvas->HasEditVentSelected(),
            m_canvas->IsEditVentComplex(),
            m_canvas->IsEditVentSmooth(),
            m_canvas->EditVentNodeCount(),
            m_canvas->GetPathEditTool(),
            m_canvas->GetSelectedPlaceableNode() >= 0);
        if (!m_ventEditToolbar->IsShown())
            m_ventEditToolbar->Show();
        m_ventEditToolbar->Raise();   // keep above the GL surface
    }
    else if (m_canvas->IsEditingRunner())
    {
        // Same shared overlay, retitled for runners. node[0] is pinned to the
        // sprue feed point and the endpoint is free (no perimeter snap).
        m_ventEditToolbar->SetLabels("EDIT RUNNER PATH", "Select a runner");
        m_ventEditToolbar->Configure(
            m_canvas->HasEditRunnerSelected(),
            m_canvas->IsEditRunnerComplex(),
            m_canvas->IsEditRunnerSmooth(),
            m_canvas->EditRunnerNodeCount(),
            m_canvas->GetPathEditTool(),
            m_canvas->GetSelectedPlaceableNode() >= 0);
        if (!m_ventEditToolbar->IsShown())
            m_ventEditToolbar->Show();
        m_ventEditToolbar->Raise();
    }
    else if (m_canvas->IsEditingGate())
    {
        // Same shared overlay, retitled for the gate's SUB-RUNNER. node[0] is
        // pinned to the gate origin; only the sub-runner gets a complex route
        // (the gate frustum stays driven by the gate-card fields).
        m_ventEditToolbar->SetLabels("EDIT SUB-RUNNER PATH", "Select a gate");
        m_ventEditToolbar->Configure(
            m_canvas->HasEditGateSelected(),
            m_canvas->IsEditGateComplex(),
            m_canvas->IsEditGateSmooth(),
            m_canvas->EditGateNodeCount(),
            m_canvas->GetPathEditTool(),
            false);   // gate sub-runner nodes are path-snapped — not eligible
        if (!m_ventEditToolbar->IsShown())
            m_ventEditToolbar->Show();
        m_ventEditToolbar->Raise();
    }
    else
    {
        if (m_ventEditToolbar->IsShown())
            m_ventEditToolbar->Hide();
    }
}

// ---------------------------------------------------------------------------
// UpdateSprueEditToolbar — sync the Edit Sprue floating toolbar to the canvas.
// Bound (with UpdateVentEditToolbar) to the canvas path-edit-changed hook, so
// it fires on mode changes and on sub-tool / injection-point changes. Shown
// only in EditSprue mode; hidden otherwise.
// ---------------------------------------------------------------------------
void MainFrame::UpdateSprueEditToolbar()
{
    if (!m_canvas || !m_sprueEditToolbar) return;

    if (m_canvas->IsEditingSprue())
    {
        m_sprueEditToolbar->Configure(
            m_canvas->HasSprueForEdit() && m_canvas->IsActiveSprueRadial(),
            m_canvas->HasInjectionChoices() || m_canvas->AllowsPerimeterInjection(),
            m_canvas->GetSprueEditTool());
        if (!m_sprueEditToolbar->IsShown())
            m_sprueEditToolbar->Show();
        m_sprueEditToolbar->Raise();   // keep above the GL surface
    }
    else
    {
        if (m_sprueEditToolbar->IsShown())
            m_sprueEditToolbar->Hide();
    }
}

void MainFrame::OnEditRunner(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::EditRunner)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::EditRunner);
}

void MainFrame::OnEditGate(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::EditGate)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::EditGate);
}

// ---------------------------------------------------------------------------
// Ejector handlers — UI scaffolding only.
//
// PlaceEjector / RemoveEjector / EditEjector toggle into their respective
// modes via SetActiveTool; the canvas-side mode-change handlers are
// placeholders right now (see GLCanvas.cpp), so clicking these buttons
// activates the toggle visually and switches mode but does not yet perform
// any geometry. ClearEjectors routes through the canvas helper, which
// likewise no-ops until ejector storage is wired up. These hooks let the
// UI ship now and let the canvas-side implementation drop in later
// without touching MainFrame.
// ---------------------------------------------------------------------------
void MainFrame::OnPlaceEjector(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::PlaceEjector)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::PlaceEjector);
}

void MainFrame::OnClearEjectors(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearEjectors();
}

void MainFrame::OnRemoveEjector(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveEjector)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveEjector);
}

void MainFrame::OnEditEjector(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::EditEjector)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::EditEjector);
}

// ===========================================================================
// InsertEditDialog — modeless transform editor for a single insert.
//
// Design notes:
//  * MODELESS + live-apply. Shown with Show(), not ShowModal(), so it stays up
//    while the user makes repeated small nudges — it never "collapses" after an
//    edit. Every field change is pushed straight to the canvas, which reanchors
//    and redraws, so the viewport tracks the spinners in real time.
//  * REPOSITIONABLE. wxDEFAULT_DIALOG_STYLE gives a native caption the user can
//    drag anywhere; wxSTAY_ON_TOP keeps it floating over the model (and matches
//    the app's other tool dialogs, e.g. PrecisionPlaceDialog) so it can't get
//    lost behind the main window mid-tune.
//  * Targets an insert by STABLE ID, not index, so a remove elsewhere can't
//    silently retarget it. On any apply, a failed lookup means the insert is
//    gone and the dialog closes itself. The frame also validates it after
//    structural changes (ValidateInsertEditor).
//  * wxSpinCtrlDouble everywhere: the increment arrows are exactly the "smaller
//    repositions to fine tune" affordance asked for. Position is in mm, uniform
//    scale is unitless (matches SceneObject's single-float scale, and keeps the
//    resolved world matrix representable as a gp_Trsf for the OCC cut).
// ===========================================================================
class InsertEditDialog : public wxDialog
{
public:
    InsertEditDialog(MainFrame* frame, GLCanvas* canvas, int insertId)
        : wxDialog(frame, wxID_ANY, "Edit Insert",
            wxDefaultPosition, wxDefaultSize,
            wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
        , m_frame(frame), m_canvas(canvas), m_insertId(insertId)
    {
        auto* main = new wxBoxSizer(wxVERTICAL);

        auto addHeader = [&](const wxString& text)
        {
            auto* lbl = new wxStaticText(this, wxID_ANY, text);
            wxFont fnt = lbl->GetFont();
            fnt.SetWeight(wxFONTWEIGHT_BOLD);
            lbl->SetFont(fnt);
            main->Add(lbl, 0, wxLEFT | wxRIGHT | wxTOP, 12);
        };

        // One spinner row. `digits`/`inc`/min/max tune the field to its role:
        // position gets a fine 0.5 mm step, rotation 1 deg, scale 0.05x.
        auto addSpin = [&](const wxString& label, wxSpinCtrlDouble*& ctrl,
            double lo, double hi, double inc, unsigned digits,
            const wxString& unit)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(this, wxID_ANY, label,
                wxDefaultPosition, wxSize(72, -1));
            ctrl = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                wxDefaultPosition, wxSize(110, -1),
                wxSP_ARROW_KEYS, lo, hi, 0.0, inc);
            ctrl->SetDigits(digits);
            auto* u = new wxStaticText(this, wxID_ANY, unit,
                wxDefaultPosition, wxSize(24, -1));
            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            row->Add(u, 0, wxALIGN_CENTER_VERTICAL);
            main->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
            // Arrow clicks and focus-out commits apply here. Enter is handled
            // at the dialog level (see the wxEVT_CHAR_HOOK below) because
            // wxSpinCtrlDouble's own Enter path commits its value AFTER emitting
            // its events, so an event-driven apply would read the stale value.
            ctrl->Bind(wxEVT_SPINCTRLDOUBLE, &InsertEditDialog::OnField, this);
        };

        addHeader("Position");
        addSpin("X:", m_px, -100000.0, 100000.0, 0.5, 3, "mm");
        addSpin("Y:", m_py, -100000.0, 100000.0, 0.5, 3, "mm");
        addSpin("Z:", m_pz, -100000.0, 100000.0, 0.5, 3, "mm");

        main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        addHeader("Rotation");
        addSpin("X:", m_rx, -36000.0, 36000.0, 1.0, 2, "\u00B0");
        addSpin("Y:", m_ry, -36000.0, 36000.0, 1.0, 2, "\u00B0");
        addSpin("Z:", m_rz, -36000.0, 36000.0, 1.0, 2, "\u00B0");

        main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        addHeader("Scale");
        addSpin("Uniform:", m_scale, 0.001, 1000.0, 0.05, 3, "\u00D7");

        // Actions: Reset returns to the placement default (origin-aligned, true
        // size); Close dismisses. The native caption "X" also closes.
        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
        auto* reset = new wxButton(this, wxID_ANY, "Reset");
        auto* close = new wxButton(this, wxID_CLOSE, "Close");
        reset->Bind(wxEVT_BUTTON, &InsertEditDialog::OnReset, this);
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
        btnRow->AddStretchSpacer(1);
        btnRow->Add(reset, 0, wxRIGHT, 6);
        btnRow->Add(close, 0);
        main->Add(btnRow, 0, wxEXPAND | wxALL, 12);

        SetSizerAndFit(main);
        CentreOnParent();
        WindowEffects::ApplyRoundedCorners(this);

        // Closing (caption X or Close button) tells the frame to drop its
        // pointer, then destroys the window. Destroy(), not delete — wxWidgets
        // owns the lifetime.
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&)
        {
            if (m_frame) m_frame->OnInsertEditorClosed();
            Destroy();
        });

        // Route ESC through Close() too. wxDialog's default ESC handling on a
        // MODELESS dialog can merely hide it, which would leave the frame's
        // pointer dangling-but-non-null; Close() fires the handler above so the
        // frame drops its pointer and the window is destroyed.
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e)
        {
            const int k = e.GetKeyCode();
            if (k == WXK_ESCAPE) { Close(); return; }
            if (k == WXK_RETURN || k == WXK_NUMPAD_ENTER)
            {
                // Commit the in-progress edit, then apply. Consume the key so it
                // doesn't go hunting for a default dialog button.
                CommitFocusedField();
                Apply();
                return;
            }
            e.Skip();
        });

        Populate();
    }

    int TargetId() const { return m_insertId; }

    // Retarget an already-open dialog at a different insert (user clicked
    // another insert while editing). Repopulate from that insert's transform.
    void SetTarget(int insertId)
    {
        m_insertId = insertId;
        Populate();
    }

    // True while the target insert still exists; the frame uses this to decide
    // between refreshing and closing after a structural change.
    bool TargetAlive() const
    {
        return m_canvas && m_canvas->InsertIndexFromId(m_insertId) >= 0;
    }

    // Re-read the target's transform into the fields (e.g. after an external
    // change). Public so the frame can refresh without a full retarget.
    void RefreshFromCanvas() { Populate(); }

private:
    // Load the target insert's transform into the spinners. Guarded by
    // m_populating so the SetValue calls don't re-enter OnField and echo the
    // values straight back to the canvas.
    void Populate()
    {
        if (!m_canvas) return;
        glm::vec3 off(0.0f), rot(0.0f);
        float scale = 1.0f;
        if (!m_canvas->GetInsertTransformById(m_insertId, off, rot, scale))
            return;   // target gone — leave fields; the frame will close us

        m_populating = true;
        m_px->SetValue(off.x); m_py->SetValue(off.y); m_pz->SetValue(off.z);
        m_rx->SetValue(rot.x); m_ry->SetValue(rot.y); m_rz->SetValue(rot.z);
        m_scale->SetValue(scale);
        m_populating = false;
    }

    // Push the current field values to the canvas. If the target is gone the
    // set fails and we close.
    void Apply()
    {
        if (m_populating || !m_canvas) return;
        const glm::vec3 off((float)m_px->GetValue(),
            (float)m_py->GetValue(),
            (float)m_pz->GetValue());
        const glm::vec3 rot((float)m_rx->GetValue(),
            (float)m_ry->GetValue(),
            (float)m_rz->GetValue());
        const float scale = (float)m_scale->GetValue();
        if (!m_canvas->SetInsertTransformById(m_insertId, off, rot, scale))
            Close();
    }

    void OnField(wxSpinDoubleEvent&) { Apply(); }

    // Force the focused field to adopt its typed-but-uncommitted text before an
    // Enter-driven Apply reads it. wxSpinCtrlDouble is the generic composite on
    // every platform: the focused widget is its inner wxTextCtrl, whose parent
    // is the spin control. Parsing the shown text and SetValue()-ing it makes
    // GetValue() fresh (and clamps to range). Unfocused fields already committed
    // on focus-loss, so only the focused one needs this.
    void CommitFocusedField()
    {
        wxWindow* foc = FindFocus();
        auto* txt = dynamic_cast<wxTextCtrl*>(foc);
        if (!txt) return;
        auto* spin = dynamic_cast<wxSpinCtrlDouble*>(txt->GetParent());
        if (!spin) return;
        double v = 0.0;
        if (txt->GetValue().ToDouble(&v))
            spin->SetValue(v);
    }

    void OnReset(wxCommandEvent&)
    {
        m_populating = true;
        m_px->SetValue(0.0); m_py->SetValue(0.0); m_pz->SetValue(0.0);
        m_rx->SetValue(0.0); m_ry->SetValue(0.0); m_rz->SetValue(0.0);
        m_scale->SetValue(1.0);
        m_populating = false;
        Apply();
    }

    MainFrame* m_frame = nullptr;
    GLCanvas* m_canvas = nullptr;
    int        m_insertId = -1;
    bool       m_populating = false;

    wxSpinCtrlDouble* m_px = nullptr; wxSpinCtrlDouble* m_py = nullptr; wxSpinCtrlDouble* m_pz = nullptr;
    wxSpinCtrlDouble* m_rx = nullptr; wxSpinCtrlDouble* m_ry = nullptr; wxSpinCtrlDouble* m_rz = nullptr;
    wxSpinCtrlDouble* m_scale = nullptr;
};

// OpenInsertEditor — create the modeless editor, or retarget/raise the existing
// one, on `insertId`. Called by the canvas when an insert is picked in
// EditInsert mode.
void MainFrame::OpenInsertEditor(int insertId)
{
    if (!m_canvas) return;
    if (m_insertEditDialog)
    {
        m_insertEditDialog->SetTarget(insertId);
        m_insertEditDialog->Raise();
        return;
    }
    m_insertEditDialog = new InsertEditDialog(this, m_canvas, insertId);
    m_insertEditDialog->Show();
}

// ValidateInsertEditor — after a structural insert change, close the editor if
// its target is gone, otherwise refresh its fields (the transform itself is
// unchanged by a reanchor, but a refresh is cheap and keeps it honest).
void MainFrame::ValidateInsertEditor()
{
    if (!m_insertEditDialog) return;
    if (!m_insertEditDialog->TargetAlive())
        m_insertEditDialog->Close();
    else
        m_insertEditDialog->RefreshFromCanvas();
}

// OnInsertEditorClosed — the dialog is closing; drop our pointer so we don't
// touch a destroyed window. The dialog Destroy()s itself right after this.
void MainFrame::OnInsertEditorClosed()
{
    m_insertEditDialog = nullptr;
}

// DestroyInsertEditor — tear the editor down from ~MainFrame. Null first so the
// window's close-notify (OnInsertEditorClosed) is a no-op if it races.
void MainFrame::DestroyInsertEditor()
{
    if (!m_insertEditDialog) return;
    InsertEditDialog* dlg = m_insertEditDialog;
    m_insertEditDialog = nullptr;
    dlg->Destroy();
}

// ---------------------------------------------------------------------------
// Insert handlers.
//
// An insert is an imported body whose pose is driven entirely by a parent
// object, so placement is a two-part act: pick the parent, then pick the file.
// OnPlaceInsert resolves the parent:
//
//   exactly one object selected -> that's the parent; go straight to the file
//                                  dialog, no mode change, no extra click.
//   zero or 2+ selected         -> toggle into PlaceInsert and let the canvas
//                                  collect the parent pick. The canvas calls
//                                  PlaceInsertOnParent below once it has one,
//                                  which runs the same file dialog.
//
// Both routes converge on PlaceInsertOnParent, so the flow is identical
// regardless of how the parent was chosen. A 2+ selection deliberately falls
// through to the pick mode rather than guessing which member to parent to.
// ---------------------------------------------------------------------------
void MainFrame::OnPlaceInsert(wxCommandEvent&)
{
    if (!m_canvas) return;

    // Toggle back out if we're already collecting a parent pick.
    if (m_canvas->GetTransformMode() == TransformMode::PlaceInsert)
    {
        SetActiveTool(TransformMode::Select);
        return;
    }

    const int sel = m_canvas->GetSingleSelectedObject();
    if (sel >= 0)
    {
        PlaceInsertOnParent(sel);
        return;
    }

    SetActiveTool(TransformMode::PlaceInsert);
}

void MainFrame::PlaceInsertOnParent(int parentIdx)
{
    if (!m_canvas) return;

    wxFileDialog dlg(
        this, "Import Insert", "", "",
        "All supported (*.step;*.stp;*.stl;*.obj)|*.step;*.stp;*.stl;*.obj|"
        "STEP files (*.step;*.stp)|*.step;*.stp|"
        "STL files (*.stl)|*.stl|"
        "OBJ files (*.obj)|*.obj|"
        "All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    // Cancelling the file dialog leaves the parent pick spent but places
    // nothing — drop back to Select rather than silently staying armed, so
    // the toggle button state always matches what the next click will do.
    if (dlg.ShowModal() != wxID_OK)
    {
        SetActiveTool(TransformMode::Select);
        return;
    }

    const bool placed =
        m_canvas->PlaceInsertOnObject(parentIdx, dlg.GetPath().ToStdString());

    SetActiveTool(TransformMode::Select);

    // Same Clean->Dirty reasoning as OnImport: an insert adds geometry to the
    // scene without invalidating an existing mould outright. (Inserts don't
    // participate in the cut yet, so this is conservative rather than
    // strictly necessary — it costs one warning on Export and stops being
    // conservative the moment cut integration lands.)
    if (placed && m_mouldState == MouldState::Clean)
        m_mouldState = MouldState::Dirty;
}

void MainFrame::OnClearInserts(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearInserts();
}

void MainFrame::OnRemoveInsert(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveInsert)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveInsert);
}

// Edit Insert — deliberately inert for now. The button ships so the Inserts
// card matches every other feature card; the mode toggles (and the canvas
// gives it a hand cursor) but nothing is authored yet. Offset / rotation
// authoring against the parent lands behind this.
void MainFrame::OnEditInsert(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::EditInsert)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::EditInsert);
}

void MainFrame::OnEditSprue(wxCommandEvent&)
{
    if (!m_canvas) return;

    // Enter the Edit Sprue environment (floating toolbar with Move + Select
    // Injection Point). Worth opening only when there's something to edit: a
    // placed sprue to move, or a choice of injection points to switch between.
    // Toggle back out if already in it.
    if (m_canvas->GetTransformMode() == TransformMode::EditSprue)
    {
        SetActiveTool(TransformMode::Select);
        return;
    }

    if (!m_canvas->HasSprueForEdit() && !m_canvas->HasInjectionChoices() &&
        !m_canvas->AllowsPerimeterInjection())
        return;   // nothing to edit

    SetActiveTool(TransformMode::EditSprue);
}

// ---------------------------------------------------------------------------
// Called by GLCanvas when the user actively picks an injection point via the
// SelectInjectionPoint tool. Currently only adjusts the Draft Angle field:
// radial injection points zero it (the radial sprue is intended to lie flat
// on the parting plane with no draft); axial injection points get the
// default value back.
//
// Scope is deliberately limited to this user-driven gesture. Programmatic
// SetActiveInjectionPoint calls (project load, fixture change) deliberately
// do NOT trigger this — project load already restored the user's saved draft
// angle, and clobbering it based on the active injection-point type would
// silently override their stored setting.
// ---------------------------------------------------------------------------
void MainFrame::OnInjectionPointSelected(const InjectionPoint& ip)
{
    if (!m_sprueDraftAngle) return;

    // "1.0" mirrors the default text used when this control is constructed
    // (see the addRow("Draft angle:", m_sprueDraftAngle, "1.0", ...) call
    // in the sprue panel builder) and the ProjectParameters default. If
    // that default ever changes, both locations need to stay in sync.
    const wxString defaultDraft = "1.0";
    const wxString radialDraft = "0.0";

    m_sprueDraftAngle->SetValue(
        ip.type == InjectionType::Radial ? radialDraft : defaultDraft);
}

// ---------------------------------------------------------------------------
// Unit system toggle
// ---------------------------------------------------------------------------
void MainFrame::OnSetMetric(wxCommandEvent&)
{
    if (!m_imperial) return;   // already metric

    // Convert all mm-based field values: displayed inches → mm
    wxTextCtrl* mmFields[] = {
        m_ventLength, m_ventWidth, m_ventOverrunStart, m_ventOverrunEnd,
        m_sprueDiameter, m_sprueColdSlugDepth, m_sprueLength, m_sprueOverrun,
        m_runnerDiameter, m_runnerColdSlugDepth,
        m_gateDiameter, m_subRunnerDiameter
    };
    for (auto* ctrl : mmFields)
    {
        if (!ctrl) continue;
        double v = 0.0;
        if (ctrl->GetValue().ToDouble(&v))
            ctrl->SetValue(wxString::Format("%.4g", v * 25.4));
    }

    // Update labels
    for (auto* lbl : m_mmUnitLabels)
        lbl->SetLabel("mm");

    m_imperial = false;
}

void MainFrame::OnSetImperial(wxCommandEvent&)
{
    if (m_imperial) return;   // already imperial

    // Convert all mm-based field values: displayed mm → inches
    wxTextCtrl* mmFields[] = {
        m_ventLength, m_ventWidth, m_ventOverrunStart, m_ventOverrunEnd,
        m_sprueDiameter, m_sprueColdSlugDepth, m_sprueLength, m_sprueOverrun,
        m_runnerDiameter, m_runnerColdSlugDepth,
        m_gateDiameter, m_subRunnerDiameter
    };
    for (auto* ctrl : mmFields)
    {
        if (!ctrl) continue;
        double v = 0.0;
        if (ctrl->GetValue().ToDouble(&v))
            ctrl->SetValue(wxString::Format("%.4g", v / 25.4));
    }

    // Update labels
    for (auto* lbl : m_mmUnitLabels)
        lbl->SetLabel("in");

    m_imperial = true;
}

// ---------------------------------------------------------------------------
// Grid menu handler — the consolidated Grid Settings dialog. Authors the full
// GridSettings (shape / size / spacing / major divisions) and pushes it to the
// live grid so the rendered grid updates immediately.
// ---------------------------------------------------------------------------
void MainFrame::OnGridSettings(wxCommandEvent&)
{
    GridSettingsDialog dlg(this, m_gridSettings, m_imperial);
    if (dlg.ShowModal() != wxID_OK)
        return;

    m_gridSettings = dlg.GetSettings();

    if (m_canvas)
        m_canvas->SetGridSettings(m_gridSettings);
}

// ---------------------------------------------------------------------------
// OnAbout — modal About box. Stack-allocated: it owns no state worth
// keeping between invocations, and the modal loop keeps it alive.
// ---------------------------------------------------------------------------
void MainFrame::OnAbout(wxCommandEvent&)
{
    AboutDialog dlg(this);
    dlg.ShowModal();
}

// ---------------------------------------------------------------------------
// OnCheckForUpdates — the manual, user-initiated check (Tier 1: notify
// only). The dialog owns the whole flow — request, timeout, result UI —
// and cancels the request if closed mid-check.
// ---------------------------------------------------------------------------
void MainFrame::OnCheckForUpdates(wxCommandEvent&)
{
    UpdateCheckDialog dlg(this);
    dlg.ShowModal();
}

void MainFrame::GetVentDimensions(float& outLength, float& outWidth,
    float& outOverrunStart, float& outOverrunEnd) const
{
    // Safe parse helper — returns defaultVal if text is empty or non-numeric
    auto parseField = [](wxTextCtrl* ctrl, float defaultVal) -> float
    {
        if (!ctrl) return defaultVal;
        double v = defaultVal;
        if (!ctrl->GetValue().ToDouble(&v)) return defaultVal;
        return (v > 0.0) ? static_cast<float>(v) : defaultVal;
    };

    const float conv = m_imperial ? 25.4f : 1.0f;
    outLength = parseField(m_ventLength, 5.0f) * conv;
    outWidth = parseField(m_ventWidth, 2.0f) * conv;
    outOverrunStart = parseField(m_ventOverrunStart, 0.5f) * conv;
    outOverrunEnd = parseField(m_ventOverrunEnd, 0.5f) * conv;
}

float MainFrame::GetSprueDiameter() const
{
    if (!m_sprueDiameter) return 5.0f;
    double v = 5.0;
    if (!m_sprueDiameter->GetValue().ToDouble(&v)) return 5.0f;
    if (v <= 0.0) v = 5.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

float MainFrame::GetSprueDraftAngle() const
{
    if (!m_sprueDraftAngle) return 1.0f;
    double v = 1.0;
    if (!m_sprueDraftAngle->GetValue().ToDouble(&v)) return 1.0f;
    if (v < 0.0) v = 0.0;
    if (v > 45.0) v = 45.0;
    return static_cast<float>(v);   // degrees — no unit conversion
}

float MainFrame::GetSprueColdSlugDepth() const
{
    if (!m_sprueColdSlugDepth) return 5.0f;
    double v = 5.0;
    if (!m_sprueColdSlugDepth->GetValue().ToDouble(&v)) return 5.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

float MainFrame::GetSprueLength() const
{
    if (!m_sprueLength) return 20.0f;
    double v = 20.0;
    if (!m_sprueLength->GetValue().ToDouble(&v)) return 20.0f;
    if (v <= 0.0) v = 20.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

float MainFrame::GetSprueOverrun() const
{
    // Direct-injection gate overrun (mm). Defaults to 0 (no overrun); negatives
    // clamp to 0. Read and converted like the other length fields.
    if (!m_sprueOverrun) return 0.0f;
    double v = 0.0;
    if (!m_sprueOverrun->GetValue().ToDouble(&v)) return 0.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

float MainFrame::GetRunnerDiameter() const
{
    if (!m_runnerDiameter) return 5.0f;
    double v = 5.0;
    if (!m_runnerDiameter->GetValue().ToDouble(&v)) return 5.0f;
    if (v <= 0.0) v = 5.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

float MainFrame::GetRunnerColdPlugDist() const
{
    if (!m_runnerColdSlugDepth) return 5.0f;
    double v = 5.0;
    if (!m_runnerColdSlugDepth->GetValue().ToDouble(&v)) return 5.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

float MainFrame::GetGateDiameter() const
{
    if (!m_gateDiameter) return 3.0f;
    double v = 3.0;
    if (!m_gateDiameter->GetValue().ToDouble(&v)) return 3.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

float MainFrame::GetGateDraftAngle() const
{
    if (!m_gateDraftAngle) return 1.0f;
    double v = 1.0;
    if (!m_gateDraftAngle->GetValue().ToDouble(&v)) return 1.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v);   // degrees — no unit conversion
}

// Distance the gate cylinder is extended backward along -pathDir into the
// model body. Returns mm regardless of the active unit system, matching the
// other length accessors. Default 0 means the gate starts exactly at the
// parting-surface placement point (legacy behaviour). Negative inputs are
// clamped to 0 — a "shorter than the surface" gate doesn't make sense for
// the cut-clearance use case.
float MainFrame::GetGateOverrun() const
{
    if (!m_gateOverrun) return 0.0f;
    double v = 0.0;
    if (!m_gateOverrun->GetValue().ToDouble(&v)) return 0.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

float MainFrame::GetSubRunnerDiameter() const
{
    if (!m_subRunnerDiameter) return 5.0f;
    double v = 5.0;
    if (!m_subRunnerDiameter->GetValue().ToDouble(&v)) return 5.0f;
    if (v <= 0.0) v = 5.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

// Ejector dimension accessors. Returns mm regardless of the active unit
// system, matching the convention used by the other Get*Dimension getters.
// Defaults are returned when the field hasn't been built yet (early calls)
// or contains an unparseable value, so callers never get NaN.
float MainFrame::GetEjectorDiameter() const
{
    if (!m_ejectorDiameter) return 3.0f;
    double v = 3.0;
    if (!m_ejectorDiameter->GetValue().ToDouble(&v)) return 3.0f;
    if (v <= 0.0) v = 3.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

float MainFrame::GetEjectorLength() const
{
    if (!m_ejectorLength) return 25.0f;
    double v = 25.0;
    if (!m_ejectorLength->GetValue().ToDouble(&v)) return 25.0f;
    if (v <= 0.0) v = 25.0;
    return static_cast<float>(v) * (m_imperial ? 25.4f : 1.0f);
}

// Insert "Cut scale": the card takes a percentage, callers want a multiplier,
// so 100 -> 1.0. No unit conversion — a percentage is unitless, which is why
// its "%" label is not registered in m_mmUnitLabels and doesn't flip with the
// metric/imperial switch. Non-positive or unparseable input falls back to
// 100% (nominal fit) rather than collapsing the body to nothing.
float MainFrame::GetInsertCutScale() const
{
    if (!m_insertCutScale) return 1.0f;
    double v = 100.0;
    if (!m_insertCutScale->GetValue().ToDouble(&v)) return 1.0f;
    if (v <= 0.0) v = 100.0;
    return static_cast<float>(v * 0.01);
}

// ---------------------------------------------------------------------------
// Menu handlers
// ---------------------------------------------------------------------------
void MainFrame::OnExit(wxCommandEvent&)
{
    Close(true);
}

void MainFrame::OnImport(wxCommandEvent&)
{
    wxFileDialog dlg(
        this, "Import Model", "", "",
        "All supported (*.step;*.stp;*.stl;*.obj)|*.step;*.stp;*.stl;*.obj|"
        "STEP files (*.step;*.stp)|*.step;*.stp|"
        "STL files (*.stl)|*.stl|"
        "OBJ files (*.obj)|*.obj|"
        "All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (dlg.ShowModal() != wxID_OK)
        return;

    m_canvas->ImportFile(dlg.GetPath().ToStdString());

    // Treat as a scene mutation, not a project reset: importing adds an
    // object without wiping any previously-generated mould geometry, so
    // the prior generation is stale (Dirty) but not gone (NeverGenerated).
    // ImportFile may not route through the canvas's NotifySceneMutated
    // hooks, so apply the same Clean→Dirty transition here that the
    // canvas callback would have applied. (If the user hasn't generated
    // yet, this is a no-op and state stays NeverGenerated.)
    if (m_mouldState == MouldState::Clean)
        m_mouldState = MouldState::Dirty;
}

// ---------------------------------------------------------------------------
// Fixture menu handlers
// ---------------------------------------------------------------------------
// Create Fixture pops the editor for authoring a new .fixture from scratch.
// The editor is non-modal (floating wxFrame), so this returns immediately
// and the user works in parallel with the main app. Once the editor reports
// a saved fixture back, this handler will likely also reload the new
// fixture into the canvas — same shape as OnChangeFixture below — but
// while the editor is scaffolding-only it just opens the window.
void MainFrame::OnCreateFixture(wxCommandEvent&)
{
    // Same two-step flow as StartupDialog::OnNewFixture — see that comment
    // for the rationale and the editor-lifecycle reasoning.
    CreateFixtureDialog createDlg(this);
    FixtureEditor* editor = new FixtureEditor(this);

    createDlg.SetLoadHandler(
        [editor, &createDlg](CreateFixtureDialog::ProgressFn progress)
    {
        editor->SetInitialFixture(createDlg.GetFixtureName(),
            createDlg.GetModelAPath(),
            createDlg.GetModelBPath(),
            progress);
    });

    if (createDlg.ShowModal() != wxID_OK)
    {
        editor->Destroy();
        return;
    }

    editor->Show();
}

void MainFrame::LoadFixtureIntoScene(const FixtureDefinition& fixture)
{
    if (fixture.kind == FixtureKind::Library)
    {
        if (!fixture.modelAPath.empty())
            m_canvas->ImportFileAsFixture(fixture.modelAPath, fixture.halfATransform);
        if (!fixture.modelBPath.empty())
            m_canvas->ImportFileAsFixture(fixture.modelBPath, fixture.halfBTransform);
    }
    else
    {
        // Parametric / Dynamic: build the two box halves procedurally. This
        // clears and repopulates m_fixtures internally, so callers that already
        // ClearFixtures()/ClearAll() beforehand stay correct either way.
        m_canvas->CreateProceduralFixture(fixture);
    }
}

void MainFrame::OnChangeFixture(wxCommandEvent&)
{
    const std::string lastFixture = AppConfig::LoadLastFixture();

    StartupDialog dlg(this);
    dlg.PreSelectFixture(lastFixture);

    if (dlg.ShowModal() != wxID_OK)
        return;

    FixtureDefinition fixture = dlg.GetFixture();
    // Procedural fixtures have no path — don't overwrite the remembered library
    // fixture with an empty string. Project save persists procedural fixtures.
    if (!fixture.fixturePath.empty())
        AppConfig::SaveLastFixture(fixture.fixturePath);
    m_fixtureDef = fixture;   // keep for project save

    // Clear existing fixtures and reload
    m_canvas->ClearFixtures();

    LoadFixtureIntoScene(fixture);

    m_canvas->SetInjectionPoints(fixture.injectionPoints);
    m_canvas->SetAllowPerimeterInjection(fixture.allowPerimeterInjection);
    if (!fixture.injectionPoints.empty())
        m_canvas->SetActiveInjectionPoint(fixture.injectionPoints[0]);

    // Reset side-panel fields to the new fixture's per-feature defaults.
    ApplyFixtureDefaults(fixture);

    // Fixture swap = scene effectively reset. Any previously-generated
    // mould was built against the old fixture(s) and is meaningless now —
    // back to "nothing to export until you Generate".
    m_mouldState = MouldState::NeverGenerated;
}

void MainFrame::OnEditFixture(wxCommandEvent&)
{
    if (!m_canvas) return;

    // Editable dimensions only exist for procedural fixtures. The menu item is
    // disabled for a library fixture (OnUpdateEditFixture), but guard anyway.
    if (m_fixtureDef.kind == FixtureKind::Library)
        return;

    // Re-open the same dialog used at creation, seeded with the current values.
    ProceduralFixtureDialog dlg(this, m_fixtureDef);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (m_fixtureDef.kind == FixtureKind::Parametric)
        m_fixtureDef.parametric = dlg.GetParametric();
    else
        m_fixtureDef.dynamic = dlg.GetDynamic();

    // Rebuild the fixture at the new size. LoadFixtureIntoScene ->
    // CreateProceduralFixture clears the old fixture (and its vents) and seeds a
    // fresh perimeter injection point, so the sprue resets to match the new
    // dimensions. A Dynamic fixture re-fits the current scene with the edited
    // clearances.
    LoadFixtureIntoScene(m_fixtureDef);
    m_canvas->SetInjectionPoints(m_fixtureDef.injectionPoints);
    m_canvas->SetAllowPerimeterInjection(m_fixtureDef.allowPerimeterInjection);

    // The blank geometry changed, so any previously-generated mould is stale.
    m_mouldState = MouldState::NeverGenerated;
}

void MainFrame::OnUpdateEditFixture(wxUpdateUIEvent& evt)
{
    // Grey the item out unless a procedural fixture (with dimensions) is active.
    evt.Enable(m_fixtureDef.kind != FixtureKind::Library);
}

// ---------------------------------------------------------------------------
// First-launch fixture prompt
// ---------------------------------------------------------------------------
// Called by the app after the main frame is shown. If the user already had a
// fixture on disk, the constructor has loaded it and this is a no-op. If not,
// we present the fixture picker on top of the now-visible main window so the
// user gets the full app chrome as context instead of a modal-over-nothing.
void MainFrame::PromptForFixtureIfMissing()
{
    if (m_fixtureDef.IsValid())
        return;

    StartupDialog dlg(this);
    dlg.PreSelectFixture(AppConfig::LoadLastFixture());

    if (dlg.ShowModal() != wxID_OK)
        return;  // user cancelled — leave the app open with no fixture loaded

    FixtureDefinition fixture = dlg.GetFixture();
    if (!fixture.fixturePath.empty())   // keep last library fixture; procedural has no path
        AppConfig::SaveLastFixture(fixture.fixturePath);
    m_fixtureDef = fixture;

    // Fresh frame, so no need to ClearFixtures() — just load.
    LoadFixtureIntoScene(fixture);

    m_canvas->SetInjectionPoints(fixture.injectionPoints);
    m_canvas->SetAllowPerimeterInjection(fixture.allowPerimeterInjection);
    if (!fixture.injectionPoints.empty())
        m_canvas->SetActiveInjectionPoint(fixture.injectionPoints[0]);

    ApplyFixtureDefaults(fixture);
}

// ---------------------------------------------------------------------------
// New Project
// ---------------------------------------------------------------------------
void MainFrame::OnNewProject(wxCommandEvent&)
{
    const std::string lastFixture = AppConfig::LoadLastFixture();

    FixtureDefinition fixture;
    std::string error;

    // If a valid default fixture exists, use it directly (same as startup)
    if (!lastFixture.empty() && FixtureFile::Load(lastFixture, fixture, error))
    {
        // Use the default fixture without showing the dialog
    }
    else
    {
        // No valid default — show the selection dialog
        StartupDialog dlg(this);
        dlg.PreSelectFixture(lastFixture);

        if (dlg.ShowModal() != wxID_OK)
            return;

        fixture = dlg.GetFixture();
        if (!fixture.fixturePath.empty())   // keep last library fixture; procedural has no path
            AppConfig::SaveLastFixture(fixture.fixturePath);
    }

    // Clear the entire scene
    m_canvas->ClearAll();

    // Load the selected fixture
    m_fixtureDef = fixture;

    LoadFixtureIntoScene(fixture);

    m_canvas->SetInjectionPoints(fixture.injectionPoints);
    m_canvas->SetAllowPerimeterInjection(fixture.allowPerimeterInjection);
    if (!fixture.injectionPoints.empty())
        m_canvas->SetActiveInjectionPoint(fixture.injectionPoints[0]);

    ApplyFixtureDefaults(fixture);

    // Reset project state
    m_projectPath.clear();
    SetTitle("Mould3r - New Project");

    // Fresh project = nothing has been generated yet. Even though
    // ClearAll above ran some canvas mutation paths that may have
    // fired NotifySceneMutated already, lock in the canonical reset
    // state here so a Generate run is genuinely required before
    // Export will quietly succeed.
    m_mouldState = MouldState::NeverGenerated;
}

// ---------------------------------------------------------------------------
// Save Project
// ---------------------------------------------------------------------------
void MainFrame::OnSaveProject(wxCommandEvent&)
{
    wxFileDialog dlg(
        this, "Save Project", "", "",
        "Mould3r Project (*.m3d)|*.m3d|All files (*.*)|*.*",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (!m_projectPath.empty())
    {
        wxFileName fn(m_projectPath);
        dlg.SetDirectory(fn.GetPath());
        dlg.SetFilename(fn.GetFullName());
    }

    if (dlg.ShowModal() != wxID_OK)
        return;

    const std::string savePath = dlg.GetPath().ToStdString();

    // Build ProjectData from current state
    ProjectData data;
    data.version = 1;
    data.fixturePath = m_fixtureDef.fixturePath;
    // Procedural fixtures persist as kind + params instead of a path; Save()
    // writes whichever applies. Library fixtures leave these at their defaults.
    data.fixtureKind = m_fixtureDef.kind;
    data.fixtureParametric = m_fixtureDef.parametric;
    data.fixtureDynamic = m_fixtureDef.dynamic;

    // Objects
    for (const auto& obj : m_canvas->GetObjects())
    {
        ProjectObjectData od;
        od.sourcePath = obj.sourcePath;
        od.pos = obj.pos;
        od.yawDeg = obj.yawDeg;
        od.pitchDeg = obj.pitchDeg;
        od.rollDeg = obj.rollDeg;
        od.scale = obj.scale;
        od.mirrorX = obj.mirrorX;
        od.mirrorZ = obj.mirrorZ;
        data.objects.push_back(od);
    }

    // Parameters (read from UI fields)
    {
        auto& p = data.params;
        float ventLength, ventWidth, ventOverrunStart, ventOverrunEnd;
        GetVentDimensions(ventLength, ventWidth, ventOverrunStart, ventOverrunEnd);
        p.ventWidth = ventWidth;
        p.ventLength = ventLength;
        p.ventOverrunStart = ventOverrunStart;
        p.ventOverrunEnd = ventOverrunEnd;
        p.sprueDiameter = GetSprueDiameter();
        p.sprueDraftAngle = GetSprueDraftAngle();
        p.sprueColdSlugDepth = GetSprueColdSlugDepth();
        p.sprueLength = GetSprueLength();
        p.sprueOverrun = GetSprueOverrun();
        p.runnerDiameter = GetRunnerDiameter();
        p.runnerColdPlugDist = GetRunnerColdPlugDist();
        p.gateDiameter = GetGateDiameter();
        p.gateDraftAngle = GetGateDraftAngle();
        p.gateOverrun = GetGateOverrun();
        p.subRunnerDiameter = GetSubRunnerDiameter();
        p.ejectorDiameter = GetEjectorDiameter();
        p.ejectorLength = GetEjectorLength();
    }

    // Sprue
    {
        const auto& sp = m_canvas->GetSprue();
        auto& sd = data.sprue;
        sd.placed = sp.hasPoint;
        sd.worldPos = sp.worldPos;
        sd.pathStart = sp.pathStart;
        sd.pathEnd = sp.pathEnd;
        sd.partingPos = sp.partingPos;
        sd.hasPartingPoint = sp.hasPartingPoint;
        sd.isDirectInjection = sp.isDirectInjection;
        sd.radius = sp.radius;
        sd.draftAngleDeg = sp.draftAngleDeg;
        sd.coldSlugDepth = sp.coldSlugDepth;

        if (m_canvas->HasActiveInjectionPoint())
            sd.injectionPoint = m_canvas->GetActiveInjectionPoint();
    }

    // Runners
    for (const auto& rf : m_canvas->GetRunners())
    {
        ProjectRunnerData pr;
        pr.point = rf.point;

        // Persist an authored complex path verbatim; simple paths re-derive.
        if (rf.path.kind == PathKind::Complex)
        {
            pr.isComplex = true;
            pr.smooth = rf.path.smooth;
            pr.nodes.reserve(rf.path.nodes.size());
            for (const auto& n : rf.path.nodes)
            {
                ProjectPathNode pn;
                pn.pos = n.pos;
                pn.dir = n.dir;
                pn.handleLen = n.handleLen;
                pn.handleIn = n.handleIn;
                pn.handleOut = n.handleOut;
                pn.handlesLinked = n.handlesLinked;
                pn.handlesManual = n.handlesManual;
                pr.nodes.push_back(pn);
            }
        }

        data.runners.push_back(pr);
    }

    // Gates
    for (const auto& gf : m_canvas->GetGates())
    {
        ProjectGateData pg;
        pg.pos = gf.point.worldPos;
        pg.normal = gf.point.worldNormal;
        pg.parentIndex = gf.parentIndex;
        pg.localPos = gf.localPos;
        pg.localNormal = gf.localNormal;

        // Persist an authored complex sub-runner verbatim; simple sub-runners
        // re-derive on load. The gate frustum is never persisted.
        if (gf.subPath.kind == PathKind::Complex)
        {
            pg.isComplex = true;
            pg.smooth = gf.subPath.smooth;
            pg.nodes.reserve(gf.subPath.nodes.size());
            for (const auto& n : gf.subPath.nodes)
            {
                ProjectPathNode pn;
                pn.pos = n.pos;
                pn.dir = n.dir;
                pn.handleLen = n.handleLen;
                pn.handleIn = n.handleIn;
                pn.handleOut = n.handleOut;
                pn.handlesLinked = n.handlesLinked;
                pn.handlesManual = n.handlesManual;
                pg.nodes.push_back(pn);
            }
        }

        data.gates.push_back(pg);
    }

    // Vents
    for (const auto& vi : m_canvas->GetVents())
    {
        ProjectVentData pv;
        pv.pos = vi.point.worldPos;
        pv.normal = vi.point.worldNormal;
        pv.parentIndex = vi.parentIndex;
        pv.localPos = vi.localPos;
        pv.localNormal = vi.localNormal;

        // Persist an authored complex path verbatim; simple paths re-derive.
        if (vi.path.kind == PathKind::Complex)
        {
            pv.isComplex = true;
            pv.smooth = vi.path.smooth;
            pv.nodes.reserve(vi.path.nodes.size());
            for (const auto& n : vi.path.nodes)
            {
                ProjectPathNode pn;
                pn.pos = n.pos;
                pn.dir = n.dir;
                pn.handleLen = n.handleLen;
                pn.handleIn = n.handleIn;
                pn.handleOut = n.handleOut;
                pn.handlesLinked = n.handlesLinked;
                pn.handlesManual = n.handlesManual;
                pv.nodes.push_back(pn);
            }
        }

        data.vents.push_back(pv);
    }

    // Ejectors — just the world-space placement point. Sticky-placement
    // (parent tracking) isn't wired up for ejectors yet; if it's added
    // later the corresponding fields will be populated on ProjectEjectorData.
    for (const auto& ef : m_canvas->GetEjectors())
    {
        ProjectEjectorData pe;
        pe.point = ef.point;
        data.ejectors.push_back(pe);
    }

    // Inserts — source body path + parent index + local transform. Cut scale is
    // a global card value, not per-insert, so it isn't stored here.
    for (const auto& in : m_canvas->GetInserts())
    {
        ProjectInsertData pi;
        pi.sourcePath = in.body.sourcePath;
        pi.parentIndex = in.parentIndex;
        pi.localOffset = in.localOffset;
        pi.localRotDeg = in.localRotDeg;
        pi.localScale = in.localScale;
        data.inserts.push_back(pi);
    }

    std::string error;
    if (!ProjectFile::Save(savePath, data, error))
    {
        wxMessageBox(error, "Save Failed", wxOK | wxICON_ERROR, this);
        return;
    }

    m_projectPath = savePath;
    SetTitle("Mould3r - " + wxFileName(savePath).GetName());
}

// ---------------------------------------------------------------------------
// Load Project
// ---------------------------------------------------------------------------
void MainFrame::OnLoadProject(wxCommandEvent&)
{
    wxFileDialog dlg(
        this, "Open Project", "", "",
        "Mould3r Project (*.m3d)|*.m3d|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (dlg.ShowModal() != wxID_OK)
        return;

    const std::string loadPath = dlg.GetPath().ToStdString();

    ProjectData data;
    std::string error;
    if (!ProjectFile::Load(loadPath, data, error))
    {
        wxMessageBox(error, "Load Failed", wxOK | wxICON_ERROR, this);
        return;
    }

    // ---- Clear everything --------------------------------------------------
    m_canvas->ClearAll();

    // ---- Load fixture ------------------------------------------------------
    if (data.fixtureKind != FixtureKind::Library)
    {
        // Procedural fixture — rebuild from the saved kind + params. No file and
        // no stored geometry: CreateProceduralFixture builds the box now (the
        // scene is still empty from ClearAll above). For a Dynamic fixture the
        // single RebuildDynamicFixture() after the object/insert restore below
        // re-fits it to the fully-loaded scene.
        FixtureDefinition fixDef;
        fixDef.kind = data.fixtureKind;
        fixDef.parametric = data.fixtureParametric;
        fixDef.dynamic = data.fixtureDynamic;
        fixDef.allowPerimeterInjection = true;   // procedural default (Part 3)
        m_fixtureDef = fixDef;

        LoadFixtureIntoScene(fixDef);

        // Procedural fixtures carry no injection points of their own; a saved
        // sprue still restores its active point, and perimeter injection is on.
        if (data.sprue.placed)
            m_canvas->SetActiveInjectionPoint(data.sprue.injectionPoint);
        m_canvas->SetInjectionPoints(fixDef.injectionPoints);
        m_canvas->SetAllowPerimeterInjection(true);
    }
    else if (!data.fixturePath.empty())
    {
        FixtureDefinition fixDef;
        std::string fixError;
        if (FixtureFile::Load(data.fixturePath, fixDef, fixError))
        {
            m_fixtureDef = fixDef;
            AppConfig::SaveLastFixture(fixDef.fixturePath);

            LoadFixtureIntoScene(fixDef);

            // Set injection point (use the one from the sprue data if available,
            // otherwise fall back to the first in the fixture)
            if (data.sprue.placed)
                m_canvas->SetActiveInjectionPoint(data.sprue.injectionPoint);
            else if (!fixDef.injectionPoints.empty())
                m_canvas->SetActiveInjectionPoint(fixDef.injectionPoints[0]);

            m_canvas->SetInjectionPoints(fixDef.injectionPoints);

            // Apply fixture's per-feature defaults FIRST, so that any UI
            // fields the project hasn't customised pick up the fixture's
            // values. SetParameterFields below then overwrites the numeric
            // fields with the project's saved values, preserving exact
            // round-trip behaviour for previously saved projects.
            ApplyFixtureDefaults(fixDef);
        }
        else
        {
            wxMessageBox("Could not load fixture:\n" + fixError,
                "Warning", wxOK | wxICON_WARNING, this);
        }
    }

    // ---- Restore UI parameters ---------------------------------------------
    SetParameterFields(data.params);

    // ---- Restore imported objects ------------------------------------------
    for (const auto& obj : data.objects)
    {
        m_canvas->RestoreObject(obj.sourcePath, obj.pos,
            obj.yawDeg, obj.pitchDeg,
            obj.rollDeg, obj.scale,
            obj.mirrorX, obj.mirrorZ);
    }

    // ---- Restore inserts ----------------------------------------------------
    // After objects: each insert re-parents by index into the objects just
    // restored (same order they were saved). An insert whose parent didn't
    // restore is dropped inside RestoreInsert.
    for (const auto& in : data.inserts)
    {
        m_canvas->RestoreInsert(in.sourcePath, in.parentIndex,
            in.localOffset, in.localRotDeg, in.localScale);
    }

    // A Dynamic fixture was built against the empty scene during fixture load
    // above; now that every object and insert is restored, re-fit it once to
    // the full scene (and rebuild its perimeter before the sprue/feature
    // restores below rely on it). No-op for Library / Parametric fixtures.
    m_canvas->RebuildDynamicFixture();

    // ---- Restore sprue -----------------------------------------------------
    if (data.sprue.placed)
        m_canvas->RestoreSprue(data.sprue);

    // ---- Restore runners ---------------------------------------------------
    for (const auto& rn : data.runners)
    {
        if (rn.isComplex && rn.nodes.size() >= 2)
        {
            std::vector<PathNode> nodes;
            nodes.reserve(rn.nodes.size());
            for (const auto& pn : rn.nodes)
            {
                PathNode nd;
                nd.pos = pn.pos;
                nd.dir = pn.dir;
                nd.handleLen = pn.handleLen;
                nd.handleIn = pn.handleIn;
                nd.handleOut = pn.handleOut;
                nd.handlesLinked = pn.handlesLinked;
                nd.handlesManual = pn.handlesManual;
                nodes.push_back(nd);
            }
            m_canvas->RestoreRunnerComplex(rn.point, nodes, rn.smooth);
        }
        else
        {
            m_canvas->RestoreRunner(rn.point);
        }
    }

    // ---- Restore gates -----------------------------------------------------
    for (const auto& gt : data.gates)
    {
        if (gt.isComplex && gt.nodes.size() >= 2)
        {
            std::vector<PathNode> nodes;
            nodes.reserve(gt.nodes.size());
            for (const auto& pn : gt.nodes)
            {
                PathNode nd;
                nd.pos = pn.pos;
                nd.dir = pn.dir;
                nd.handleLen = pn.handleLen;
                nd.handleIn = pn.handleIn;
                nd.handleOut = pn.handleOut;
                nd.handlesLinked = pn.handlesLinked;
                nd.handlesManual = pn.handlesManual;
                nodes.push_back(nd);
            }
            m_canvas->RestoreGateComplex(gt.pos, gt.normal, nodes, gt.smooth,
                gt.parentIndex, gt.localPos, gt.localNormal);
        }
        else
        {
            m_canvas->RestoreGate(gt.pos, gt.normal,
                gt.parentIndex, gt.localPos, gt.localNormal);
        }
    }

    // ---- Restore vents -----------------------------------------------------
    for (const auto& vn : data.vents)
    {
        if (vn.isComplex && vn.nodes.size() >= 2)
        {
            std::vector<PathNode> nodes;
            nodes.reserve(vn.nodes.size());
            for (const auto& pn : vn.nodes)
            {
                PathNode nd;
                nd.pos = pn.pos;
                nd.dir = pn.dir;
                nd.handleLen = pn.handleLen;
                nd.handleIn = pn.handleIn;
                nd.handleOut = pn.handleOut;
                nd.handlesLinked = pn.handlesLinked;
                nd.handlesManual = pn.handlesManual;
                nodes.push_back(nd);
            }

            m_canvas->RestoreVentComplex(vn.pos, vn.normal, nodes, vn.smooth,
                data.params.ventWidth,
                data.params.ventLength,
                data.params.ventOverrunStart,
                data.params.ventOverrunEnd,
                vn.parentIndex, vn.localPos, vn.localNormal);
        }
        else
        {
            m_canvas->RestoreVent(vn.pos, vn.normal,
                data.params.ventWidth,
                data.params.ventLength,
                data.params.ventOverrunStart,
                data.params.ventOverrunEnd,
                vn.parentIndex, vn.localPos, vn.localNormal);
        }
    }

    // ---- Restore ejectors --------------------------------------------------
    // Diameter and length come from data.params and are applied by
    // RebuildEjectorSolids (called via RebuildAllFeatures below) which
    // reads them out of the now-populated UI fields.
    for (const auto& ej : data.ejectors)
        m_canvas->RestoreEjector(ej.point);

    // ---- Rebuild all derived GPU geometry -----------------------------------
    m_canvas->RebuildAllFeatures();

    m_projectPath = loadPath;
    SetTitle("Mould3r - " + wxFileName(loadPath).GetName());

    // A loaded project starts in the "no mould generated yet" state.
    // Restoring features through the canvas's Restore* methods (called
    // above for objects/vents/runners/etc) does not generate the mould
    // itself — those just rebuild the scene's source geometry — so the
    // Export gate is correctly closed until the user runs Generate.
    // The various Restore* paths may have fired NotifySceneMutated
    // during load already; this explicit set locks the final state.
    m_mouldState = MouldState::NeverGenerated;
}

// ---------------------------------------------------------------------------
// SetParameterFields — populate the left-panel UI fields from saved data.
// ---------------------------------------------------------------------------
void MainFrame::SetParameterFields(const ProjectParameters& p)
{
    auto setField = [](wxTextCtrl* ctrl, float value)
    {
        if (ctrl)
            ctrl->SetValue(wxString::Format("%.4g", value));
    };

    // Project stores mm internally; convert for display if imperial
    const float conv = m_imperial ? (1.0f / 25.4f) : 1.0f;

    setField(m_ventWidth, p.ventWidth * conv);
    setField(m_ventLength, p.ventLength * conv);
    setField(m_ventOverrunStart, p.ventOverrunStart * conv);
    setField(m_ventOverrunEnd, p.ventOverrunEnd * conv);
    setField(m_sprueDiameter, p.sprueDiameter * conv);
    setField(m_sprueDraftAngle, p.sprueDraftAngle);          // degrees — no conversion
    setField(m_sprueColdSlugDepth, p.sprueColdSlugDepth * conv);
    setField(m_sprueLength, p.sprueLength * conv);
    setField(m_sprueOverrun, p.sprueOverrun * conv);
    setField(m_runnerDiameter, p.runnerDiameter * conv);
    setField(m_runnerColdSlugDepth, p.runnerColdPlugDist * conv);
    setField(m_gateDiameter, p.gateDiameter * conv);
    setField(m_gateDraftAngle, p.gateDraftAngle);           // degrees — no conversion
    setField(m_gateOverrun, p.gateOverrun * conv);
    setField(m_subRunnerDiameter, p.subRunnerDiameter * conv);
    setField(m_ejectorDiameter, p.ejectorDiameter * conv);
    setField(m_ejectorLength, p.ejectorLength * conv);
}

// ---------------------------------------------------------------------------
// ApplyFixtureDefaults — copy fixture-specified per-feature default overrides
// into the side-panel UI fields.
//
// Each FixtureDefinition::*Defaults struct holds std::optional fields; only
// the optionals that are set get written to the UI, so a fixture that
// overrides only (say) sprue diameter leaves every other sprue field at its
// existing value. On a freshly built side panel that existing value is the
// application's hardcoded default; on an already-populated panel (e.g. user
// changed a fixture mid-session) prior values are kept where the new fixture
// is silent.
//
// Unit handling matches SetParameterFields: lengths come in mm and are scaled
// to the current display unit; angles are degrees and pass through.
//
// Type-string overrides target the wxChoice controls. When the override
// doesn't match a known entry we leave the choice untouched — this keeps
// fixtures forward-compatible with builds that haven't added a particular
// type yet, and avoids surprising the user with a silent fallback to index 0.
// ---------------------------------------------------------------------------
void MainFrame::ApplyFixtureDefaults(const FixtureDefinition& def)
{
    // Length conversion: stored as mm in the fixture, displayed in current unit.
    const float lenConv = m_imperial ? (1.0f / 25.4f) : 1.0f;

    auto setLen = [&](wxTextCtrl* ctrl, const std::optional<float>& v)
    {
        if (ctrl && v)
            ctrl->SetValue(wxString::Format("%.4g", *v * lenConv));
    };
    auto setDeg = [&](wxTextCtrl* ctrl, const std::optional<float>& v)
    {
        if (ctrl && v)
            ctrl->SetValue(wxString::Format("%.4g", *v));
    };

    // Type-choice override. We synthesise a wxEVT_CHOICE so any handlers
    // bound to the control (which currently show/hide the matching dimensions
    // panel) run the same way they would on a user click — keeps the UI
    // consistent if/when more than one type per category exists.
    auto setChoice = [](wxChoice* ctrl, const std::optional<std::string>& v)
    {
        if (!ctrl || !v) return;
        const int idx = ctrl->FindString(wxString::FromUTF8(v->c_str()));
        if (idx == wxNOT_FOUND) return;        // unknown type — ignore
        if (idx == ctrl->GetSelection()) return; // already selected
        ctrl->SetSelection(idx);

        wxCommandEvent evt(wxEVT_CHOICE, ctrl->GetId());
        evt.SetEventObject(ctrl);
        evt.SetInt(idx);
        ctrl->GetEventHandler()->ProcessEvent(evt);
    };

    // ---- Vent ---------------------------------------------------------------
    setChoice(m_ventTypeChoice, def.ventDefaults.type);
    setLen(m_ventLength, def.ventDefaults.length);
    setLen(m_ventWidth, def.ventDefaults.width);
    setLen(m_ventOverrunStart, def.ventDefaults.overrunStart);
    setLen(m_ventOverrunEnd, def.ventDefaults.overrunEnd);

    // ---- Sprue --------------------------------------------------------------
    setChoice(m_sprueTypeChoice, def.sprueDefaults.type);
    setLen(m_sprueDiameter, def.sprueDefaults.diameter);
    setDeg(m_sprueDraftAngle, def.sprueDefaults.draftAngle);
    setLen(m_sprueColdSlugDepth, def.sprueDefaults.coldSlugLength);
    setLen(m_sprueLength, def.sprueDefaults.length);
    setLen(m_sprueOverrun, def.sprueDefaults.overrun);

    // ---- Runner -------------------------------------------------------------
    setChoice(m_runnerTypeChoice, def.runnerDefaults.type);
    setLen(m_runnerDiameter, def.runnerDefaults.diameter);
    setLen(m_runnerColdSlugDepth, def.runnerDefaults.coldSlugLength);

    // ---- Gate ---------------------------------------------------------------
    setChoice(m_gateTypeChoice, def.gateDefaults.type);
    setLen(m_gateDiameter, def.gateDefaults.diameter);
    setDeg(m_gateDraftAngle, def.gateDefaults.draftAngle);

    // ---- Sub-runner ---------------------------------------------------------
    setChoice(m_subRunnerTypeChoice, def.subRunnerDefaults.type);
    setLen(m_subRunnerDiameter, def.subRunnerDefaults.diameter);

    // ---- Ejector ------------------------------------------------------------
    setChoice(m_ejectorTypeChoice, def.ejectorDefaults.type);
    setLen(m_ejectorDiameter, def.ejectorDefaults.diameter);
    setLen(m_ejectorLength, def.ejectorDefaults.length);

    // ---- Grid ---------------------------------------------------------------
    // Start from the current grid settings and override only the fields the
    // fixture specifies (presence-driven, like the feature defaults above), so
    // a fixture silent on the grid — or one predating [grid_defaults] — leaves
    // the live grid untouched. Then push to the canvas so it re-renders.
    {
        const GridDefaults& g = def.gridDefaults;
        if (g.shape)
            m_gridSettings.shape = (*g.shape == "circular")
            ? GridShape::Circular : GridShape::Rectangular;
        if (g.sizeX)      m_gridSettings.sizeX = *g.sizeX;
        if (g.sizeY)      m_gridSettings.sizeY = *g.sizeY;
        if (g.radius)     m_gridSettings.radius = *g.radius;
        if (g.spokes)     m_gridSettings.spokes = *g.spokes;
        if (g.spacing)    m_gridSettings.spacing = *g.spacing;
        if (g.majorEvery) m_gridSettings.majorEvery = *g.majorEvery;

        if (m_canvas)
            m_canvas->SetGridSettings(m_gridSettings);
    }
}

void MainFrame::OnBrowseExport(wxCommandEvent&)
{
    wxDirDialog dlg(this, "Select export folder", "",
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK)
        m_exportPath->SetValue(dlg.GetPath());
}

void MainFrame::OnExport(wxCommandEvent&)
{
    // Action zone of the export split button: run whichever mode the dropdown
    // currently selects. Each DoExport* keeps its own gate + file dialog.
    switch (m_exportMode)
    {
    case ExportMode::Mould:    DoExportMould();    break;
    case ExportMode::ShotBody: DoExportShotBody(); break;
    }
}

void MainFrame::OnExportModeChanged(wxCommandEvent& e)
{
    // Menu-item order is fixed in CreateRibbon: 0 = Mould, 1 = Shot body.
    m_exportMode = (e.GetInt() == 1) ? ExportMode::ShotBody
                                     : ExportMode::Mould;
    UpdateExportButtonLabel();
}

void MainFrame::UpdateExportButtonLabel()
{
    if (!m_btnExport) return;
    m_btnExport->SetLabel(m_exportMode == ExportMode::ShotBody
        ? "Export Shot Body" : "Export Mould");
}

void MainFrame::DoExportMould()
{
    // Gate: tri-state model (see MouldState enum in MainFrame.h).
    //   NeverGenerated  - nothing to export. Hard block with an
    //                     informational popup, then return.
    //   Dirty           - mould exists but is stale relative to the
    //                     current scene. Warn the user; let them decide
    //                     whether to proceed with the older geometry.
    //                     State stays Dirty after a Yes — the user
    //                     accepted this export but the mould isn't
    //                     refreshed, so a subsequent click warrants
    //                     another warning.
    //   Clean           - happy path. Fall through to the file dialog
    //                     below without prompting.
    switch (m_mouldState)
    {
    case MouldState::NeverGenerated:
        wxMessageBox("Mould must be generated before it can be exported.",
            "Export", wxOK | wxICON_INFORMATION, this);
        return;

    case MouldState::Dirty:
    {
        const int ans = wxMessageBox(
            "An edit has been made since the last Mould Generation, "
            "some features may not be reflected. Would you like to continue?",
            "Export", wxYES_NO | wxICON_WARNING, this);
        if (ans != wxYES) return;
        break;
    }

    case MouldState::Clean:
        break;
    }

    // Filename-driven export. Instead of asking only for an output folder
    // and writing fixed "model_a.step" / "model_b.step" into it, prompt
    // the user for a base filename and append "_a.step" / "_b.step" to
    // produce the two halves.
    //
    // Suggestion logic:
    //   - If a project has been saved or loaded (m_projectPath non-empty),
    //     use the project's stem as the suggested base filename and start
    //     the dialog in the project's directory. This makes the common
    //     case (export named-project mould) a single Enter press.
    //   - Otherwise, no default — the user types a name from scratch.
    //
    // Note on overwrite handling: we deliberately do NOT pass
    // wxFD_OVERWRITE_PROMPT to the file dialog. The path the user picks
    // (e.g. "widget.step") is not what we actually write — we write
    // "widget_a.step" and "widget_b.step" — so wx's built-in prompt
    // would ask about the wrong file. We do a manual existence check on
    // the real output paths below instead.
    wxString suggestedDir;
    wxString suggestedName;
    if (!m_projectPath.empty())
    {
        wxFileName fn(m_projectPath);
        suggestedDir = fn.GetPath();
        suggestedName = fn.GetName();   // bare stem, no extension
    }

    // Mesh scenes export STL (the halves are meshes); BREP scenes export STEP.
    const bool meshScene = m_canvas->IsSceneMeshType();
    const wxString ext = meshScene ? ".stl" : ".step";
    const wxString wildcard = meshScene
        ? "STL files (*.stl)|*.stl|All files (*.*)|*.*"
        : "STEP files (*.step;*.stp)|*.step;*.stp|All files (*.*)|*.*";

    wxFileDialog dlg(this, "Export Mould Halves",
        suggestedDir, suggestedName,
        wildcard,
        wxFD_SAVE);

    if (dlg.ShowModal() != wxID_OK)
        return;

    // Strip extension from whatever the user typed/selected. wxFileName
    // strips only the trailing extension, so "widget.step" -> "widget"
    // and "widget.v2.step" -> "widget.v2", which is the right policy.
    // We deliberately do not strip a trailing "_a"/"_b" — if the user
    // explicitly types one, treat it as part of their chosen base.
    wxFileName picked(dlg.GetPath());
    const wxString folder = picked.GetPath();
    const wxString baseStem = picked.GetName();

    if (baseStem.IsEmpty())
    {
        wxMessageBox("Please enter a filename for the export.",
            "Export", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const wxString pathA = folder + wxFileName::GetPathSeparator()
        + baseStem + "_a" + ext;
    const wxString pathB = folder + wxFileName::GetPathSeparator()
        + baseStem + "_b" + ext;

    // Manual overwrite check on the real output paths. List only the
    // halves that actually exist so the prompt isn't misleading when
    // only one of the two would be overwritten.
    wxString existing;
    if (wxFileExists(pathA)) existing += pathA + "\n";
    if (wxFileExists(pathB)) existing += pathB + "\n";
    if (!existing.IsEmpty())
    {
        const int ans = wxMessageBox(
            "The following file(s) already exist:\n\n"
            + existing + "\nOverwrite?",
            "Export", wxYES_NO | wxICON_QUESTION, this);
        if (ans != wxYES) return;
    }

    m_canvas->ExportFixtures(pathA.ToStdString(), pathB.ToStdString());
}

void MainFrame::DoExportShotBody()
{
    // Same tri-state gate as DoExportMould (see MouldState enum in
    // MainFrame.h) — the shot body is a by-product of the same Generate Mould
    // run as the mould halves, so it goes stale on the same schedule.
    switch (m_mouldState)
    {
    case MouldState::NeverGenerated:
        wxMessageBox("Mould must be generated before the shot body can be exported.",
            "Export Shot Body", wxOK | wxICON_INFORMATION, this);
        return;

    case MouldState::Dirty:
    {
        const int ans = wxMessageBox(
            "An edit has been made since the last Mould Generation, "
            "some features may not be reflected. Would you like to continue?",
            "Export Shot Body", wxYES_NO | wxICON_WARNING, this);
        if (ans != wxYES) return;
        break;
    }

    case MouldState::Clean:
        break;
    }

    // The mould as a whole can be Clean while the shot specifically failed
    // to build (e.g. no objects/feed features, or the fuse came back empty —
    // see the "Shot model" section of GenerateMould), so check separately
    // rather than assuming Clean implies a shot exists.
    if (!m_canvas->HasLastShotMesh())
    {
        wxMessageBox("No shot body is available to export (it may be empty, "
            "or the last generation failed to build one).",
            "Export Shot Body", wxOK | wxICON_INFORMATION, this);
        return;
    }

    // Single-file export (unlike the two-half Export Mould dialog above), so
    // the path the user picks IS the path we write — wxFD_OVERWRITE_PROMPT
    // is safe to use here, no manual existence check needed.
    wxString suggestedDir;
    wxString suggestedName;
    if (!m_projectPath.empty())
    {
        wxFileName fn(m_projectPath);
        suggestedDir = fn.GetPath();
        suggestedName = fn.GetName() + "_shot";
    }

    // Mesh scenes export STL (the shot has no BREP — see GenerateMould);
    // BREP scenes export STEP, matching Export Mould's routing.
    const bool meshScene = m_canvas->IsSceneMeshType();
    const wxString wildcard = meshScene
        ? "STL files (*.stl)|*.stl|All files (*.*)|*.*"
        : "STEP files (*.step;*.stp)|*.step;*.stp|All files (*.*)|*.*";

    wxFileDialog dlg(this, "Export Shot Body",
        suggestedDir, suggestedName,
        wildcard,
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    if (dlg.ShowModal() != wxID_OK)
        return;

    // wxFileName strips only a trailing extension; if the user typed a bare
    // name with no extension, supply the correct one for the scene type.
    wxFileName picked(dlg.GetPath());
    if (picked.GetExt().IsEmpty())
        picked.SetExt(meshScene ? "stl" : "step");

    m_canvas->ExportShotBody(picked.GetFullPath().ToStdString());
}

void MainFrame::OnGenerateMould(wxCommandEvent&)
{
    if (!m_canvas) return;
    // GenerateMould returns true only when both up-front validations
    // (fixtures loaded, objects to subtract) pass and the build pipeline
    // ran to its natural end. Failure leaves the previous mould state
    // untouched — the user gets the validation message box from inside
    // GenerateMould itself.
    //
    // Subtle ordering: GenerateMould internally calls into mutating
    // canvas methods (PlaceSprue at the very least), each of which
    // fires NotifySceneMutated, which routes through the canvas callback
    // and may bump Clean → Dirty mid-flight. The state set below runs
    // AFTER GenerateMould returns and authoritatively reflects "the
    // mould is now fresh", so it wins regardless of how many
    // intermediate Clean→Dirty toggles happened inside.
    if (m_canvas->GenerateMould())
    {
        m_mouldState = MouldState::Clean;

        // Seed the embedded preview perspective with the post-cut halves and,
        // when one was built, the shot artefacts (mesh + BREP + face map +
        // volume) — the shot gets its own "Shot" toggle and feeds the design
        // checks. SetData clears the previous generation's parts first, so the
        // preview always reflects the latest run. The actual GL upload is
        // deferred until the Preview page is visible (see PreviewPanel).
        const auto& halves = m_canvas->GetLastMouldMeshes();
        if (m_previewPanel && !halves.empty())
        {
            ShotPreviewInput shot;
            shot.sceneIsMesh = m_canvas->IsSceneMeshType();
            // Which mould kind produced this run — the preview locks cast
            // generation to procedural (Parametric / Dynamic) moulds.
            shot.mouldKind = m_fixtureDef.kind;
            if (m_canvas->HasLastShotMesh())
            {
                shot.mesh = &m_canvas->GetLastShotMesh();
                shot.shape = &m_canvas->GetLastShotShape();
                shot.faceIds = &m_canvas->GetLastShotFaceIds();
                shot.volumeMm3 = m_canvas->GetLastShotVolumeMm3();
                shot.halves = &m_canvas->GetLastHalfShapes();
                // Augmented shot (vents + scaled inserts + ejectors) for the
                // cast-mould bases; null-safe when none was built.
                if (m_canvas->HasLastCastShotMesh())
                    shot.castMesh = &m_canvas->GetLastCastShotMesh();
            }

            // Inserts form their own preview category (one checkbox, yellow).
            // Passed even when empty — SetData treats an empty list as "no
            // insert checkbox", so a run without inserts is unaffected.
            m_previewPanel->SetData(halves, shot,
                m_canvas->GetLastInsertMeshes());
        }

        // Jump to the Preview perspective so the freshly generated mould is
        // shown — mirrors the old behaviour of the preview window popping open.
        SetPerspective(Perspective::Preview);
    }
}

wxPanel* MainFrame::CreateCollapsibleSection(wxWindow* parent,
    wxSizer* parentSizer,
    const wxString& title,
    wxPanel** contentOut)
{
    auto* headerBtn = new wxToggleButton(parent, wxID_ANY, title,
        wxDefaultPosition, wxSize(-1, 28),
        wxBU_LEFT);
    headerBtn->SetValue(true);
    headerBtn->SetBackgroundColour(Style::SectionHeaderBg);
    headerBtn->SetForegroundColour(Style::Accent);
    headerBtn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    headerBtn->SetBitmap(LoadSvgBundle(kChevronDownSvg, wxSize(14, 14), true));
    headerBtn->SetBitmapPosition(wxLEFT);
    parentSizer->Add(headerBtn, 0, wxEXPAND | wxTOP, 4);

    // Use custom content if provided, otherwise build placeholder
    wxPanel* content = nullptr;
    if (contentOut && *contentOut)
    {
        content = *contentOut;
        parentSizer->Add(content, 0, wxEXPAND);
    }
    else
    {
        content = new wxPanel(parent, wxID_ANY);
        content->SetBackgroundColour(kRibbonBg);

        auto* cs = new wxBoxSizer(wxVERTICAL);
        auto* placeholder = new wxStaticText(content, wxID_ANY,
            "Add options later");
        placeholder->SetForegroundColour(Style::TextDim);
        placeholder->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        cs->Add(placeholder, 0, wxALL, 10);
        content->SetSizer(cs);
        parentSizer->Add(content, 0, wxEXPAND);

        if (contentOut) *contentOut = content;
    }

    wxPanel* contentRef = content;
    headerBtn->Bind(wxEVT_TOGGLEBUTTON, [headerBtn, contentRef,
        parent, title](wxCommandEvent&)
    {
        const bool expanded = headerBtn->GetValue();
        headerBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg,
            wxSize(14, 14), true));
        contentRef->Show(expanded);
        parent->Layout();
        parent->GetParent()->Layout();
    });

    return content;
}

RoundedButton* MainFrame::MakePlaceButton(wxWindow* parent, int id,
    const wxString& label)
{
    // Matches the "Place Sprue" button: rounded corners (RoundedButton),
    // BtnPlace background, white semibold text, 32px tall. The Sprue button is
    // a one-shot action; these are placement-mode toggles, so the handlers
    // (bound to wxEVT_BUTTON) flip in/out of the mode based on the canvas's
    // current TransformMode, and the visual selected state is driven externally
    // via the setter registered below.
    auto* btn = new RoundedButton(parent, id, label,
        wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
    btn->SetBackgroundColour(Style::BtnPlace);
    btn->SetForegroundColour(*wxWHITE);
    btn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));

    // Register a setter so SetActiveTool can drive this button's visual state
    // externally (button click, Escape clearing the mode, or the canvas
    // snapping back to Select after a placement completes).
    m_toolBtnSetters[id] = [btn](bool active) {
        btn->SetBackgroundColour(active ? Style::BtnSecondarySelected
            : Style::BtnPlace);
        btn->Refresh();
    };

    return btn;
}

wxPanel* MainFrame::CreateVentsContent(wxWindow* parent)
{

    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Section title ------------------------------------------------------
    auto* titleLabel = new wxStaticText(panel, wxID_ANY, "Vents");
    titleLabel->SetForegroundColour(*wxWHITE);
    titleLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(6);

    // ---- "Place Vent" toggle button -----------------------------------------
    auto* btnPlace = MakePlaceButton(panel, ID_ToolPlaceVent, "Place Vent");
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(6);

    // ---- Edit / Remove / Clear all ------------------------------------------
    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);
    auto makeSmallBtn = [&](const wxString& label) -> RoundedButton*
    {
        auto* btn = new RoundedButton(panel, wxID_ANY, label,
            wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
        btn->SetBackgroundColour(Style::BtnSmall);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        return btn;
    };
    auto* btnEdit = makeSmallBtn("Edit");
    btnEdit->SetId(ID_EditVent);
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveVent);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearVentPoints);
    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(8);

    // ---- Collapsible "Settings" sub-section ---------------------------------
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // Settings content
    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Vent type dropdown (inline with label) -----------------------------
    auto* typeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Vent type:");
    typeLabel->SetForegroundColour(Style::TextMuted);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_ventTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_ventTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_ventTypeChoice->SetForegroundColour(Style::TextMuted);
    m_ventTypeChoice->Append("Rectangular");
    m_ventTypeChoice->SetSelection(0);
    typeRow->Add(typeLabel, 0, wxALIGN_CENTER_VERTICAL);
    typeRow->AddStretchSpacer(1);
    typeRow->Add(m_ventTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(typeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- Dimensions panel --------------------------------------------------
    m_ventDimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    m_ventDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);

    auto addDimRow = [&](const wxString& label, wxTextCtrl*& ctrl,
        const wxString& defaultVal)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(m_ventDimsPanel, wxID_ANY, label);
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        ctrl = new wxTextCtrl(m_ventDimsPanel, wxID_ANY, defaultVal,
            wxDefaultPosition, wxSize(kFieldWidth, 22));
        ctrl->SetBackgroundColour(Style::BtnSmall);
        ctrl->SetForegroundColour(kTextDefault);
        auto* unit = new wxStaticText(m_ventDimsPanel, wxID_ANY, "mm");
        m_mmUnitLabels.push_back(unit);
        unit->SetForegroundColour(Style::TextSubtle);
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        unit->SetMinSize(wxSize(kUnitWidth, -1));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    };

    addDimRow("Length:", m_ventLength, "1.0");
    addDimRow("Width:", m_ventWidth, "2.0");
    addDimRow("Overrun (start):", m_ventOverrunStart, "0.5");
    addDimRow("Overrun (end):", m_ventOverrunEnd, "0.5");

    m_ventDimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(m_ventDimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    m_ventTypeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
    {
        m_ventDimsPanel->Show(m_ventTypeChoice->GetStringSelection() == "Rectangular");
        m_ventDimsPanel->GetParent()->Layout();
        m_ventDimsPanel->GetParent()->GetParent()->Layout();
        m_ventDimsPanel->GetParent()->GetParent()->GetParent()->Layout();
    });

    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&)
    {
        static wxLongLong lastToggleMs = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - lastToggleMs).GetValue() < 200) { settingsBtn->SetValue(!settingsBtn->GetValue()); return; }
        lastToggleMs = now;
        const bool expanded = settingsBtn->GetValue();
        settingsBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg,
            wxSize(12, 12), true));
        settingsBtn->SetBitmapPosition(wxRIGHT);
        settingsPanel->Show(expanded);
        panel->Layout(); panel->GetParent()->Layout(); panel->GetParent()->GetParent()->Layout();
    });

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::CreateSpruesContent(wxWindow* parent)
{

    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* titleLabel = new wxStaticText(panel, wxID_ANY, "Sprues");
    titleLabel->SetForegroundColour(*wxWHITE);
    titleLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(6);

    // Place Sprue — regular button (one-shot action, not a toggle mode)
    auto* btnPlace = new RoundedButton(panel, ID_PlaceSprue, "Place Sprue",
        wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
    btnPlace->SetBackgroundColour(Style::BtnPlace);
    btnPlace->SetForegroundColour(*wxWHITE);
    btnPlace->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(6);

    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);
    auto makeSmallBtn = [&](const wxString& label) -> RoundedButton* {
        auto* btn = new RoundedButton(panel, wxID_ANY, label,
            wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
        btn->SetBackgroundColour(Style::BtnSmall);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        return btn;
    };
    auto* btnEdit = makeSmallBtn("Edit");
    btnEdit->SetId(ID_EditSprue);
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveSprue);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearSprue);
    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(8);

    // Collapsible Settings
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    auto* typeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Sprue type:");
    typeLabel->SetForegroundColour(Style::TextMuted);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_sprueTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_sprueTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_sprueTypeChoice->SetForegroundColour(Style::TextMuted);
    m_sprueTypeChoice->Append("Cylinder");
    m_sprueTypeChoice->SetSelection(0);
    typeRow->Add(typeLabel, 0, wxALIGN_CENTER_VERTICAL);
    typeRow->AddStretchSpacer(1);
    typeRow->Add(m_sprueTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(typeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* dimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    dimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);

    auto addRow = [&](const wxString& label, wxTextCtrl*& ctrl, const wxString& defVal, const wxString& unitStr) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, label);
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        ctrl = new wxTextCtrl(dimsPanel, wxID_ANY, defVal, wxDefaultPosition, wxSize(kFieldWidth, 22));
        ctrl->SetBackgroundColour(Style::BtnSmall); ctrl->SetForegroundColour(kTextDefault);
        auto* u = new wxStaticText(dimsPanel, wxID_ANY, unitStr);
        if (unitStr == "mm") m_mmUnitLabels.push_back(u);
        u->SetForegroundColour(Style::TextSubtle);
        u->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        u->SetMinSize(wxSize(kUnitWidth, -1));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(u, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    };

    addRow("Diameter:", m_sprueDiameter, "5.0", "mm");
    addRow("Draft angle:", m_sprueDraftAngle, "1.0", wxString::FromUTF8("\xC2\xB0"));
    addRow("Cold slug:", m_sprueColdSlugDepth, "5.0", "mm");
    addRow("Sprue length:", m_sprueLength, "20.0", "mm");
    addRow("Overrun:", m_sprueOverrun, "0.0", "mm");

    dimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 10);
    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    m_sprueTypeChoice->Bind(wxEVT_CHOICE, [dimsPanel, this](wxCommandEvent&) {
        dimsPanel->Show(m_sprueTypeChoice->GetStringSelection() == "Cylinder");
        dimsPanel->GetParent()->Layout(); dimsPanel->GetParent()->GetParent()->Layout();
        dimsPanel->GetParent()->GetParent()->GetParent()->Layout();
    });

    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&) {
        static wxLongLong lastToggleMs = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - lastToggleMs).GetValue() < 200) { settingsBtn->SetValue(!settingsBtn->GetValue()); return; }
        lastToggleMs = now;
        const bool expanded = settingsBtn->GetValue();
        settingsBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg,
            wxSize(12, 12), true));
        settingsBtn->SetBitmapPosition(wxRIGHT);
        settingsPanel->Show(expanded);
        panel->Layout(); panel->GetParent()->Layout(); panel->GetParent()->GetParent()->Layout();
    });

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::CreateRunnersContent(wxWindow* parent)
{
    // Local colours sampled from the reference mockup

    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- "Runners" section title (inside the card) --------------------------
    auto* runnersLabel = new wxStaticText(panel, wxID_ANY, "Runners");
    runnersLabel->SetForegroundColour(*wxWHITE);
    runnersLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(runnersLabel, 0, wxLEFT | wxTOP, 12);

    sizer->AddSpacer(6);

    // ---- "Place Runner" toggle button (full-width, muted indigo) -------------
    auto* btnPlace = MakePlaceButton(panel, ID_PlaceRunner, "Place Runner");
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    sizer->AddSpacer(6);

    // ---- Edit / Remove / Clear all — equal-width button row -----------------
    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);   // 1 row, 3 cols, 4px h-gap

    auto makeSmallBtn = [&](const wxString& label) -> RoundedButton*
    {
        auto* btn = new RoundedButton(panel, wxID_ANY, label,
            wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
        btn->SetBackgroundColour(Style::BtnSmall);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        return btn;
    };

    auto* btnEdit = makeSmallBtn("Edit");
    btnEdit->SetId(ID_EditRunner);
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveRunner);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearRunners);

    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    sizer->AddSpacer(8);

    // ---- Collapsible "Settings" sub-section ---------------------------------
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // Settings content panel (contains the existing type/dimension fields)
    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Runner type dropdown (inline with label) ----------------------------
    auto* typeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Runner type:");
    typeLabel->SetForegroundColour(Style::TextMuted);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_runnerTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_runnerTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_runnerTypeChoice->SetForegroundColour(Style::TextMuted);
    m_runnerTypeChoice->Append("Cylindrical");
    m_runnerTypeChoice->SetSelection(0);
    typeRow->Add(typeLabel, 0, wxALIGN_CENTER_VERTICAL);
    typeRow->AddStretchSpacer(1);
    typeRow->Add(m_runnerTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(typeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- Dimensions panel (shown for Cylindrical) ---------------------------
    auto* dimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    dimsPanel->SetBackgroundColour(Style::CardBg);

    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);

    // Diameter row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Diameter:");
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_runnerDiameter = new wxTextCtrl(dimsPanel, wxID_ANY, "4.0",
            wxDefaultPosition, wxSize(kFieldWidth, 22));
        m_runnerDiameter->SetBackgroundColour(Style::BtnSmall);
        m_runnerDiameter->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "mm");
        m_mmUnitLabels.push_back(unit);
        unit->SetForegroundColour(Style::TextSubtle);
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        unit->SetMinSize(wxSize(kUnitWidth, -1));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(m_runnerDiameter, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // Cold Slug Well Row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Cold slug length:");
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_runnerColdSlugDepth = new wxTextCtrl(dimsPanel, wxID_ANY, "5.0",
            wxDefaultPosition, wxSize(kFieldWidth, 22));
        m_runnerColdSlugDepth->SetBackgroundColour(Style::BtnSmall);
        m_runnerColdSlugDepth->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "mm");
        m_mmUnitLabels.push_back(unit);
        unit->SetForegroundColour(Style::TextSubtle);
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        unit->SetMinSize(wxSize(kUnitWidth, -1));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(m_runnerColdSlugDepth, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    dimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    // Show/hide dims based on type selection (future-proofed for more types)
    m_runnerTypeChoice->Bind(wxEVT_CHOICE, [dimsPanel, this](wxCommandEvent&)
    {
        dimsPanel->Show(m_runnerTypeChoice->GetStringSelection() == "Cylindrical");
        dimsPanel->GetParent()->Layout();
        dimsPanel->GetParent()->GetParent()->Layout();
        dimsPanel->GetParent()->GetParent()->GetParent()->Layout();
    });

    // Toggle the Settings sub-section (debounced — 200ms cooldown)
    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&)
    {
        static wxLongLong lastToggleMs = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - lastToggleMs).GetValue() < 200)
        {
            // Too fast — revert the toggle state and ignore
            settingsBtn->SetValue(!settingsBtn->GetValue());
            return;
        }
        lastToggleMs = now;

        const bool expanded = settingsBtn->GetValue();
        settingsBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg,
            wxSize(12, 12), true));
        settingsBtn->SetBitmapPosition(wxRIGHT);
        settingsPanel->Show(expanded);
        panel->Layout();
        panel->GetParent()->Layout();
        panel->GetParent()->GetParent()->Layout();
    });

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::CreateGatesContent(wxWindow* parent)
{

    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* titleLabel = new wxStaticText(panel, wxID_ANY, "Gates");
    titleLabel->SetForegroundColour(*wxWHITE);
    titleLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(6);

    auto* btnPlace = MakePlaceButton(panel, ID_PlaceGate, "Place Gate");
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(6);

    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);
    auto makeSmallBtn = [&](const wxString& label) -> RoundedButton* {
        auto* btn = new RoundedButton(panel, wxID_ANY, label,
            wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
        btn->SetBackgroundColour(Style::BtnSmall);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        return btn;
    };
    auto* btnEdit = makeSmallBtn("Edit");
    btnEdit->SetId(ID_EditGate);
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveGate);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearGates);
    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(8);

    // Collapsible Settings
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    // Helper for dimension rows
    auto addRow = [&](wxWindow* parent_, wxSizer* parentSz,
        const wxString& label, wxTextCtrl*& ctrl,
        const wxString& defVal, const wxString& unitStr, int lblW = 60)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(parent_, wxID_ANY, label);
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        ctrl = new wxTextCtrl(parent_, wxID_ANY, defVal, wxDefaultPosition, wxSize(kFieldWidth, 22));
        ctrl->SetBackgroundColour(Style::BtnSmall); ctrl->SetForegroundColour(kTextDefault);
        auto* u = new wxStaticText(parent_, wxID_ANY, unitStr);
        if (unitStr == "mm") m_mmUnitLabels.push_back(u);
        u->SetForegroundColour(Style::TextSubtle);
        u->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        u->SetMinSize(wxSize(kUnitWidth, -1));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(u, 0, wxALIGN_CENTER_VERTICAL);
        parentSz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    };

    // ---- Gate type dropdown (inline with label) ------------------------------
    auto* typeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Gate type:");
    typeLabel->SetForegroundColour(Style::TextMuted);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_gateTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_gateTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_gateTypeChoice->SetForegroundColour(Style::TextMuted);
    m_gateTypeChoice->Append("Tapered Cylinder");
    m_gateTypeChoice->SetSelection(0);
    typeRow->Add(typeLabel, 0, wxALIGN_CENTER_VERTICAL);
    typeRow->AddStretchSpacer(1);
    typeRow->Add(m_gateTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(typeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // Gate dimensions
    auto* dimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    dimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);
    addRow(dimsPanel, dimsSizer, "Diameter:", m_gateDiameter, "3.0", "mm");
    addRow(dimsPanel, dimsSizer, "Draft angle:", m_gateDraftAngle, "1.0", wxString::FromUTF8("\xC2\xB0"));
    // Overrun extends the gate cylinder backward into the model along the
    // path direction, so the cut clears irregular geometry near the
    // parting-line entry. Default 0 = current behaviour (gate starts at
    // the parting surface). The radius at the parting surface is preserved
    // — see RebuildGateSolids for the math.
    addRow(dimsPanel, dimsSizer, "Overrun:", m_gateOverrun, "0.0", "mm");
    dimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 6);

    // ---- Sub-runner divider ------------------------------------------------
    auto* subSep = new wxPanel(settingsPanel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    subSep->SetBackgroundColour(Style::Divider);
    settingsSizer->Add(subSep, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // ---- Sub-runner type dropdown (inline with label) ------------------------
    auto* subTypeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* subTypeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Sub-runner type:");
    subTypeLabel->SetForegroundColour(Style::TextMuted);
    subTypeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_subRunnerTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_subRunnerTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_subRunnerTypeChoice->SetForegroundColour(Style::TextMuted);
    m_subRunnerTypeChoice->Append("Cylinder");
    m_subRunnerTypeChoice->SetSelection(0);
    subTypeRow->Add(subTypeLabel, 0, wxALIGN_CENTER_VERTICAL);
    subTypeRow->AddStretchSpacer(1);
    subTypeRow->Add(m_subRunnerTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(subTypeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // Sub-runner dimensions
    auto* subDimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    subDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* subDimsSizer = new wxBoxSizer(wxVERTICAL);
    addRow(subDimsPanel, subDimsSizer, "Diameter:", m_subRunnerDiameter, "5.0", "mm");
    subDimsPanel->SetSizer(subDimsSizer);
    settingsSizer->Add(subDimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    m_gateTypeChoice->Bind(wxEVT_CHOICE, [dimsPanel, this](wxCommandEvent&) {
        dimsPanel->Show(m_gateTypeChoice->GetStringSelection() == "Tapered Cylinder");
        dimsPanel->GetParent()->Layout(); dimsPanel->GetParent()->GetParent()->Layout();
        dimsPanel->GetParent()->GetParent()->GetParent()->Layout();
    });

    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&) {
        static wxLongLong lastToggleMs = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - lastToggleMs).GetValue() < 200) { settingsBtn->SetValue(!settingsBtn->GetValue()); return; }
        lastToggleMs = now;
        const bool expanded = settingsBtn->GetValue();
        settingsBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg,
            wxSize(12, 12), true));
        settingsBtn->SetBitmapPosition(wxRIGHT);
        settingsPanel->Show(expanded);
        panel->Layout(); panel->GetParent()->Layout(); panel->GetParent()->GetParent()->Layout();
    });

    panel->SetSizer(sizer);
    return panel;
}

// ---------------------------------------------------------------------------
// CreateEjectorsContent — left-panel "Ejectors" feature card.
//
// Mirrors CreateGatesContent's layout (title, Place button, three small
// action buttons, collapsible Settings panel with a type dropdown and
// dimension rows). Currently UI-only: the Place / Edit / Remove / Clear
// buttons fire the corresponding TransformMode handlers, but the canvas-
// side mode behaviour is a placeholder pending the geometry implementation.
//
// The Type dropdown is wired the same way as the Gates card's "Tapered
// Cylinder" branch: changing the dropdown shows / hides the Cylindrical
// dimension panel. To add a new ejector geometry later, append the type
// to m_ejectorTypeChoice and add a parallel dimension panel + Show()
// branch in the EVT_CHOICE handler — no other plumbing required.
// ---------------------------------------------------------------------------
wxPanel* MainFrame::CreateEjectorsContent(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* titleLabel = new wxStaticText(panel, wxID_ANY, "Ejectors");
    titleLabel->SetForegroundColour(*wxWHITE);
    titleLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(6);

    auto* btnPlace = MakePlaceButton(panel, ID_PlaceEjector, "Place Ejector");
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(6);

    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);
    auto makeSmallBtn = [&](const wxString& label) -> RoundedButton* {
        auto* btn = new RoundedButton(panel, wxID_ANY, label,
            wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
        btn->SetBackgroundColour(Style::BtnSmall);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        return btn;
    };
    auto* btnEdit = makeSmallBtn("Edit");
    btnEdit->SetId(ID_EditEjector);
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveEjector);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearEjectors);
    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(8);

    // Collapsible Settings — same chevron / debounce pattern as the other cards.
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    // Dimension-row helper — identical to the one used in CreateGatesContent.
    auto addRow = [&](wxWindow* parent_, wxSizer* parentSz,
        const wxString& label, wxTextCtrl*& ctrl,
        const wxString& defVal, const wxString& unitStr, int /*lblW*/ = 60)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(parent_, wxID_ANY, label);
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        ctrl = new wxTextCtrl(parent_, wxID_ANY, defVal, wxDefaultPosition, wxSize(kFieldWidth, 22));
        ctrl->SetBackgroundColour(Style::BtnSmall); ctrl->SetForegroundColour(kTextDefault);
        auto* u = new wxStaticText(parent_, wxID_ANY, unitStr);
        if (unitStr == "mm") m_mmUnitLabels.push_back(u);
        u->SetForegroundColour(Style::TextSubtle);
        u->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        u->SetMinSize(wxSize(kUnitWidth, -1));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(u, 0, wxALIGN_CENTER_VERTICAL);
        parentSz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    };

    // ---- Ejector type dropdown (inline with label) ------------------------
    auto* typeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Ejector type:");
    typeLabel->SetForegroundColour(Style::TextMuted);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_ejectorTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_ejectorTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_ejectorTypeChoice->SetForegroundColour(Style::TextMuted);
    m_ejectorTypeChoice->Append("Cylindrical");
    m_ejectorTypeChoice->SetSelection(0);
    typeRow->Add(typeLabel, 0, wxALIGN_CENTER_VERTICAL);
    typeRow->AddStretchSpacer(1);
    typeRow->Add(m_ejectorTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(typeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // Cylindrical dimensions. When a second geometry is added later, build
    // a parallel dimension panel and toggle visibility in the EVT_CHOICE
    // handler below — same structure as the gate card's tapered-cylinder
    // branch.
    auto* dimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    dimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);
    addRow(dimsPanel, dimsSizer, "Diameter:", m_ejectorDiameter, "3.0", "mm");
    addRow(dimsPanel, dimsSizer, "Length:", m_ejectorLength, "25.0", "mm");
    dimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    m_ejectorTypeChoice->Bind(wxEVT_CHOICE, [dimsPanel, this](wxCommandEvent&) {
        // Currently only one type — left as a Show() toggle so additional
        // types can be added by appending an else-if without restructuring.
        dimsPanel->Show(m_ejectorTypeChoice->GetStringSelection() == "Cylindrical");
        dimsPanel->GetParent()->Layout();
        dimsPanel->GetParent()->GetParent()->Layout();
        dimsPanel->GetParent()->GetParent()->GetParent()->Layout();
    });

    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&) {
        // Same 200ms debounce as the other settings togglers — mid-frame
        // double-clicks (often from a touchpad tap) otherwise re-collapse
        // the panel before the layout finishes.
        static wxLongLong lastToggleMs = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - lastToggleMs).GetValue() < 200) { settingsBtn->SetValue(!settingsBtn->GetValue()); return; }
        lastToggleMs = now;
        const bool expanded = settingsBtn->GetValue();
        settingsBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg,
            wxSize(12, 12), true));
        settingsBtn->SetBitmapPosition(wxRIGHT);
        settingsPanel->Show(expanded);
        panel->Layout(); panel->GetParent()->Layout(); panel->GetParent()->GetParent()->Layout();
    });

    panel->SetSizer(sizer);
    return panel;
}

// ---------------------------------------------------------------------------
// CreateInsertsContent — left-panel "Inserts" feature card.
//
// Same layout as CreateEjectorsContent (title, Place button, three small
// action buttons, collapsible Settings) so the card reads identically to its
// neighbours.
//
// The one structural difference is the Settings body. Every other feature
// authors its geometry from card fields, so those cards carry a type dropdown
// plus dimension rows. An insert's geometry is whatever was imported, so
// there is nothing to dimension and no second geometry to switch between —
// a type dropdown with one dead entry would be noise. Instead Settings holds
// the single value that IS authored here: "Cut scale", the percentage the
// body is scaled up by when it cuts its own pocket during Generate Mould
// (>100% opens clearance around the insert; 100% is a nominal fit). Read at
// placement time and captured onto the InsertFeature — same convention as the
// dimension fields everywhere else, so changing it affects the NEXT insert
// placed, not the ones already down.
// ---------------------------------------------------------------------------
wxPanel* MainFrame::CreateInsertsContent(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* titleLabel = new wxStaticText(panel, wxID_ANY, "Inserts");
    titleLabel->SetForegroundColour(*wxWHITE);
    titleLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(6);

    auto* btnPlace = MakePlaceButton(panel, ID_PlaceInsert, "Place Insert");
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(6);

    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);
    auto makeSmallBtn = [&](const wxString& label) -> RoundedButton* {
        auto* btn = new RoundedButton(panel, wxID_ANY, label,
            wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
        btn->SetBackgroundColour(Style::BtnSmall);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        return btn;
    };
    auto* btnEdit = makeSmallBtn("Edit");
    btnEdit->SetId(ID_EditInsert);
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveInsert);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearInserts);
    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(8);

    // Collapsible Settings — same chevron / debounce pattern as the other cards.
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    // Dimension-row helper — identical to the one used in CreateEjectorsContent.
    // Note the unitStr == "mm" guard: "%" never lands in m_mmUnitLabels, so the
    // Cut scale label survives the metric/imperial switch unchanged.
    auto addRow = [&](wxWindow* parent_, wxSizer* parentSz,
        const wxString& label, wxTextCtrl*& ctrl,
        const wxString& defVal, const wxString& unitStr, int /*lblW*/ = 60)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(parent_, wxID_ANY, label);
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        ctrl = new wxTextCtrl(parent_, wxID_ANY, defVal, wxDefaultPosition, wxSize(kFieldWidth, 22));
        ctrl->SetBackgroundColour(Style::BtnSmall); ctrl->SetForegroundColour(kTextDefault);
        auto* u = new wxStaticText(parent_, wxID_ANY, unitStr);
        if (unitStr == "mm") m_mmUnitLabels.push_back(u);
        u->SetForegroundColour(Style::TextSubtle);
        u->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        u->SetMinSize(wxSize(kUnitWidth, -1));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(u, 0, wxALIGN_CENTER_VERTICAL);
        parentSz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    };

    auto* dimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    dimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);
    addRow(dimsPanel, dimsSizer, "Cut scale:", m_insertCutScale, "100.0", "%");
    dimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&) {
        // Same 200ms debounce as the other settings togglers — mid-frame
        // double-clicks (often from a touchpad tap) otherwise re-collapse
        // the panel before the layout finishes.
        static wxLongLong lastToggleMs = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - lastToggleMs).GetValue() < 200) { settingsBtn->SetValue(!settingsBtn->GetValue()); return; }
        lastToggleMs = now;
        const bool expanded = settingsBtn->GetValue();
        settingsBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg,
            wxSize(12, 12), true));
        settingsBtn->SetBitmapPosition(wxRIGHT);
        settingsPanel->Show(expanded);
        panel->Layout(); panel->GetParent()->Layout(); panel->GetParent()->GetParent()->Layout();
    });

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::CreateLeftPanel(wxWindow* parent)
{
    // Outer container: content column + right border
    auto* outer = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(300, -1));
    outer->SetBackgroundColour(kRibbonBg);
    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // Content column: fixed Model Tools on top, scrollable mould settings below
    auto* column = new wxPanel(outer, wxID_ANY);
    column->SetBackgroundColour(kRibbonBg);
    auto* colSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Import Model (sits above Model Tools) ------------------------------
    // Moved here from the top ribbon: importing is a Prepare-time action, so it
    // lives at the top of the Prepare workspace. Bound to ID_Import (the
    // existing OnImport binding in CreateRibbon catches it via propagation).
    {
        auto* btnImport = new RoundedButton(column, ID_Import, "Import Model",
            wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
        btnImport->SetBackgroundColour(Style::BtnSecondary);
        btnImport->SetForegroundColour(*wxWHITE);
        btnImport->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
        colSizer->Add(btnImport, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }

    // ---- Model Tools section (fixed, non-scrolling) -------------------------
    {

        auto* toolsPanel = new wxPanel(column, wxID_ANY);
        toolsPanel->SetBackgroundColour(kRibbonBg);
        auto* toolsSizer = new wxBoxSizer(wxVERTICAL);

        // Header matching the MOULD TOOL SETTINGS style
        auto* toolsLabel = new wxStaticText(toolsPanel, wxID_ANY, "MODEL TOOLS");
        toolsLabel->SetForegroundColour(Style::TextPrimary);
        toolsLabel->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        toolsSizer->Add(toolsLabel, 0, wxLEFT | wxTOP, 12);

        //auto* toolsLine = new wxPanel(toolsPanel, wxID_ANY,
        //    wxDefaultPosition, wxSize(-1, 1));
        //toolsLine->SetBackgroundColour(Style::TextPrimary);
        //toolsSizer->Add(toolsLine, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        toolsSizer->AddSpacer(8);

        // ---- SVG icon paths for model tool buttons --------------------------------
        // Fill in the path to each SVG file (relative to the executable, or absolute).
        // Leave a string empty ("") to show the text label only.
        static const wxString kIconMove = "res/icons/arrows-move.svg";
        static const wxString kIconRotate = "res/icons/rotate-2.svg";
        static const wxString kIconScale = "res/icons/resize.svg";
        static const wxString kIconPattern = "res/icons/pattern.svg";
        static const wxString kIconPrecisionPlace = "res/icons/precision-place.svg";
        static const wxString kIconCenter = "res/icons/focus-centered.svg";
        static const wxString kIconAlignFace = "res/icons/align-face.svg";
        static const wxString kIconAlignMidplane = "res/icons/align-midplane.svg";

        // Helper: load an SVG, recolor all strokes and fills to white, and return a
        // wxBitmapBundle.  Relative paths are anchored to the executable directory.
        // Returns an invalid bundle (IsOk() == false) if the path is empty or missing.
        auto LoadToolIcon = [](const wxString& svgPath) -> wxBitmapBundle
        {
            if (svgPath.IsEmpty())
                return wxBitmapBundle();

            wxFileName fn(svgPath);
            if (fn.IsRelative())
            {
                wxFileName exeDir(wxStandardPaths::Get().GetExecutablePath());
                fn.MakeAbsolute(exeDir.GetPath());
            }

            // Read raw SVG text so we can override its colors before rendering.
            wxFile file(fn.GetFullPath());
            if (!file.IsOpened())
                return wxBitmapBundle();
            wxString svg;
            file.ReadAll(&svg);

            // Replace the most common color tokens used by icon sets (e.g. Lucide)
            // with plain white so the icon matches the button text color.
            svg.Replace("currentColor", "white");
            svg.Replace("\"black\"", "\"white\"");
            svg.Replace("\"#000000\"", "\"white\"");
            svg.Replace("\"#000\"", "\"white\"");

            const wxScopedCharBuffer utf8 = svg.utf8_str();
            return wxBitmapBundle::FromSVG(utf8.data(), wxSize(18, 18));
        };

        static const wxFont kToolBtnFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI");

        auto makeToolBtn = [&](int id, const wxString& label, bool toggle,
            const wxString& svgPath = "") -> wxWindow*
        {
            // Use a plain wxPanel so we can freely position the icon+text
            // with a sizer, giving true centred layout that native buttons
            // won't provide once a bitmap is attached.
            auto* panel = new wxPanel(toolsPanel, wxID_ANY,
                wxDefaultPosition, wxSize(-1, 34), wxBORDER_NONE);
            panel->SetBackgroundColour(Style::BtnSecondary);

            // ---- Rounded-corner repaint -----------------------------------
            // The panel paints itself: parent bg fills the whole client
            // area first (so the four corner triangles outside the rounded
            // shape pick up the toolbar's colour), then a filled rounded
            // rectangle in the panel's *current* bg colour covers the rest.
            // applyColours below mutates panel->SetBackgroundColour and
            // calls Refresh(), so the existing hover / selected / idle
            // state machine drives the paint with no extra wiring.
            //
            // wxBG_STYLE_PAINT promises wxWidgets we'll fill the client
            // area ourselves — required when pairing with wxAutoBuffered-
            // PaintDC, otherwise the default erase pass fights the buffer
            // and the result flickers on hover.
            //
            // Matches the pattern used by RoundedButton.cpp; the 4 px
            // radius keeps these in lockstep with the text-only
            // RoundedButton's default. One constant to tune if the
            // design ever wants a different number.
            constexpr int kToolBtnCornerRadius = 4;
            panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
            panel->Bind(wxEVT_PAINT, [panel](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(panel);
                const wxColour parentBg = panel->GetParent()
                    ? panel->GetParent()->GetBackgroundColour()
                    : panel->GetBackgroundColour();
                dc.SetBackground(wxBrush(parentBg));
                dc.Clear();

                std::unique_ptr<wxGraphicsContext> gc(
                    wxGraphicsContext::Create(dc));
                if (!gc) return;
                gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
                gc->SetBrush(wxBrush(panel->GetBackgroundColour()));
                gc->SetPen(*wxTRANSPARENT_PEN);
                const wxSize sz = panel->GetClientSize();
                gc->DrawRoundedRectangle(0, 0, sz.x, sz.y,
                    kToolBtnCornerRadius);
            });

            // Inner horizontal sizer: [icon] [gap] [label]
            auto* hSizer = new wxBoxSizer(wxHORIZONTAL);

            wxStaticBitmap* bmpCtrl = nullptr;
            wxBitmapBundle icon = LoadToolIcon(svgPath);
            if (icon.IsOk())
            {
                bmpCtrl = new wxStaticBitmap(panel, wxID_ANY,
                    icon.GetBitmapFor(panel));
                hSizer->Add(bmpCtrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            }

            auto* txt = new wxStaticText(panel, wxID_ANY, label);
            txt->SetForegroundColour(Style::TextPrimary);
            txt->SetBackgroundColour(Style::BtnSecondary);
            txt->SetFont(kToolBtnFont);
            hSizer->Add(txt, 0, wxALIGN_CENTER_VERTICAL);

            // Wrap in a centering sizer using stretch spacers
            auto* outer = new wxBoxSizer(wxHORIZONTAL);
            outer->AddStretchSpacer(1);
            outer->Add(hSizer, 0, wxALIGN_CENTER_VERTICAL);
            outer->AddStretchSpacer(1);
            panel->SetSizer(outer);

            // Shared toggle state (avoids raw-pointer lifetime issues)
            auto toggled = std::make_shared<bool>(false);

            // Helpers to apply normal / hover / active colours
            auto applyColours = [=](const wxColour& bg, const wxColour& fg) {
                panel->SetBackgroundColour(bg);
                txt->SetBackgroundColour(bg);
                txt->SetForegroundColour(fg);
                panel->Refresh();
                txt->Refresh();
            };

            // Register a setter so SetActiveTool can drive this button's
            // visual state externally (e.g. when Escape clears the mode).
            // The lambda closes over the same `toggled` pointer the click
            // handler uses, so both routes converge on the same state.
            if (toggle)
            {
                m_toolBtnSetters[id] = [toggled, applyColours](bool active) {
                    if (*toggled == active) return;  // already in target state
                    *toggled = active;
                    applyColours(active ? Style::BtnSecondarySelected : Style::BtnSecondary,
                        active ? kTextActive : Style::TextPrimary);
                };
            }

            // Left-click: fire the appropriate command event and update visuals
            auto onClick = [=](wxMouseEvent& e) {
                if (toggle)
                {
                    *toggled = !*toggled;
                    applyColours(*toggled ? Style::BtnSecondarySelected : Style::BtnSecondary,
                        *toggled ? kTextActive : Style::TextPrimary);
                    wxCommandEvent evt(wxEVT_TOGGLEBUTTON, id);
                    evt.SetEventObject(panel);
                    evt.SetInt(*toggled ? 1 : 0);
                    panel->GetEventHandler()->ProcessEvent(evt);
                }
                else
                {
                    wxCommandEvent evt(wxEVT_BUTTON, id);
                    evt.SetEventObject(panel);
                    panel->GetEventHandler()->ProcessEvent(evt);
                }
                e.Skip();
            };

            // Hover colours (only when not toggled-on)
            auto onEnter = [=](wxMouseEvent& e) {
                if (!*toggled)
                    applyColours(Style::BtnSecondaryHover, Style::TextPrimary);
                e.Skip();
            };
            auto onLeave = [=](wxMouseEvent& e) {
                // Phantom-leave guard: txt and bmpCtrl are real child
                // windows of the panel, so the panel fires LEAVE as
                // soon as the cursor crosses onto either one — even
                // though, from the user's point of view, the cursor is
                // still very much on the button. The corollary ENTER
                // on the child does fire, but the relative ordering
                // between the two isn't guaranteed on Windows and we
                // were getting a stuck-off hover from the race.
                //
                // Fix: hit-test the cursor in screen coords against
                // the panel's screen rect. If it's still anywhere over
                // the composite, the leave is phantom — suppress it.
                // A genuine leave (cursor truly off the button) lands
                // outside the rect and falls through to the colour
                // reset.
                const wxRect screenRect(panel->GetScreenPosition(),
                    panel->GetSize());
                if (!screenRect.Contains(wxGetMousePosition()))
                {
                    if (!*toggled)
                        applyColours(Style::BtnSecondary, Style::TextPrimary);
                }
                e.Skip();
            };

            // Bind events to the panel and every child so the full hit-area works
            for (wxWindow* w : { (wxWindow*)panel, (wxWindow*)txt,
                                 (wxWindow*)bmpCtrl })
            {
                if (!w) continue;
                w->Bind(wxEVT_LEFT_UP, onClick);
                w->Bind(wxEVT_ENTER_WINDOW, onEnter);
                w->Bind(wxEVT_LEAVE_WINDOW, onLeave);
            }

            return panel;
        };

        // Row 1 — Move / Rotate / Scale in equal thirds.
        auto* toolsRow1 = new wxGridSizer(1, 3, 0, 4);
        toolsRow1->Add(makeToolBtn(ID_ToolTranslate, "Move", true, kIconMove), 0, wxEXPAND);
        toolsRow1->Add(makeToolBtn(ID_ToolRotate, "Rotate", true, kIconRotate), 0, wxEXPAND);
        toolsRow1->Add(makeToolBtn(ID_ToolScale, "Scale", true, kIconScale), 0, wxEXPAND);
        toolsSizer->Add(toolsRow1, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        toolsSizer->AddSpacer(4);  // matches the original 4-px inter-row gap

        // Row 2 — Center / Pattern in halves. Center is the only non-toggle
        // (one-shot action); the rest of the model tools are modal toggles.
        // Precision Place name reduced to "Place" for placement on row 2 for ui purposes.
        // Built toggle-style like the other transform dialogs so it gets the
        // same press feedback; OnToolPrecisionPlace clears the toggle as soon
        // as the dialog opens.
        auto* toolsRow2 = new wxGridSizer(1, 3, 0, 4);
        toolsRow2->Add(makeToolBtn(ID_ToolCenter, "Center", false, kIconCenter), 0, wxEXPAND);
        toolsRow2->Add(makeToolBtn(ID_ToolPattern, "Pattern", true, kIconPattern), 0, wxEXPAND);
        toolsRow2->Add(makeToolBtn(ID_ToolPrecisionPlace, "Place", true, kIconPrecisionPlace), 0, wxEXPAND);
        toolsSizer->Add(toolsRow2, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        toolsSizer->AddSpacer(10);// gap before the next subsection header

        // ---- Model Alignment subsection ------------------------------------
        // Same MODEL TOOLS header treatment plus an inline help indicator.
        // The "?" character is a placeholder for the circled-question-mark
        // glyph in the reference image — see flag in the chat, the final
        // visual (Unicode glyph vs. SVG icon vs. custom-painted badge) is
        // intentionally left as a follow-up once the help-text content is
        // settled.
        auto* alignHeaderRow = new wxBoxSizer(wxHORIZONTAL);

        auto* alignLabel = new wxStaticText(toolsPanel, wxID_ANY, "MODEL ALIGNMENT");
        alignLabel->SetForegroundColour(Style::TextPrimary);
        alignLabel->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        alignHeaderRow->Add(alignLabel, 0, wxALIGN_CENTER_VERTICAL);

        auto* alignHelp = new wxStaticText(toolsPanel, wxID_ANY, "?");
        alignHelp->SetForegroundColour(Style::TextMuted);
        alignHelp->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        alignHelp->SetToolTip("Align selected geometry to a reference face or "
            "midplane of another object.");
        alignHeaderRow->Add(alignHelp, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

        toolsSizer->Add(alignHeaderRow, 0, wxLEFT | wxTOP, 12);
        toolsSizer->AddSpacer(8);

        // Row 3 — Align Face / Align Midplane in halves.
        auto* alignRow = new wxGridSizer(1, 2, 0, 4);
        alignRow->Add(makeToolBtn(ID_ToolAlignFace, "Align Face", true, kIconAlignFace), 0, wxEXPAND);
        alignRow->Add(makeToolBtn(ID_ToolAlignMidplane, "Align Midplane", true, kIconAlignMidplane), 0, wxEXPAND);
        toolsSizer->Add(alignRow, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

        toolsSizer->AddSpacer(10);

        toolsPanel->SetSizer(toolsSizer);
        colSizer->Add(toolsPanel, 0, wxEXPAND);
    }

    //Create dividing line
    auto* titleLine = new wxPanel(column, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 1));
    titleLine->SetBackgroundColour(Style::TextPrimary);
    colSizer->Add(titleLine, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);


    // ---- Mould settings title (fixed, non-scrolling) ------------------------
    auto* title = new wxStaticText(column, wxID_ANY, "MOULD TOOL SETTINGS");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    colSizer->Add(title, 0, wxLEFT | wxTOP, 12);
    colSizer->AddSpacer(8);


    // ---- Scrollable mould tool settings -------------------------------------
    auto* scrollWin = new wxScrolledWindow(column, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxVSCROLL | wxBORDER_NONE);
    scrollWin->SetScrollRate(0, 8);
    scrollWin->SetBackgroundColour(kRibbonBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    sizer->AddSpacer(4);

    // ---- Feature sections (headers built into each content panel) -----------
    wxPanel* spruesContent = CreateSpruesContent(scrollWin);
    sizer->Add(spruesContent, 0, wxEXPAND | wxTOP, 8);

    wxPanel* runnersContent = CreateRunnersContent(scrollWin);
    sizer->Add(runnersContent, 0, wxEXPAND | wxTOP, 8);

    wxPanel* gatesContent = CreateGatesContent(scrollWin);
    sizer->Add(gatesContent, 0, wxEXPAND | wxTOP, 8);

    wxPanel* ventsContent = CreateVentsContent(scrollWin);
    sizer->Add(ventsContent, 0, wxEXPAND | wxTOP, 8);

    wxPanel* ejectorsContent = CreateEjectorsContent(scrollWin);
    sizer->Add(ejectorsContent, 0, wxEXPAND | wxTOP, 8);

    wxPanel* insertsContent = CreateInsertsContent(scrollWin);
    sizer->Add(insertsContent, 0, wxEXPAND | wxTOP, 8);

    sizer->AddSpacer(12);

    scrollWin->SetSizer(sizer);
    colSizer->Add(scrollWin, 1, wxEXPAND);   // scroll area fills remaining space 

    column->SetSizer(colSizer);
    outerSizer->Add(column, 1, wxEXPAND);

    // Right border line
    auto* borderLine = new wxPanel(outer, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    borderLine->SetBackgroundColour(Style::Divider);
    outerSizer->Add(borderLine, 0, wxEXPAND);

    outer->SetSizer(outerSizer);
    return outer;
}