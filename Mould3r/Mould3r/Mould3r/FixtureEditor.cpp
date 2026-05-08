#include "FixtureEditor.h"

#include <wx/bmpbndl.h>
#include <wx/file.h>
#include <wx/filedlg.h>  // wxFileDialog — Import buttons
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/tglbtn.h>   // wxEVT_TOGGLEBUTTON — synthesised by the makeToolBtn helper
#include <memory>

#include "FixtureCanvas.h"
#include "style.h"

// ---------------------------------------------------------------------------
// Local style aliases — kept narrow on purpose. The toolbar wants the same
// look as MainFrame's MODEL TOOLS panel (Style::BtnSecondary, hover/selected
// states, white labels), but doesn't pull in MainFrame.cpp's full ribbon
// constants. If a design refresh moves these, MainFrame.cpp's constants need
// the same treatment — they're parallel by design, not by inheritance.
// ---------------------------------------------------------------------------
namespace
{
    const wxColour& kEditorBg = Style::AppBg;
    const wxColour& kTextActive = Style::TextActive;

    static const wxFont kToolBtnFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI");

    // SVG icon paths for fixture-editor tool buttons — same assets used by
    // MainFrame's MODEL TOOLS panel, intentionally so the two toolbars stay
    // visually identical.
    const wxString kIconMove = "res/icons/arrows-move.svg";
    const wxString kIconRotate = "res/icons/rotate-2.svg";
    const wxString kIconScale = "res/icons/resize.svg";
    const wxString kIconCenter = "res/icons/focus-centered.svg";
    const wxString kIconAlignFace = "res/icons/align-face.svg";

    // Load an SVG, recolor strokes/fills to white, and return a bitmap
    // bundle at 18px (matches the main toolbar). Relative paths anchor to
    // the executable directory. Returns an invalid bundle on miss; the
    // caller falls through to text-only display.
    //
    // Mirrors LoadToolIcon() in MainFrame.cpp. If the icon-loading rules
    // ever diverge between toolbars this is the place to change; until
    // then they're deliberately kept in lock-step.
    wxBitmapBundle LoadToolIcon(const wxString& svgPath)
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

        // Replace the most common color tokens used by icon sets (e.g.
        // Lucide) with plain white so the icon matches the button text.
        svg.Replace("currentColor", "white");
        svg.Replace("\"black\"", "\"white\"");
        svg.Replace("\"#000000\"", "\"white\"");
        svg.Replace("\"#000\"", "\"white\"");

        const wxScopedCharBuffer utf8 = svg.utf8_str();
        return wxBitmapBundle::FromSVG(utf8.data(), wxSize(18, 18));
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
FixtureEditor::FixtureEditor(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, "Fixture Editor",
        wxDefaultPosition, wxSize(1000, 680),
        wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
{
    SetBackgroundColour(kEditorBg);

    // Match the parent dialog's minimum size sensibly — small enough to
    // tile next to the StartupDialog on a 1366x768 laptop, large enough
    // that the toolbar + canvas placeholder don't crowd each other.
    SetMinSize(wxSize(720, 480));

    BuildUI();

    // Bindings — the toggle buttons go through wxEVT_TOGGLEBUTTON because
    // the makeToolBtn helper synthesizes that event type for toggles, and
    // wxEVT_BUTTON for momentary actions (Center) and the Import buttons.
    Bind(wxEVT_BUTTON, &FixtureEditor::OnImportModelA, this, ID_FE_ImportModelA);
    Bind(wxEVT_BUTTON, &FixtureEditor::OnImportModelB, this, ID_FE_ImportModelB);
    Bind(wxEVT_TOGGLEBUTTON, &FixtureEditor::OnToolMove, this, ID_FE_Move);
    Bind(wxEVT_TOGGLEBUTTON, &FixtureEditor::OnToolRotate, this, ID_FE_Rotate);
    Bind(wxEVT_TOGGLEBUTTON, &FixtureEditor::OnToolScale, this, ID_FE_Scale);
    Bind(wxEVT_BUTTON, &FixtureEditor::OnToolCenter, this, ID_FE_Center);
    Bind(wxEVT_TOGGLEBUTTON, &FixtureEditor::OnToolAlignFace, this, ID_FE_AlignFace);

    // Explicit close handler. Without this, parenting the frame to a modal
    // wxDialog (StartupDialog runs ShowModal while the editor is open) can
    // swallow the default close path on some platforms — clicking the X
    // does nothing visible. Forwarding to Destroy() makes close behavior
    // unconditional and also matches the documented wxFrame teardown
    // pattern (close events on frames should call Destroy, not delete).
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Destroy(); });

