#include "FixtureEditor.h"

#include <wx/bmpbndl.h>
#include <wx/file.h>
#include <wx/filedlg.h>  // wxFileDialog — Import buttons + Generate Fixture save
#include <wx/filename.h>
#include <wx/msgdlg.h>   // wxMessageBox — OnGenerateFixture validation/error
#include <wx/scrolwin.h> // wxScrolledWindow — sidebar scrolls when cards overflow
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/tglbtn.h>   // wxEVT_TOGGLEBUTTON — synthesised by the makeToolBtn helper

#include <filesystem>    // OnGenerateFixture default-folder construction
#include <memory>
#include <vector>        // AddTypeRow's options parameter

#include "FixtureCanvas.h"
#include "FixtureFile.h"       // OnGenerateFixture
#include "InjectionPointDialog.h"  // Add/Edit injection-point flow
#include "RotateDialog.h"      // OnToolRotate
#include "ScaleDialog.h"       // OnToolScale
#include "TranslateDialog.h"   // OnToolMove
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
    const wxColour& kTextDefault = Style::TextPrimary;

    static const wxFont kToolBtnFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI");

    // Sidebar layout constants — kept in lock-step with MainFrame.cpp's
    // matching kFieldWidth/kUnitWidth/kFieldGap/kCtrlColWidth so the two
    // settings panels read as the same control column at a glance. If you
    // tweak one set, mirror the change in MainFrame.cpp.
    static const int kFieldWidth = 90;     // text-entry width (px)
    static const int kUnitWidth = 28;      // fixed unit-label column (px)
    static const int kFieldGap = 4;        // gap between field and unit label
    static const int kCtrlColWidth = kFieldWidth + kFieldGap + kUnitWidth;

    // Sidebar widths
    static const int kSidebarWidth = 300;  // matches MainFrame's left panel

    // Card-internal fonts — pulled out so every CreateXxxContent helper
    // hits the same sizes without duplicating wxFont constructors.
    static const wxFont kCardTitleFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI");
    static const wxFont kFieldLabelFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI");
    static const wxFont kSectionHeaderFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI");

    // Degree symbol — wxString from UTF-8 encoded "°" so this file stays
    // ASCII-safe regardless of the editor's source-encoding settings.
    inline wxString DegSym() { return wxString::FromUTF8("\xC2\xB0"); }

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

    // -----------------------------------------------------------------------
    // Sidebar UI helpers — small functions that build a single dimension row
    // (label + numeric field + unit) or section header. Pulled out of the
    // CreateXxxContent helpers because each card needs ~3-5 of these and
    // the inline copies in MainFrame.cpp are repetitive.
    //
    // The card structure each helper expects:
    //   * card panel (CardBg) holds a wxBoxSizer sized vertically
    //   * each "row" is a horizontal sizer added at LEFT/RIGHT/TOP padding 10
    // No m_mmUnitLabels equivalent — the fixture editor doesn't carry
    // metric/imperial switching, so unit labels are static text and never
    // need to be tracked for runtime updates.
    // -----------------------------------------------------------------------

    // Append [label .... field unit] to `parentSz`, with `field` set to a
    // fresh wxTextCtrl carrying `defVal`. `unitStr` is rendered after the
    // field (e.g. "mm" or "°").
    void AddDimRow(wxWindow* parent, wxSizer* parentSz,
        const wxString& label, wxTextCtrl*& outField,
        const wxString& defVal, const wxString& unitStr)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(parent, wxID_ANY, label);
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(kFieldLabelFont);

        outField = new wxTextCtrl(parent, wxID_ANY, defVal,
            wxDefaultPosition, wxSize(kFieldWidth, 22));
        outField->SetBackgroundColour(Style::BtnSmall);
        outField->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(parent, wxID_ANY, unitStr);
        unit->SetForegroundColour(Style::TextSubtle);
        unit->SetFont(kFieldLabelFont);
        unit->SetMinSize(wxSize(kUnitWidth, -1));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(outField, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        parentSz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // Append [label ............ choice] to `parentSz`, with `outChoice`
    // set to a fresh wxChoice populated with `options` (selecting the
    // first by default). Reused for every card's "Type" dropdown.
    void AddTypeRow(wxWindow* parent, wxSizer* parentSz,
        const wxString& label,
        const std::vector<wxString>& options,
        wxChoice*& outChoice)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(parent, wxID_ANY, label);
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(kFieldLabelFont);

        outChoice = new wxChoice(parent, wxID_ANY,
            wxDefaultPosition, wxSize(kCtrlColWidth, -1));
        outChoice->SetBackgroundColour(Style::BtnSmall);
        outChoice->SetForegroundColour(Style::TextMuted);
        for (const auto& opt : options)
            outChoice->Append(opt);
        outChoice->SetSelection(0);

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(outChoice, 0, wxALIGN_CENTER_VERTICAL);
        parentSz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // Build and return a card panel pre-sized with title + spacing. The
    // caller adds further rows to the returned sizer (passed via outSizer).
    // CardBg matches MainFrame's feature cards.
    wxPanel* MakeCardWithTitle(wxWindow* parent, const wxString& title,
        wxSizer*& outSizer)
    {
        auto* card = new wxPanel(parent, wxID_ANY);
        card->SetBackgroundColour(Style::CardBg);

        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* titleLabel = new wxStaticText(card, wxID_ANY, title);
        titleLabel->SetForegroundColour(*wxWHITE);
        titleLabel->SetFont(kCardTitleFont);
        sizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
        sizer->AddSpacer(6);

        card->SetSizer(sizer);
        outSizer = sizer;
        return card;
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
FixtureEditor::FixtureEditor(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, "Fixture Editor",
        wxDefaultPosition, wxSize(1280, 720),
        wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
{
    SetBackgroundColour(kEditorBg);

    // Minimum size accommodates toolbar (160) + reasonable canvas (~240) +
    // sidebar (300). Vertical floor leaves enough room for the ribbon's
    // two import rows + at least a couple of feature cards before the
    // sidebar starts scrolling.
    SetMinSize(wxSize(1000, 560));

    BuildUI();

    // Bindings — the toggle buttons go through wxEVT_TOGGLEBUTTON because
    // the makeToolBtn helper synthesizes that event type for toggles, and
    // wxEVT_BUTTON for momentary actions (Center, Generate Fixture) and
    // the Import buttons.
    Bind(wxEVT_BUTTON, &FixtureEditor::OnImportModelA, this, ID_FE_ImportModelA);
    Bind(wxEVT_BUTTON, &FixtureEditor::OnImportModelB, this, ID_FE_ImportModelB);
    Bind(wxEVT_BUTTON, &FixtureEditor::OnGenerateFixture, this, ID_FE_GenerateFixture);
    Bind(wxEVT_BUTTON, &FixtureEditor::OnAddInjectionPoint, this, ID_FE_AddInjectionPoint);
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
    // Outer layout:
    //   1. top ribbon (Import Mould Half A / B + path labels +
    //                  Generate Fixture button)
    //   2. content row: toolbar | canvas | feature-defaults sidebar
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
    contentSizer->Add(BuildSidePanel(root), 0, wxEXPAND);
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
    // Layout (single horizontal sizer at the outer level):
    //   [vertical stack: Import Half A row, Import Half B row]   [Generate Fixture]
    //
    // The two import rows are 32px tall each plus padding; the Generate
    // Fixture button on the right matches the combined import-stack height
    // so it visually anchors that side of the ribbon.
    auto* ribbon = new wxPanel(parent, wxID_ANY);
    ribbon->SetBackgroundColour(kEditorBg);

    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // Inner vertical stack for the two Import-Half rows. Built first so the
    // buildRow lambda below has a target sizer to add into.
    auto* importsCol = new wxBoxSizer(wxVERTICAL);

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

            importsCol->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
            return lbl;
        };

    m_lblModelAPath = buildRow(ID_FE_ImportModelA, "Import Mould Half A");
    m_lblModelBPath = buildRow(ID_FE_ImportModelB, "Import Mould Half B");
    importsCol->AddSpacer(8);   // bottom padding so rows don't hug the separator

    // proportion=1 so the import column takes the available width and the
    // path labels can ellipsize against the leftover space (rather than
    // collapsing to text-width and pulling Generate Fixture inwards).
    outerSizer->Add(importsCol, 1, wxEXPAND);

    // ---- Generate Fixture — primary action on the right --------------------
    // Same Style::BtnPlace indigo MainFrame uses for its primary "Place"
    // actions, so the button reads as the editor's headline action without
    // any extra styling. Sized to roughly match the combined height of the
    // two import rows above (32 + 32 + ~16 padding ≈ 80) so it visually
    // anchors the ribbon's right side.
    //
    // Handler is a stub for now — see OnGenerateFixture below for the wiring
    // intent.
    auto* btnGenerate = new wxButton(ribbon, ID_FE_GenerateFixture,
        "Generate Fixture", wxDefaultPosition, wxSize(160, 64), wxBORDER_NONE);
    btnGenerate->SetBackgroundColour(Style::BtnPlace);
    btnGenerate->SetForegroundColour(*wxWHITE);
    btnGenerate->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    outerSizer->Add(btnGenerate, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 12);

    ribbon->SetSizer(outerSizer);
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
// Side panel — feature defaults
//
// Right-hand column carrying the per-feature default settings that get
// baked into the saved .fixture file. Layout mirrors MainFrame's left
// panel structure (header label + scrollable column of CardBg cards),
// but each card is stripped of MainFrame's placement chrome (Place
// button, Edit/Remove/Clear, collapsible Settings toggle) — fixture
// authoring isn't a placement context, so the entire card is just
// settings.
//
// Order follows the same convention as MainFrame: Sprues → Runners →
// Gates → Vents → Ejectors. Same default values, same field naming, so
// the future Generate-Fixture handler can read them back into a
// FixtureDefinition with no surprises.
// ---------------------------------------------------------------------------
wxWindow* FixtureEditor::BuildSidePanel(wxWindow* parent)
{
    // Outer container: content column + left border (the canvas sits to
    // the left of us, so the border lives on our left edge).
    auto* outer = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(kSidebarWidth, -1));
    outer->SetBackgroundColour(kEditorBg);

    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // Left-edge accent line — same 1px Divider treatment used between the
    // ribbon and content area, so the visual seams match.
    auto* borderLine = new wxPanel(outer, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    borderLine->SetBackgroundColour(Style::Divider);
    outerSizer->Add(borderLine, 0, wxEXPAND);

    auto* column = new wxPanel(outer, wxID_ANY);
    column->SetBackgroundColour(kEditorBg);
    auto* colSizer = new wxBoxSizer(wxVERTICAL);

    // Header — same styling as MainFrame's "MOULD TOOL SETTINGS" label.
    auto* title = new wxStaticText(column, wxID_ANY, "FEATURE DEFAULTS");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(kSectionHeaderFont);
    colSizer->Add(title, 0, wxLEFT | wxTOP, 12);
    colSizer->AddSpacer(8);

    // Scrollable card column — five feature cards likely overflow on a
    // ~720px-tall window, so wrap the list in a wxScrolledWindow with a
    // vertical scrollbar.
    auto* scrollWin = new wxScrolledWindow(column, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxVSCROLL | wxBORDER_NONE);
    scrollWin->SetScrollRate(0, 8);
    scrollWin->SetBackgroundColour(kEditorBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(4);

    // Card order matches MainFrame so users transitioning between the two
    // panels find features in the same place. The Injection Points card
    // leads — points are upstream of every per-feature default (sprues
    // feed from them, runners route between them), so it reads naturally
    // as the first thing the user configures after positioning the halves.
    sizer->Add(CreateInjectionPointsContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    sizer->Add(CreateSpruesContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    sizer->Add(CreateRunnersContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    sizer->Add(CreateGatesContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    sizer->Add(CreateVentsContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    sizer->Add(CreateEjectorsContent(scrollWin), 0, wxEXPAND | wxTOP, 8);

    sizer->AddSpacer(12);

    scrollWin->SetSizer(sizer);
    colSizer->Add(scrollWin, 1, wxEXPAND);

    column->SetSizer(colSizer);
    outerSizer->Add(column, 1, wxEXPAND);

    outer->SetSizer(outerSizer);
    return outer;
}

// ---------------------------------------------------------------------------
// Per-feature cards.
//
// Each card uses the AddTypeRow / AddDimRow helpers from the anon namespace
// at the top of this file — so the per-feature builders below only carry
// the fields and defaults specific to that feature. Defaults match
// MainFrame.cpp exactly: same numbers, same units, same wxChoice options.
// Distance fields are mm, angle fields are degrees.
// ---------------------------------------------------------------------------
wxPanel* FixtureEditor::CreateInjectionPointsContent(wxWindow* parent)
{
    // Injection-points card differs structurally from the per-feature
    // defaults below: instead of a fixed set of dimension fields, it
    // surfaces a dynamic list of user-added points plus an "Add" button
    // that opens InjectionPointDialog. Entries are rendered into
    // m_injectionListPanel by RebuildInjectionList — see that function
    // for the per-row layout.
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Injection Points", sizer);

    // List container — empty at construction; RebuildInjectionList
    // populates it after each Add / Edit / Remove.
    m_injectionListPanel = new wxPanel(card, wxID_ANY);
    m_injectionListPanel->SetBackgroundColour(Style::CardBg);
    m_injectionListPanel->SetSizer(new wxBoxSizer(wxVERTICAL));
    sizer->Add(m_injectionListPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // Add button — full-width inside the card, matching the visual weight
    // of MainFrame's Place button on placement-mode cards. ID is the
    // single static button on this card; Edit/Remove buttons inside the
    // list bind their own per-row lambdas (see RebuildInjectionList).
    auto* addBtn = new wxButton(card, ID_FE_AddInjectionPoint,
        "Add Injection Point",
        wxDefaultPosition, wxSize(-1, 26));
    addBtn->SetBackgroundColour(Style::BtnSmall);
    addBtn->SetForegroundColour(*wxWHITE);
    sizer->Add(addBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    sizer->AddSpacer(10);

    // Render the (empty) initial state. RebuildInjectionList draws a
    // muted "No injection points" placeholder when the vector is empty,
    // so the card doesn't collapse to a thin sliver before the user adds
    // anything.
    RebuildInjectionList();

    return card;
}

// ---------------------------------------------------------------------------
// RebuildInjectionList — repaint the list of injection points inside
// m_injectionListPanel from m_injectionPoints.
//
// Called after every Add / Edit / Remove. We tear down and rebuild the
// child windows entirely rather than trying to incrementally diff the
// list — the list is small (typically 1-5 entries) and rebuilding is
// trivially cheap, while diff logic would have to track per-row sub-
// panels and re-bind buttons on index shifts. The teardown also disposes
// of the per-row Edit/Remove button bindings cleanly: wxWindow children
// destroy their own bound event handlers.
//
// Layout per entry — two stacked rows inside a sub-panel:
//   row 1: [ label_text ........ ] [ Edit ] [ Remove ]
//   row 2:   ( x.x, y.y, z.z ) mm
// keeping the buttons on the busier first row and letting the coordinate
// readout sit on its own line so long labels don't get truncated.
//
// Empty state: a single muted "No injection points" line, so the card
// has visible content even before the first point is added.
// ---------------------------------------------------------------------------
void FixtureEditor::RebuildInjectionList()
{
    if (!m_injectionListPanel) return;

    m_injectionListPanel->DestroyChildren();
    auto* listSizer = m_injectionListPanel->GetSizer();
    listSizer->Clear(false);   // children already destroyed; just empty the sizer

    if (m_injectionPoints.empty())
    {
        auto* placeholder = new wxStaticText(m_injectionListPanel, wxID_ANY,
            "No injection points");
        placeholder->SetForegroundColour(Style::TextSubtle);
        placeholder->SetFont(kFieldLabelFont);
        listSizer->Add(placeholder, 0, wxTOP | wxBOTTOM, 4);
    }
    else
    {
        for (int i = 0; i < (int)m_injectionPoints.size(); ++i)
        {
            const InjectionPoint& ip = m_injectionPoints[i];

            // Per-entry sub-panel keeps the two rows visually grouped and
            // makes spacing trivial — vertical sizer with the two rows
            // stacked, an 8px bottom margin between entries.
            auto* entry = new wxPanel(m_injectionListPanel, wxID_ANY);
            entry->SetBackgroundColour(Style::CardBg);
            auto* entrySizer = new wxBoxSizer(wxVERTICAL);

            // Row 1: label text + Edit/Remove buttons
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);

                // Empty-label fallback so a row with no label still has
                // visible content (rather than just appearing blank).
                const wxString labelText = ip.label.empty()
                    ? wxString("(unnamed)")
                    : wxString(ip.label);
                auto* lblText = new wxStaticText(entry, wxID_ANY, labelText,
                    wxDefaultPosition, wxDefaultSize,
                    wxST_ELLIPSIZE_END);
                lblText->SetForegroundColour(*wxWHITE);
                lblText->SetFont(kFieldLabelFont);

                auto* editBtn = new wxButton(entry, wxID_ANY, "Edit",
                    wxDefaultPosition, wxSize(44, 22));
                editBtn->SetBackgroundColour(Style::BtnSmall);
                editBtn->SetForegroundColour(*wxWHITE);

                auto* removeBtn = new wxButton(entry, wxID_ANY, "Remove",
                    wxDefaultPosition, wxSize(56, 22));
                removeBtn->SetBackgroundColour(Style::BtnSmall);
                removeBtn->SetForegroundColour(*wxWHITE);

                // Per-button bindings via lambda. Capturing `i` by value
                // is intentional — `i` here is the index into
                // m_injectionPoints AT BUILD TIME, which equals the
                // current index for this row because the whole list is
                // rebuilt on every mutation. Capturing `this` lets the
                // lambdas dispatch back into the editor's own helpers.
                editBtn->Bind(wxEVT_BUTTON,
                    [this, i](wxCommandEvent&) { EditInjectionPointAt(i); });
                removeBtn->Bind(wxEVT_BUTTON,
                    [this, i](wxCommandEvent&) { RemoveInjectionPointAt(i); });

                row->Add(lblText, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                row->Add(editBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                row->Add(removeBtn, 0, wxALIGN_CENTER_VERTICAL);
                entrySizer->Add(row, 0, wxEXPAND);
            }

            // Row 2: coordinate readout. Single decimal is enough — the
            // user typed mm values that were almost certainly authored
            // to the millimetre or finer; %g would chop trailing zeros
            // (10 → "10") and lose the unit feel, so %.1f for a stable
            // readable column.
            {
                const wxString coords = wxString::Format(
                    "(%.1f, %.1f, %.1f) mm", ip.x, ip.y, ip.z);
                auto* coordText = new wxStaticText(entry, wxID_ANY, coords);
                coordText->SetForegroundColour(Style::TextSubtle);
                coordText->SetFont(kFieldLabelFont);
                entrySizer->Add(coordText, 0, wxTOP, 2);
            }

            entry->SetSizer(entrySizer);
            listSizer->Add(entry, 0, wxEXPAND | wxBOTTOM, 8);
        }
    }

    // Re-layout from the list panel up to the scrolled window's virtual
    // size — without this, newly-added rows render with stale geometry
    // until the next external Layout() trigger (e.g. window resize).
    m_injectionListPanel->Layout();
    if (auto* parent = m_injectionListPanel->GetParent())
    {
        parent->Layout();
        // Walk up to the wxScrolledWindow (sidebar's scrollable column)
        // and refresh its virtual size — otherwise the scrollbar doesn't
        // appear when the list grows past the visible area.
        for (wxWindow* w = parent; w; w = w->GetParent())
        {
            if (auto* scr = wxDynamicCast(w, wxScrolledWindow))
            {
                scr->FitInside();
                break;
            }
        }
    }

    // Push the staged points to the canvas so it renders the purple
    // markers alongside the imported halves. Cheap on every list mutation
    // — the canvas just stores the vector and walks it per-frame.
    if (m_canvas)
        m_canvas->SetInjectionPoints(m_injectionPoints);
}

// ---------------------------------------------------------------------------
// Add / Edit / Remove handlers.
//
// Add opens InjectionPointDialog with empty defaults; on OK, appends a
// new point with type derived from Y via InjectionPoint::TypeFor — points
// on the parting plane (y=0) become Radial, anything else becomes Axial.
// Edit pre-fills the dialog from the existing point and replaces in
// place, re-deriving the type from the (possibly edited) Y. The fixture
// file's `type` field is therefore always consistent with `y` for every
// point this editor produces; older or hand-authored files get
// normalised on load by FixtureFile::Load applying the same rule.
// Remove just drops the entry and rebuilds.
// ---------------------------------------------------------------------------
void FixtureEditor::OnAddInjectionPoint(wxCommandEvent&)
{
    InjectionPointDialog dlg(this, "Add Injection Point");
    if (dlg.ShowModal() != wxID_OK) return;

    const InjectionPointValues vals = dlg.GetValues();
    InjectionPoint ip;
    ip.label = vals.label;
    ip.x = vals.x;
    ip.y = vals.y;
    ip.z = vals.z;
    ip.type = InjectionPoint::TypeFor(ip.y);
    m_injectionPoints.push_back(std::move(ip));

    RebuildInjectionList();
}

void FixtureEditor::EditInjectionPointAt(int index)
{
    if (index < 0 || index >= (int)m_injectionPoints.size()) return;

    const InjectionPoint& existing = m_injectionPoints[index];
    InjectionPointValues initial;
    initial.label = existing.label;
    initial.x = existing.x;
    initial.y = existing.y;
    initial.z = existing.z;

    InjectionPointDialog dlg(this, "Edit Injection Point", initial);
    if (dlg.ShowModal() != wxID_OK) return;

    const InjectionPointValues vals = dlg.GetValues();
    m_injectionPoints[index].label = vals.label;
    m_injectionPoints[index].x = vals.x;
    m_injectionPoints[index].y = vals.y;
    m_injectionPoints[index].z = vals.z;
    m_injectionPoints[index].type = InjectionPoint::TypeFor(vals.y);

    RebuildInjectionList();
}

void FixtureEditor::RemoveInjectionPointAt(int index)
{
    if (index < 0 || index >= (int)m_injectionPoints.size()) return;
    m_injectionPoints.erase(m_injectionPoints.begin() + index);
    RebuildInjectionList();
}

wxPanel* FixtureEditor::CreateSpruesContent(wxWindow* parent)
{
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Sprues", sizer);

    AddTypeRow(card, sizer, "Sprue type:", { "Cylinder" }, m_sprueTypeChoice);

    AddDimRow(card, sizer, "Diameter:", m_sprueDiameter, "5.0", "mm");
    AddDimRow(card, sizer, "Draft angle:", m_sprueDraftAngle, "1.0", DegSym());
    AddDimRow(card, sizer, "Cold slug:", m_sprueColdSlugDepth, "5.0", "mm");
    AddDimRow(card, sizer, "Sprue length:", m_sprueLength, "20.0", "mm");

    sizer->AddSpacer(10);   // matches the wxBOTTOM=10 margin on MainFrame's
    // dimsPanel — keeps card heights consistent.
    return card;
}

wxPanel* FixtureEditor::CreateRunnersContent(wxWindow* parent)
{
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Runners", sizer);

    AddTypeRow(card, sizer, "Runner type:", { "Cylindrical" }, m_runnerTypeChoice);

    AddDimRow(card, sizer, "Diameter:", m_runnerDiameter, "4.0", "mm");
    AddDimRow(card, sizer, "Cold slug length:", m_runnerColdSlugDepth, "5.0", "mm");

    sizer->AddSpacer(10);
    return card;
}

wxPanel* FixtureEditor::CreateGatesContent(wxWindow* parent)
{
    // Gate card uniquely covers two related defaults: the gate itself and
    // the sub-runner that feeds it. MainFrame puts both in the same card
    // because they share parameters at place-time; we mirror that grouping
    // here so the file-format mapping is the same. A 1-px Divider line
    // separates the two halves visually, same as MainFrame.
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Gates", sizer);

    AddTypeRow(card, sizer, "Gate type:", { "Tapered Cylinder" }, m_gateTypeChoice);

    AddDimRow(card, sizer, "Diameter:", m_gateDiameter, "3.0", "mm");
    AddDimRow(card, sizer, "Draft angle:", m_gateDraftAngle, "1.0", DegSym());

    sizer->AddSpacer(6);
    auto* subSep = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    subSep->SetBackgroundColour(Style::Divider);
    sizer->Add(subSep, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    AddTypeRow(card, sizer, "Sub-runner type:", { "Cylinder" }, m_subRunnerTypeChoice);
    AddDimRow(card, sizer, "Diameter:", m_subRunnerDiameter, "5.0", "mm");

    sizer->AddSpacer(10);
    return card;
}

wxPanel* FixtureEditor::CreateVentsContent(wxWindow* parent)
{
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Vents", sizer);

    AddTypeRow(card, sizer, "Vent type:", { "Rectangular" }, m_ventTypeChoice);

    AddDimRow(card, sizer, "Length:", m_ventLength, "1.0", "mm");
    AddDimRow(card, sizer, "Width:", m_ventWidth, "2.0", "mm");
    AddDimRow(card, sizer, "Overrun (start):", m_ventOverrunStart, "0.5", "mm");
    AddDimRow(card, sizer, "Overrun (end):", m_ventOverrunEnd, "0.5", "mm");

    sizer->AddSpacer(10);
    return card;
}

wxPanel* FixtureEditor::CreateEjectorsContent(wxWindow* parent)
{
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Ejectors", sizer);

    AddTypeRow(card, sizer, "Ejector type:", { "Cylindrical" }, m_ejectorTypeChoice);

    AddDimRow(card, sizer, "Diameter:", m_ejectorDiameter, "3.0", "mm");
    AddDimRow(card, sizer, "Length:", m_ejectorLength, "25.0", "mm");

    sizer->AddSpacer(10);
    return card;
}

// ---------------------------------------------------------------------------
// Active-tool routing
//
// Drives toolbar visuals AND the canvas's transform mode in lock-step. The
// only persistent canvas mode the fixture editor uses is AlignFace; every
// other active-tool ID corresponds to either a dialog tool (Move/Rotate/
// Scale) that immediately untoggles itself, a momentary action (Center),
// or wxID_NONE (post-ESC, post-dialog). All of those want the canvas back
// in Select. Centralising the mapping here keeps the two state machines
// from drifting out of sync regardless of how SetActiveTool got called —
// button click, dialog handler clearing toggles, or the canvas's ESC
// callback.
// ---------------------------------------------------------------------------
void FixtureEditor::SetActiveTool(int activeId)
{
    m_activeToolId = activeId;
    for (auto& kv : m_toolBtnSetters)
        kv.second(kv.first == activeId);

    if (m_canvas)
    {
        m_canvas->SetTransformMode(activeId == ID_FE_AlignFace
            ? FixtureCanvas::TransformMode::AlignFace
            : FixtureCanvas::TransformMode::Select);
    }
}

// ---------------------------------------------------------------------------
// Tool button handlers — wired through to the canvas.
//
// Move / Rotate / Scale follow MainFrame's dialog-based convention exactly:
// the toggle latches on briefly when clicked, the handler immediately
// untoggles it via SetActiveTool(wxID_NONE) (which also brings the canvas
// back to Select if it was in AlignFace), then the matching dialog opens.
// On OK, the result is forwarded to the canvas; if no half is selected,
// the canvas's Apply* methods no-op silently — same behaviour as
// MainFrame's HasSelection guard.
//
// Center is a momentary action — never latches, never affects the active-
// tool state, mirrors MainFrame::OnToolCenter.
//
// AlignFace is the only true persistent toggle. Routing through
// SetActiveTool keeps the toolbar visual and the canvas mode in sync
// (and ESC on the canvas side calls back into SetActiveTool to clear the
// toggle when the user bails out).
// ---------------------------------------------------------------------------
void FixtureEditor::OnToolMove(wxCommandEvent&)
{
    SetActiveTool(wxID_NONE);

    if (!m_canvas) return;

    TranslateDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
        return;

    const TranslateValues v = dlg.GetValues();
    m_canvas->ApplyTranslation(v.x, v.y, v.z);
}

void FixtureEditor::OnToolRotate(wxCommandEvent&)
{
    SetActiveTool(wxID_NONE);

    if (!m_canvas) return;

    RotateDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
        return;

    const RotateValues v = dlg.GetValues();
    m_canvas->ApplyRotation(v.x, v.y, v.z);
}

void FixtureEditor::OnToolScale(wxCommandEvent&)
{
    SetActiveTool(wxID_NONE);

    if (!m_canvas) return;

    ScaleDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
        return;

    const ScaleValues v = dlg.GetValues();
    m_canvas->ApplyScale(v.uniform);
}

void FixtureEditor::OnToolCenter(wxCommandEvent&)
{
    // Momentary action — does not affect the active-tool state. Intentional
    // mirror of MainFrame::OnToolCenter, which immediately re-centers the
    // selected object without leaving any tool latched. No-op silently
    // when no half is selected.
    if (!m_canvas) return;
    if (!m_canvas->HasSelection()) return;
    m_canvas->CenterSelected();
}

void FixtureEditor::OnToolAlignFace(wxCommandEvent& evt)
{
    // True persistent toggle. The wxEVT_TOGGLEBUTTON event's Int() carries
    // the new toggle state from makeToolBtn — 1 for ON, 0 for OFF —
    // letting us drive the canvas's mode without having to inspect the
    // button visual directly. SetActiveTool also brings the canvas's
    // TransformMode in sync, so we don't call SetTransformMode here.
    SetActiveTool(evt.GetInt() ? ID_FE_AlignFace : wxID_NONE);
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

// ---------------------------------------------------------------------------
// Generate Fixture — collect editor state into a FixtureDefinition and
// write it via FixtureFile::Save.
//
// Validation (in order, abort on first failure with a MessageBox):
//   1. Both modelA and modelB paths must be set (user clicked both Import
//      buttons).
//   2. Both halves must report valid=true from FixtureCanvas::GetHalfPose
//      — i.e. the imports actually loaded successfully. A path that's
//      set but never made it onto the canvas (load error swallowed by
//      LoadHalf) would otherwise produce a fixture pointing at a half
//      that doesn't exist as far as this editor knows.
//
// Defaults are read from every sidebar field unconditionally — the
// editor's UI always has a value for each field, so we always emit every
// defaults section, even when the user accepted the hardcoded defaults.
// FixtureFile::Save's "skip empty section" guard still kicks in for the
// per-half transform sections (those skip on identity).
//
// On success, closes the editor. The parent StartupDialog doesn't yet
// auto-rescan to surface the new file in its list — that's a separate
// thread to pull when the editor's lifecycle communicates with the
// StartupDialog more deeply. For now the user re-opens the dialog or
// hits Browse Folder to see the new fixture.
// ---------------------------------------------------------------------------
void FixtureEditor::OnGenerateFixture(wxCommandEvent&)
{
    // ---- Validation -------------------------------------------------------
    if (m_modelAPath.empty() || m_modelBPath.empty())
    {
        wxMessageBox("Both Mould Half A and Mould Half B must be imported "
            "before generating a fixture.",
            "Generate Fixture", wxOK | wxICON_WARNING, this);
        return;
    }

    if (!m_canvas)
    {
        // Defensive — m_canvas is constructed in BuildUI and never reassigned,
        // so this branch is effectively dead. Kept for safety since the
        // validation message is more useful than a crash.
        wxMessageBox("Internal error: 3D viewport is unavailable.",
            "Generate Fixture", wxOK | wxICON_ERROR, this);
        return;
    }

    const auto poseA = m_canvas->GetHalfPose(FixtureCanvas::HalfSlot::A);
    const auto poseB = m_canvas->GetHalfPose(FixtureCanvas::HalfSlot::B);
    if (!poseA.valid || !poseB.valid)
    {
        wxMessageBox("Both halves must finish loading into the viewport "
            "before generating a fixture. Try re-importing any half whose "
            "load may have failed.",
            "Generate Fixture", wxOK | wxICON_WARNING, this);
        return;
    }

    // ---- Injection points required ----------------------------------------
    // A fixture with no injection points has nothing for the main app's
    // sprue placement (and downstream runner / gate / vent flow) to attach
    // to — it'd load successfully but be unusable. Catch the omission here
    // rather than letting the user discover it on next session by clicking
    // "Place Sprue" and finding nothing to choose from.
    if (m_injectionPoints.empty())
    {
        wxMessageBox("Add at least one injection point before generating "
            "the fixture. Use the \"Add Injection Point\" button on the "
            "Injection Points card to author one.",
            "Generate Fixture", wxOK | wxICON_WARNING, this);
        return;
    }

    // ---- Save target ------------------------------------------------------
    // Default to the same fixtures/ folder StartupDialog scans on launch,
    // so a freshly-written fixture appears in the dialog's list without
    // the user having to browse to a different folder. wxStandardPaths
    // walks back from the running executable, matching how StartupDialog
    // computes its m_fixturesFolder.
    namespace fs = std::filesystem;
    const std::string defaultDir = (fs::path(wxStandardPaths::Get()
        .GetExecutablePath().ToStdString()).parent_path() / "fixtures").string();

    // wxString implicit conversion from std::string uses the current locale,
    // which matches how StartupDialog feeds m_fixturesFolder into wxControls.
    // No explicit FromUTF8 here — that'd misinterpret legacy-ANSI paths on
    // Windows installations that haven't opted into the UTF-8 codepage.
    wxFileDialog dlg(this, "Save Fixture",
        defaultDir, "NewFixture.fixture",
        "Fixture files (*.fixture)|*.fixture",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;   // user cancelled

    const std::string outPath = dlg.GetPath().ToStdString();

    // ---- Build FixtureDefinition ------------------------------------------
    FixtureDefinition def;
    def.modelAPath = m_modelAPath;
    def.modelBPath = m_modelBPath;
    def.fixturePath = outPath;

    // Half transforms. The canvas internally calls these yawDeg / pitchDeg /
    // rollDeg matching its YXZ Euler convention (yaw=Y, pitch=X, roll=Z).
    // The file format axes are world X/Y/Z, so we map across:
    //   pitchDeg → rotation_x   (rotation around world X)
    //   yawDeg   → rotation_y   (rotation around world Y)
    //   rollDeg  → rotation_z   (rotation around world Z)
    auto poseToTransform = [](const FixtureCanvas::HalfPose& p)
        {
            HalfTransform t;
            t.posX = p.pos.x;
            t.posY = p.pos.y;
            t.posZ = p.pos.z;
            t.rotX = p.pitchDeg;
            t.rotY = p.yawDeg;
            t.rotZ = p.rollDeg;
            t.scale = p.scale;
            return t;
        };
    def.halfATransform = poseToTransform(poseA);
    def.halfBTransform = poseToTransform(poseB);

    // ---- Defaults from sidebar fields -------------------------------------
    // wxTextCtrl::GetValue + ToDouble for numbers; ToDouble returns false
    // on parse failure (e.g. user typed "abc"). We leave the optional
    // unset on failure so that field falls back to the application
    // default at load time — same forgiving stance MainFrame uses.
    auto readField = [](wxTextCtrl* ctrl, std::optional<float>& dst)
        {
            if (!ctrl) return;
            double v = 0.0;
            if (ctrl->GetValue().ToDouble(&v))
                dst = static_cast<float>(v);
        };
    auto readChoice = [](wxChoice* ch, std::optional<std::string>& dst)
        {
            if (!ch) return;
            const wxString sel = ch->GetStringSelection();
            if (!sel.IsEmpty())
                dst = sel.ToStdString();
        };

    // Sprues
    readChoice(m_sprueTypeChoice, def.sprueDefaults.type);
    readField(m_sprueDiameter, def.sprueDefaults.diameter);
    readField(m_sprueDraftAngle, def.sprueDefaults.draftAngle);
    readField(m_sprueColdSlugDepth, def.sprueDefaults.coldSlugLength);
    readField(m_sprueLength, def.sprueDefaults.length);

    // Runners
    readChoice(m_runnerTypeChoice, def.runnerDefaults.type);
    readField(m_runnerDiameter, def.runnerDefaults.diameter);
    readField(m_runnerColdSlugDepth, def.runnerDefaults.coldSlugLength);

    // Gates + sub-runner — both feed off the gate card.
    readChoice(m_gateTypeChoice, def.gateDefaults.type);
    readField(m_gateDiameter, def.gateDefaults.diameter);
    readField(m_gateDraftAngle, def.gateDefaults.draftAngle);
    readChoice(m_subRunnerTypeChoice, def.subRunnerDefaults.type);
    readField(m_subRunnerDiameter, def.subRunnerDefaults.diameter);

    // Vents
    readChoice(m_ventTypeChoice, def.ventDefaults.type);
    readField(m_ventLength, def.ventDefaults.length);
    readField(m_ventWidth, def.ventDefaults.width);
    readField(m_ventOverrunStart, def.ventDefaults.overrunStart);
    readField(m_ventOverrunEnd, def.ventDefaults.overrunEnd);

    // Ejectors
    readChoice(m_ejectorTypeChoice, def.ejectorDefaults.type);
    readField(m_ejectorDiameter, def.ejectorDefaults.diameter);
    readField(m_ejectorLength, def.ejectorDefaults.length);

    // Injection points — straight copy of the editor's live list. Order
    // is preserved so the saved file's [injection_point.0..N] sections
    // come out in the same order the user added them in the sidebar.
    def.injectionPoints = m_injectionPoints;

    // ---- Write ------------------------------------------------------------
    std::string error;
    if (!FixtureFile::Save(outPath, def, error))
    {
        // Implicit std::string → wxString matches StartupDialog and MainFrame's
        // convention for displaying file-IO errors (locale-aware, not forced
        // UTF-8) — error strings from FixtureFile::Save embed the OS-encoded
        // path, so forcing UTF-8 would misinterpret legacy-ANSI characters.
        wxMessageBox(wxString("Failed to save fixture file:\n\n") + error,
            "Generate Fixture", wxOK | wxICON_ERROR, this);
        return;
    }

    // Success — close the editor. wxEVT_CLOSE_WINDOW handler calls
    // Destroy(), so the frame goes away cleanly and the parent
    // StartupDialog regains focus.
    Close();
}