    CentreOnScreen();
}

void FixtureEditor::BuildUI()
{
    // Outer layout is now two-tier:
    //   1. top ribbon (Import Mould Half A / B + path labels)
    //   2. content row: toolbar | canvas
    auto* root = new wxPanel(this, wxID_ANY);
    root->SetBackgroundColour(kEditorBg);

    auto* vSizer = new wxBoxSizer(wxVERTICAL);

    vSizer->Add(BuildTopRibbon(root), 0, wxEXPAND);

    // 1-px separator between ribbon and content — same accent-line treatment
    // used between the main app's ribbon and viewport so the visual seam
    // reads consistently.
    auto* sep = new wxPanel(root, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    sep->SetBackgroundColour(Style::Accent);
    vSizer->Add(sep, 0, wxEXPAND);

    auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);
    contentSizer->Add(BuildToolbar(root), 0, wxEXPAND);
    contentSizer->Add(BuildCanvasArea(root), 1, wxEXPAND);
    vSizer->Add(contentSizer, 1, wxEXPAND);

    root->SetSizer(vSizer);

    // Frame sizer wraps the root panel so margins/padding land on the
    // panel rather than on the frame itself.
    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(root, 1, wxEXPAND);
    SetSizer(frameSizer);
}

// ---------------------------------------------------------------------------
// Top ribbon
// ---------------------------------------------------------------------------
wxWindow* FixtureEditor::BuildTopRibbon(wxWindow* parent)
{
    // Two stacked rows — height per row matches the main ribbon's button
    // (32px) plus a small margin top/bottom. Total ribbon height ends up
    // around 80px.
    auto* ribbon = new wxPanel(parent, wxID_ANY);
    ribbon->SetBackgroundColour(kEditorBg);

    auto* vSizer = new wxBoxSizer(wxVERTICAL);

    // Empty-state placeholder shown next to a button until the user picks
    // a file. Kept identical between rows so they line up visually.
    static const wxString kNoFileText = "(no file selected)";

    static const wxFont kBtnFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI");
    static const wxFont kPathFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI");

    // Builds one [Import button] [path label] row. Returns the path
    // wxStaticText so the caller can stash it in a member for later
    // updates.
    auto buildRow = [&](int btnId, const wxString& btnLabel) -> wxStaticText*
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);

            // Button — same indigo/white treatment as the main ribbon's
            // "Import Model" button, just sized wider to fit the longer
            // "Import Mould Half X" text.
            auto* btn = new wxButton(ribbon, btnId, btnLabel,
                wxDefaultPosition, wxSize(180, 32), wxBORDER_NONE);
            btn->SetBackgroundColour(Style::BtnSecondary);
            btn->SetForegroundColour(*wxWHITE);
            btn->SetFont(kBtnFont);
            row->Add(btn, 0, wxALIGN_CENTER_VERTICAL);

            // Path label. wxST_ELLIPSIZE_END so a long absolute path
            // doesn't push the right edge of the ribbon out — same
            // treatment StartupDialog uses for the fixtures-folder
            // label.
            auto* lbl = new wxStaticText(ribbon, wxID_ANY, kNoFileText,
                wxDefaultPosition, wxDefaultSize,
                wxST_ELLIPSIZE_END);
            lbl->SetForegroundColour(Style::TextSubtext);
            lbl->SetFont(kPathFont);
            // proportion=1 so the label takes the remaining horizontal
            // space and the ellipsize behavior actually has something to
            // ellipsize against.
            row->Add(lbl, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

            vSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
            return lbl;
        };

    m_lblModelAPath = buildRow(ID_FE_ImportModelA, "Import Mould Half A");
    m_lblModelBPath = buildRow(ID_FE_ImportModelB, "Import Mould Half B");
    vSizer->AddSpacer(8);   // bottom padding so the separator doesn't hug the buttons

    ribbon->SetSizer(vSizer);
    return ribbon;
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
wxWindow* FixtureEditor::BuildToolbar(wxWindow* parent)
{
    // Fixed-width column on the left. Width matches roughly the cell
    // width of MainFrame's 2-column tool grid plus side padding so the
    // buttons read as the same size to the user.
    auto* toolbar = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(160, -1));
    toolbar->SetBackgroundColour(kEditorBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Section header — matches the "MODEL TOOLS" label style in MainFrame.
    auto* header = new wxStaticText(toolbar, wxID_ANY, "FIXTURE TOOLS");
    header->SetForegroundColour(Style::TextPrimary);
    header->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(header, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(8);

    // ------------------------------------------------------------------
    // makeToolBtn — builds one toolbar button (icon + label on a coloured
    // panel) and registers a setter into m_toolBtnSetters so SetActiveTool
    // can drive its visual state externally. Parallels the same-named
    // helper in MainFrame.cpp; intentionally kept verbose-but-faithful so
    // future style tweaks can be made by editing both copies in lockstep.
    // ------------------------------------------------------------------
    auto makeToolBtn = [&](int id, const wxString& label, bool toggle,
        const wxString& svgPath) -> wxWindow*
        {
            // Plain panel — gives true centred icon+text layout that
            // native wxButton can't deliver once a bitmap is attached.
            auto* panel = new wxPanel(toolbar, wxID_ANY,
                wxDefaultPosition, wxSize(-1, 34), wxBORDER_NONE);
            panel->SetBackgroundColour(Style::BtnSecondary);

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

            // Center the icon+label group horizontally inside the panel
            auto* outer = new wxBoxSizer(wxHORIZONTAL);
            outer->AddStretchSpacer(1);
            outer->Add(hSizer, 0, wxALIGN_CENTER_VERTICAL);
            outer->AddStretchSpacer(1);
            panel->SetSizer(outer);

            // Shared toggle state — captured by both the click handler
            // and the external SetActiveTool setter so the two routes
            // can't get out of sync.
            auto toggled = std::make_shared<bool>(false);

            auto applyColours = [=](const wxColour& bg, const wxColour& fg) {
                panel->SetBackgroundColour(bg);
                txt->SetBackgroundColour(bg);
                txt->SetForegroundColour(fg);
                panel->Refresh();
                txt->Refresh();
                };

            if (toggle)
            {
                m_toolBtnSetters[id] = [toggled, applyColours](bool active) {
                    if (*toggled == active) return;       // already in target state
                    *toggled = active;
                    applyColours(active ? Style::BtnSecondarySelected : Style::BtnSecondary,
                        active ? kTextActive : Style::TextPrimary);
                    };
            }

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

            // Hover swap only when not currently toggled-on
            auto onEnter = [=](wxMouseEvent& e) {
                if (!*toggled)
                    applyColours(Style::BtnSecondaryHover, Style::TextPrimary);
                e.Skip();
                };
            auto onLeave = [=](wxMouseEvent& e) {
                if (!*toggled)
                    applyColours(Style::BtnSecondary, Style::TextPrimary);
                e.Skip();
                };

            // Bind the panel and every child so the full hit-area works,
            // not just the bare panel background.
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

    // Vertical stack — Pattern + Align Midplane intentionally omitted
    // (see class comment in FixtureEditor.h for why).
    sizer->Add(makeToolBtn(ID_FE_Move, "Move", true, kIconMove),
        0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(makeToolBtn(ID_FE_Rotate, "Rotate", true, kIconRotate),
        0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
    sizer->Add(makeToolBtn(ID_FE_Scale, "Scale", true, kIconScale),
        0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
    sizer->Add(makeToolBtn(ID_FE_Center, "Center", false, kIconCenter),
        0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
    sizer->Add(makeToolBtn(ID_FE_AlignFace, "Align Face", true, kIconAlignFace),
        0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

    toolbar->SetSizer(sizer);
    return toolbar;
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------
wxWindow* FixtureEditor::BuildCanvasArea(wxWindow* parent)
{
    // Real GL viewport. The canvas owns its own context, camera, and grid
    // renderer — see FixtureCanvas.h for the rationale on why this isn't
    // the main GLCanvas. Stored in m_canvas so the (future) import
    // handlers and toolbar wiring can push state into it directly.
    m_canvas = new FixtureCanvas(parent);
    return m_canvas;
}

// ---------------------------------------------------------------------------
// Active-tool routing
// ---------------------------------------------------------------------------
void FixtureEditor::SetActiveTool(int activeId)
{
    m_activeToolId = activeId;
    for (auto& kv : m_toolBtnSetters)
        kv.second(kv.first == activeId);
}

// ---------------------------------------------------------------------------
// Tool button handlers — scaffolding only.
//
// Each toggle handler reads the wxCommandEvent's Int() (set to 1 when the
// button just turned ON, 0 when it turned OFF by makeToolBtn's onClick).
// On the ON edge we route through SetActiveTool so the other toggles
// deactivate; on the OFF edge we clear the active tool so no button is
// shown selected. This keeps the visual state consistent while the
// real transform-mode logic is still pending.
// ---------------------------------------------------------------------------
void FixtureEditor::OnToolMove(wxCommandEvent& evt)
{
    SetActiveTool(evt.GetInt() ? ID_FE_Move : wxID_NONE);
    // TODO: enter Translate transform mode on the fixture canvas.
}

void FixtureEditor::OnToolRotate(wxCommandEvent& evt)
{
    SetActiveTool(evt.GetInt() ? ID_FE_Rotate : wxID_NONE);
    // TODO: enter Rotate transform mode (or open RotateDialog, TBD).
}

void FixtureEditor::OnToolScale(wxCommandEvent& evt)
{
    SetActiveTool(evt.GetInt() ? ID_FE_Scale : wxID_NONE);
    // TODO: enter Scale transform mode (or open ScaleDialog, TBD).
}

void FixtureEditor::OnToolCenter(wxCommandEvent&)
{
    // Momentary action — does not affect the active-tool state. Intentional
    // mirror of MainFrame::OnToolCenter, which immediately re-centers the
    // selected object without leaving any tool latched.
    // TODO: re-center the currently-selected fixture half on the world origin.
}

void FixtureEditor::OnToolAlignFace(wxCommandEvent& evt)
{
    SetActiveTool(evt.GetInt() ? ID_FE_AlignFace : wxID_NONE);
    // TODO: enter AlignFace pick mode on the fixture canvas.
}

// ---------------------------------------------------------------------------
// Import handlers
//
// Both handlers share the same body via PickStepFile — the only difference is
// which member path/label they target. STEP filter matches what the old
// StartupDialog::OnNewFixture flow used (".step;.stp"). On a successful
// pick, the file is also pushed into the canvas's matching half slot via
// FixtureCanvas::LoadHalf, which runs the shared FileImporter pipeline
// (parse + normals + crease split + GPU upload).
// ---------------------------------------------------------------------------
void FixtureEditor::OnImportModelA(wxCommandEvent&)
{
    if (PickStepFile("Select Mould Half A", m_modelAPath, m_lblModelAPath)
        && m_canvas)
    {
        m_canvas->LoadHalf(FixtureCanvas::HalfSlot::A, m_modelAPath);
    }
}

void FixtureEditor::OnImportModelB(wxCommandEvent&)
{
    if (PickStepFile("Select Mould Half B", m_modelBPath, m_lblModelBPath)
        && m_canvas)
    {
        m_canvas->LoadHalf(FixtureCanvas::HalfSlot::B, m_modelBPath);
    }
}

bool FixtureEditor::PickStepFile(const wxString& title,
    std::string& outPath,
    wxStaticText* pathLabel)
{
    wxFileDialog dlg(this, title, "", "",
        "STEP files (*.step;*.stp)|*.step;*.stp",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return false;   // user cancelled — keep prior state

    outPath = dlg.GetPath().ToStdString();

    if (pathLabel)
    {
        // Switch from the muted empty-state look to the primary-text look
        // so a populated row is visually distinct from a never-touched one.
        pathLabel->SetLabel(outPath);
        pathLabel->SetForegroundColour(Style::TextPrimary);
        // The label sits inside an ellipsizing static-text whose width is
        // sizer-driven. Re-laying out the parent picks up the new text
        // length and re-applies the ellipsis if needed.
        if (pathLabel->GetParent())
            pathLabel->GetParent()->Layout();
    }
    return true;
}
