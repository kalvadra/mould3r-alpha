#include "FixtureEditor.h"

#include <wx/bmpbndl.h>
#include <wx/dcbuffer.h>  // wxAutoBufferedPaintDC — flicker-free repaint on hover/toggle for tool buttons
#include <wx/file.h>
#include <wx/filedlg.h>  // wxFileDialog — Import buttons + Generate Fixture save
#include <wx/filename.h>
#include <wx/graphics.h> // wxGraphicsContext — rounded-rect paint for icon tool buttons
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
#include "GridSettingsDialog.h" // Grid Defaults card "Edit…" button
#include "RoundedButton.h"     // owner-drawn rounded button — replaces wxButton in custom-themed UI
#include "WindowEffects.h"     // DWM corner rounding for the editor frame
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
    // outUnitLabel (optional): if non-null, receives the unit wxStaticText*
    // so the caller can register it in m_mmUnitLabels for mm <-> in swaps.
    void AddDimRow(wxWindow* parent, wxSizer* parentSz,
        const wxString& label, wxTextCtrl*& outField,
        const wxString& defVal, const wxString& unitStr,
        wxStaticText** outUnitLabel = nullptr)
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

        if (outUnitLabel) *outUnitLabel = unit;
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
    Bind(wxEVT_BUTTON, &FixtureEditor::OnSelectModelA, this, ID_FE_SelectModelA);
    Bind(wxEVT_BUTTON, &FixtureEditor::OnSelectModelB, this, ID_FE_SelectModelB);
    Bind(wxEVT_CHECKBOX, &FixtureEditor::OnHideHalfA, this, ID_FE_HideA);
    Bind(wxEVT_CHECKBOX, &FixtureEditor::OnHideHalfB, this, ID_FE_HideB);
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

    // Round the frame's outer corners via DWM on Win11. wxFrame's
    // system title bar comes along for the ride (it's drawn by the
    // compositor, not by us), so the result looks consistent with
    // native Win11 windows.
    WindowEffects::ApplyRoundedCorners(this);
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
    //   [Cell A vertical]  [spacer]  [Cell B vertical]   [50px gap]   [Save Fixture]
    //
    // Each cell stacks vertically:
    //   MOULD HALF A
    //   [........ path field ........]  [Select]  [☐ Hide]
    //
    // Label sits above the field/Select/Hide row so the label can be wide
    // without eating into the field's working width. Label font is small
    // (7pt) and uppercase to keep the two-row cell visually compact.
    auto* ribbon = new wxPanel(parent, wxID_ANY);
    ribbon->SetBackgroundColour(kEditorBg);

    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // Single horizontal row that holds both cells side by side.
    auto* pickerRow = new wxBoxSizer(wxHORIZONTAL);

    // Label dropped from 9pt to 7pt to keep the two-row cell compact —
    // the label is short and bold so legibility holds. Field stays at 9pt
    // because the path is what the user reads; Select button keeps its
    // 9pt semibold to match the rest of the app's primary actions.
    static const wxFont kLabelFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI");
    static const wxFont kFieldFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI");
    static const wxFont kSelectFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI");
    static const wxFont kHideFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI");

    // Builds one vertical cell:
    //   row 1: [label]
    //   row 2: [path field (expand)] [Select] [Hide]
    // and appends it to pickerRow. Returns the text field; the checkbox
    // is captured via an out-param. Hide sits at the end of the field
    // row as a row-level affordance — keeps field+Select tightly grouped
    // as the load action, with the visibility toggle as a separate concern.
    auto buildCell = [&](int selectId, int hideId, const wxString& rowLabel,
        wxCheckBox*& outHideCheck) -> wxTextCtrl*
        {
            auto* cellCol = new wxBoxSizer(wxVERTICAL);

            auto* lbl = new wxStaticText(ribbon, wxID_ANY, rowLabel);
            lbl->SetForegroundColour(Style::TextPrimary);
            lbl->SetFont(kLabelFont);
            cellCol->Add(lbl, 0, wxBOTTOM | wxLEFT, 2);

            // Inner horizontal row for the actual controls.
            auto* fieldRow = new wxBoxSizer(wxHORIZONTAL);

            auto* field = new wxTextCtrl(ribbon, wxID_ANY, wxEmptyString,
                wxDefaultPosition, wxSize(-1, 28), wxBORDER_NONE);
            field->SetBackgroundColour(Style::InputBg);
            field->SetForegroundColour(Style::TextPrimary);
            field->SetFont(kFieldFont);

            auto* btnSelect = new RoundedButton(ribbon, selectId, "...",
                wxDefaultPosition, wxSize(36, 28), wxBORDER_NONE);
            btnSelect->SetBackgroundColour(Style::BtnSecondary);
            btnSelect->SetForegroundColour(*wxWHITE);
            btnSelect->SetFont(kSelectFont);

            // wxCheckBox uses native Windows theming for the check glyph
            // itself; we can only colour the surrounding label and bg.
            // Font dropped to 8pt — same family-of-text-shrinking as the
            // section label, keeps the row visually balanced.
            auto* hide = new wxCheckBox(ribbon, hideId, "Hide");
            hide->SetBackgroundColour(kEditorBg);
            hide->SetForegroundColour(Style::TextPrimary);
            hide->SetFont(kHideFont);
            outHideCheck = hide;

            // proportion=1 on the field so both cells' fields fill the
            // available width minus Select+Hide. Select (36px ellipsis
            // button — was "Select" at 70px; shortened to reclaim ribbon
            // space) and Hide take their intrinsic widths.
            fieldRow->Add(field, 1, wxALIGN_CENTER_VERTICAL);
            fieldRow->Add(btnSelect, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
            fieldRow->Add(hide, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);

            cellCol->Add(fieldRow, 0, wxEXPAND);

            // proportion=1 on the cell so cell A and cell B share the
            // available horizontal space equally.
            pickerRow->Add(cellCol, 1, wxALIGN_CENTER_VERTICAL);
            return field;
        };

    m_pathACtrl = buildCell(ID_FE_SelectModelA, ID_FE_HideA, "MOULD HALF A", m_hideACheck);
    pickerRow->AddSpacer(16);   // gap between the two cells
    m_pathBCtrl = buildCell(ID_FE_SelectModelB, ID_FE_HideB, "MOULD HALF B", m_hideBCheck);

    // Outer: pickerRow takes the bulk, fixed 100px gap, then Save Fixture
    // on the right. wxRIGHT dropped from pickerRow and wxLEFT from btnWrap
    // so the literal 100 in the code is exactly the gap between the two
    // regions — easier to tune than juggling three contributing values.
    outerSizer->Add(pickerRow, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    outerSizer->AddSpacer(100);

    // ---- Save Fixture — primary action on the right -----------------------
    // Wrapped in a vertical sizer with asymmetric top/bottom margins —
    // top stays at 5 (where it was), bottom grows to 10 so the ribbon
    // overall gains 5px of bottom padding (the button drives the ribbon
    // height since pickerRow is shorter, so the bottom spacer flows
    // through to the ribbon's bottom edge). Split into two Adds because
    // wxSizer borders take a single value for all flagged sides.
    auto* btnWrap = new wxBoxSizer(wxVERTICAL);
    auto* btnGenerate = new RoundedButton(ribbon, ID_FE_GenerateFixture,
        "Save Fixture", wxDefaultPosition, wxSize(160, 32), wxBORDER_NONE);
    btnGenerate->SetBackgroundColour(Style::BtnGenerate);
    btnGenerate->SetForegroundColour(*wxWHITE);
    btnGenerate->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    btnWrap->Add(btnGenerate, 0, wxTOP, 5);
    btnWrap->AddSpacer(10);
    outerSizer->Add(btnWrap, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    ribbon->SetSizer(outerSizer);
    return ribbon;
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
wxWindow* FixtureEditor::BuildToolbar(wxWindow* parent)
{
    // Left-hand panel: tool buttons on top, feature-defaults cards below.
    // Width matches the former right sidebar so the total editor width is
    // unchanged — we've just consolidated both columns into one.
    auto* toolbar = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(kSidebarWidth, -1));
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

            // ---- Rounded-corner repaint ---------------------------------
            // The panel paints itself: parent bg fills the whole client
            // area first (so the four corner triangles outside the rounded
            // shape pick up the toolbar's colour), then a filled rounded
            // rectangle in the panel's *current* bg colour covers the
            // rest. applyColours below mutates panel->SetBackgroundColour
            // and calls Refresh(), so the existing hover / selected /
            // idle state machine drives the paint with no extra wiring.
            //
            // wxBG_STYLE_PAINT promises wxWidgets we'll fill the client
            // area ourselves — required when pairing with wxAutoBuffered-
            // PaintDC, otherwise the default erase pass fights the buffer
            // and the result flickers on hover.
            //
            // Kept in lockstep with the same block in MainFrame.cpp's
            // makeToolBtn — the two helpers are parallel by design, see
            // the namespace-level comment at the top of this file. The
            // 4 px radius matches RoundedButton's default so the icon
            // tool buttons and the text-only rounded buttons share a
            // corner profile across the app.
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
                // Phantom-leave guard: txt and bmpCtrl are real child
                // windows of the panel, so the panel fires LEAVE as
                // soon as the cursor crosses onto either one — even
                // though, from the user's point of view, the cursor is
                // still very much on the button. The corollary ENTER
                // on the child does fire, but the relative ordering
                // between the two isn't guaranteed on Windows and we
                // were getting a stuck-off hover from the race.
                //
                // Fix: hit-test the cursor in screen coords against the
                // panel's screen rect. If it's still anywhere over the
                // composite, the leave is phantom — suppress it. A
                // genuine leave (cursor truly off the button) lands
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

    // ---- FIXTURE TOOLS — Move/Rotate/Scale/Center in a 2x2 grid -----------
    // wxGridSizer gives equal-width cells without manual proportion math.
    auto* grid = new wxGridSizer(2, 2, 4, 4);
    grid->Add(makeToolBtn(ID_FE_Move, "Move", true, kIconMove), 1, wxEXPAND);
    grid->Add(makeToolBtn(ID_FE_Rotate, "Rotate", true, kIconRotate), 1, wxEXPAND);
    grid->Add(makeToolBtn(ID_FE_Scale, "Scale", true, kIconScale), 1, wxEXPAND);
    grid->Add(makeToolBtn(ID_FE_Center, "Center", false, kIconCenter), 1, wxEXPAND);
    sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- Divider + FIXTURE ALIGNMENT section ------------------------------
    sizer->AddSpacer(8);
    auto* alignDivider = new wxPanel(toolbar, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 1));
    alignDivider->SetBackgroundColour(Style::Divider);
    sizer->Add(alignDivider, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* alignHeader = new wxStaticText(toolbar, wxID_ANY, "FIXTURE ALIGNMENT");
    alignHeader->SetForegroundColour(Style::TextPrimary);
    alignHeader->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(alignHeader, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(8);

    sizer->Add(makeToolBtn(ID_FE_AlignFace, "Align Face", true, kIconAlignFace),
        0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // ---- Divider between tool buttons and fixture defaults ----------------
    sizer->AddSpacer(8);
    auto* divider = new wxPanel(toolbar, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 1));
    divider->SetBackgroundColour(Style::Divider);
    sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(4);

    // ---- Fixture defaults header ------------------------------------------
    auto* defaultsHeader = new wxStaticText(toolbar, wxID_ANY, "FIXTURE DEFAULTS");
    defaultsHeader->SetForegroundColour(Style::TextPrimary);
    defaultsHeader->SetFont(kSectionHeaderFont);
    sizer->Add(defaultsHeader, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(8);

    // ---- Scrollable feature cards -----------------------------------------
    auto* scrollWin = new wxScrolledWindow(toolbar, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxVSCROLL | wxBORDER_NONE);
    scrollWin->SetScrollRate(0, 8);
    scrollWin->SetBackgroundColour(kEditorBg);

    auto* cardSizer = new wxBoxSizer(wxVERTICAL);
    cardSizer->AddSpacer(4);
    // Unit toggle sits above the injection-points card so it reads as a
    // global setting for all the defaults below it. Margins match the
    // left/right inset used by the fixture-tool buttons above.
    cardSizer->Add(CreateUnitToggle(scrollWin), 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    cardSizer->Add(CreateInjectionPointsContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    cardSizer->Add(CreateSpruesContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    cardSizer->Add(CreateRunnersContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    cardSizer->Add(CreateGatesContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    cardSizer->Add(CreateVentsContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    cardSizer->Add(CreateEjectorsContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    cardSizer->Add(CreateGridDefaultsContent(scrollWin), 0, wxEXPAND | wxTOP, 8);
    cardSizer->AddSpacer(12);
    scrollWin->SetSizer(cardSizer);
    scrollWin->FitInside();

    sizer->Add(scrollWin, 1, wxEXPAND);

    toolbar->SetSizer(sizer);
    return toolbar;
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------
wxWindow* FixtureEditor::BuildCanvasArea(wxWindow* parent)
{
    // Wrapper panel so we can stack a title ribbon above the GL viewport
    // without disturbing the outer content-row sizer proportions.
    auto* container = new wxPanel(parent, wxID_ANY);
    container->SetBackgroundColour(kEditorBg);

    auto* vSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Title ribbon ------------------------------------------------------
    auto* titleBar = new wxPanel(container, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 32));
    titleBar->SetBackgroundColour(Style::BtnSecondary);

    auto* titleSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* titleLbl = new wxStaticText(titleBar, wxID_ANY, "FIXTURE EDITOR",
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    titleLbl->SetForegroundColour(*wxWHITE);
    titleLbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    titleSizer->AddStretchSpacer(1);
    titleSizer->Add(titleLbl, 0, wxALIGN_CENTER_VERTICAL);
    titleSizer->AddStretchSpacer(1);
    titleBar->SetSizer(titleSizer);

    vSizer->Add(titleBar, 0, wxEXPAND);

    // ---- GL viewport -------------------------------------------------------
    // Real GL viewport. The canvas owns its own context, camera, and grid
    // renderer — see FixtureCanvas.h for the rationale on why this isn't
    // the main GLCanvas. Stored in m_canvas so the import handlers and
    // toolbar wiring can push state into it directly.
    m_canvas = new FixtureCanvas(container);
    vSizer->Add(m_canvas, 1, wxEXPAND);

    container->SetSizer(vSizer);
    return container;
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
    // Feature defaults have been consolidated into the left-hand toolbar
    // (BuildToolbar). This function is retained to satisfy the declaration
    // in FixtureEditor.h; it is no longer called from BuildUI.
    return new wxPanel(parent, wxID_ANY);
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
// ---------------------------------------------------------------------------
// Unit toggle
//
// A two-segment pill control (Metric | Imperial) placed above the injection-
// points card. Metric is the default (left segment starts selected).
// The segments store their active state in m_isMetric and expose m_metricSeg /
// m_imperialSeg so future unit-conversion wiring has a clear hook point.
// ---------------------------------------------------------------------------
wxPanel* FixtureEditor::CreateUnitToggle(wxWindow* parent)
{
    // Outer wrapper matches the scroll window background so only the two
    // segments themselves are visible as a pill. Container height shrunk
    // from 32 → 22 (about 10px shorter, per design tweak). Inner label
    // padding also dropped from 6 → 3 each side to keep the 9pt label
    // from clipping in the reduced height.
    auto* container = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 22));
    container->SetBackgroundColour(kEditorBg);

    // ---- Build one segment --------------------------------------------
    // Returns the segment panel + label pair; caller assigns to members.
    auto makeSegment = [&](const wxString& label, bool active)
        -> std::pair<wxPanel*, wxStaticText*>
        {
            const wxColour& bgOn = Style::BtnSecondarySelected;
            const wxColour& bgOff = Style::BtnSecondary;

            auto* seg = new wxPanel(container, wxID_ANY,
                wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
            seg->SetBackgroundColour(active ? bgOn : bgOff);

            auto* lbl = new wxStaticText(seg, wxID_ANY, label,
                wxDefaultPosition, wxDefaultSize,
                wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE);
            lbl->SetFont(kToolBtnFont);
            lbl->SetForegroundColour(active ? kTextActive : Style::TextPrimary);
            lbl->SetBackgroundColour(active ? bgOn : bgOff);

            auto* inner = new wxBoxSizer(wxHORIZONTAL);
            inner->AddStretchSpacer(1);
            inner->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 3);
            inner->AddStretchSpacer(1);
            seg->SetSizer(inner);

            return { seg, lbl };
        };

    auto [metSeg, metLbl] = makeSegment("Metric", true);
    auto [impSeg, impLbl] = makeSegment("Imperial", false);

    m_metricSeg = metSeg;
    m_metricLbl = metLbl;
    m_imperialSeg = impSeg;
    m_imperialLbl = impLbl;

    // 1-px vertical divider between the two segments, matching the
    // colour used by all other dividers in the sidebar.
    auto* divider = new wxPanel(container, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    divider->SetBackgroundColour(Style::Divider);

    // ---- Click + hover handlers -------------------------------------
    // applyUnit: switches active segment, swaps colours, converts all
    // tracked mm field values, and updates their unit labels in one pass.
    // Clicking the already-active segment is a no-op.
    // Degree fields are not in m_mmFields so they are never touched.
    auto applyUnit = [this](bool toMetric)
        {
            if (m_isMetric == toMetric) return;
            m_isMetric = toMetric;

            auto recolour = [](wxPanel* seg, wxStaticText* lbl, bool active)
                {
                    const wxColour& bg = active
                        ? Style::BtnSecondarySelected : Style::BtnSecondary;
                    const wxColour& fg = active
                        ? Style::TextActive : Style::TextPrimary;
                    seg->SetBackgroundColour(bg);
                    lbl->SetBackgroundColour(bg);
                    lbl->SetForegroundColour(fg);
                    seg->Refresh();
                    lbl->Refresh();
                };
            recolour(m_metricSeg, m_metricLbl, toMetric);
            recolour(m_imperialSeg, m_imperialLbl, !toMetric);

            // Convert every tracked mm field and swap its unit label.
            // toMetric:   in  * 25.4 -> mm
            // toImperial: mm / 25.4  -> in
            const double factor = toMetric ? 25.4 : (1.0 / 25.4);
            const wxString fmt = toMetric ? "%.2f" : "%.4f";
            const wxString newLbl = toMetric ? "mm" : "in";

            for (size_t i = 0; i < m_mmFields.size(); ++i)
            {
                wxTextCtrl* field = m_mmFields[i];
                wxStaticText* lbl = m_mmUnitLabels[i];
                if (!field || !lbl) continue;
                lbl->SetLabel(newLbl);
                double v = 0.0;
                if (field->GetValue().ToDouble(&v))
                    field->SetValue(wxString::Format(fmt, v * factor));
            }

            // The Grid Defaults summary is derived text (stored in mm), so
            // just re-render it in the new unit.
            UpdateGridSummary();
        };

    // Hover: highlight the inactive segment on mouse-over.
    auto onEnterMetric = [this](wxMouseEvent& e)
        { if (!m_isMetric) {
        m_metricSeg->SetBackgroundColour(Style::BtnSecondaryHover);
        m_metricLbl->SetBackgroundColour(Style::BtnSecondaryHover);
        m_metricSeg->Refresh();
    } e.Skip(); };
    auto onLeaveMetric = [this](wxMouseEvent& e)
        { if (!m_isMetric) {
        m_metricSeg->SetBackgroundColour(Style::BtnSecondary);
        m_metricLbl->SetBackgroundColour(Style::BtnSecondary);
        m_metricSeg->Refresh();
    } e.Skip(); };
    auto onEnterImperial = [this](wxMouseEvent& e)
        { if (m_isMetric) {
        m_imperialSeg->SetBackgroundColour(Style::BtnSecondaryHover);
        m_imperialLbl->SetBackgroundColour(Style::BtnSecondaryHover);
        m_imperialSeg->Refresh();
    } e.Skip(); };
    auto onLeaveImperial = [this](wxMouseEvent& e)
        { if (m_isMetric) {
        m_imperialSeg->SetBackgroundColour(Style::BtnSecondary);
        m_imperialLbl->SetBackgroundColour(Style::BtnSecondary);
        m_imperialSeg->Refresh();
    } e.Skip(); };

    for (wxWindow* w : { (wxWindow*)m_metricSeg, (wxWindow*)m_metricLbl })
    {
        w->Bind(wxEVT_LEFT_UP, [applyUnit](wxMouseEvent& e) { applyUnit(true);  e.Skip(); });
        w->Bind(wxEVT_ENTER_WINDOW, onEnterMetric);
        w->Bind(wxEVT_LEAVE_WINDOW, onLeaveMetric);
    }
    for (wxWindow* w : { (wxWindow*)m_imperialSeg, (wxWindow*)m_imperialLbl })
    {
        w->Bind(wxEVT_LEFT_UP, [applyUnit](wxMouseEvent& e) { applyUnit(false); e.Skip(); });
        w->Bind(wxEVT_ENTER_WINDOW, onEnterImperial);
        w->Bind(wxEVT_LEAVE_WINDOW, onLeaveImperial);
    }

    auto* hSizer = new wxBoxSizer(wxHORIZONTAL);
    hSizer->Add(m_metricSeg, 1, wxEXPAND);
    hSizer->Add(divider, 0, wxEXPAND);
    hSizer->Add(m_imperialSeg, 1, wxEXPAND);
    container->SetSizer(hSizer);

    return container;
}

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
    auto* addBtn = new RoundedButton(card, ID_FE_AddInjectionPoint,
        "Add Injection Point",
        wxDefaultPosition, wxSize(-1, 26));
    addBtn->SetBackgroundColour(Style::BtnSmall);
    addBtn->SetForegroundColour(*wxWHITE);
    sizer->Add(addBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    sizer->AddSpacer(10);

    // "Fixture Perimeter" injection option — when checked, the sprue's
    // injection point can be placed (and dragged) anywhere on the fixture
    // perimeter, in addition to any fixed points listed above. Read into
    // FixtureDefinition::allowPerimeterInjection at save time.
    m_allowPerimeterInjection = new wxCheckBox(card, wxID_ANY,
        "Fixture Perimeter (place injection point anywhere on perimeter)");
    m_allowPerimeterInjection->SetForegroundColour(Style::TextPrimary);
    sizer->Add(m_allowPerimeterInjection, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

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

                auto* editBtn = new RoundedButton(entry, wxID_ANY, "Edit",
                    wxDefaultPosition, wxSize(44, 22));
                editBtn->SetBackgroundColour(Style::BtnSmall);
                editBtn->SetForegroundColour(*wxWHITE);

                auto* removeBtn = new RoundedButton(entry, wxID_ANY, "Remove",
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

    // "No Override" is index 0 and the default selection. When active, all
    // dimension fields are hidden and nothing is written to the fixture file.
    AddTypeRow(card, sizer, "Sprue type:",
        { "No Override", "Cylinder" }, m_sprueTypeChoice);

    // Sub-panel wrapping all dim rows so they can be shown/hidden as a unit
    // by toggling m_sprueDimsPanel->Show() + Layout().
    m_sprueDimsPanel = new wxPanel(card, wxID_ANY);
    m_sprueDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimSizer = new wxBoxSizer(wxVERTICAL);

    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_sprueDimsPanel, dimSizer, "Diameter:", m_sprueDiameter, "5.0", "mm", &ul);
        m_mmFields.push_back(m_sprueDiameter); m_mmUnitLabels.push_back(ul);
    }
    AddDimRow(m_sprueDimsPanel, dimSizer, "Draft angle:", m_sprueDraftAngle, "1.0", DegSym());
    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_sprueDimsPanel, dimSizer, "Cold slug:", m_sprueColdSlugDepth, "5.0", "mm", &ul);
        m_mmFields.push_back(m_sprueColdSlugDepth); m_mmUnitLabels.push_back(ul);
    }
    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_sprueDimsPanel, dimSizer, "Sprue length:", m_sprueLength, "20.0", "mm", &ul);
        m_mmFields.push_back(m_sprueLength); m_mmUnitLabels.push_back(ul);
    }
    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_sprueDimsPanel, dimSizer, "Overrun:", m_sprueOverrun, "0.0", "mm", &ul);
        m_mmFields.push_back(m_sprueOverrun); m_mmUnitLabels.push_back(ul);
    }
    dimSizer->AddSpacer(4);

    m_sprueDimsPanel->SetSizer(dimSizer);
    m_sprueDimsPanel->Hide();   // hidden by default — "No Override" is selected
    sizer->Add(m_sprueDimsPanel, 0, wxEXPAND);

    // Show/hide the dims panel whenever the type choice changes.
    m_sprueTypeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            const bool noOverride =
                m_sprueTypeChoice->GetSelection() == 0;   // "No Override"
            m_sprueDimsPanel->Show(!noOverride);
            // Re-layout upward to the scrolled window so the card collapses
            // or expands cleanly without leaving stale whitespace.
            for (wxWindow* w = m_sprueDimsPanel->GetParent(); w; w = w->GetParent())
            {
                w->Layout();
                if (wxDynamicCast(w, wxScrolledWindow))
                {
                    wxDynamicCast(w, wxScrolledWindow)->FitInside();
                    break;
                }
            }
        });

    sizer->AddSpacer(10);
    return card;
}

wxPanel* FixtureEditor::CreateRunnersContent(wxWindow* parent)
{
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Runners", sizer);

    // "No Override" is index 0 and the default selection. When active, all
    // dimension fields are hidden and nothing is written to the fixture file.
    AddTypeRow(card, sizer, "Runner type:",
        { "No Override", "Cylindrical" }, m_runnerTypeChoice);

    // Sub-panel wrapping all dim rows so they can be shown/hidden as a unit.
    m_runnerDimsPanel = new wxPanel(card, wxID_ANY);
    m_runnerDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimSizer = new wxBoxSizer(wxVERTICAL);

    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_runnerDimsPanel, dimSizer, "Diameter:", m_runnerDiameter, "4.0", "mm", &ul);
        m_mmFields.push_back(m_runnerDiameter); m_mmUnitLabels.push_back(ul);
    }
    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_runnerDimsPanel, dimSizer, "Cold slug length:", m_runnerColdSlugDepth, "5.0", "mm", &ul);
        m_mmFields.push_back(m_runnerColdSlugDepth); m_mmUnitLabels.push_back(ul);
    }
    dimSizer->AddSpacer(4);

    m_runnerDimsPanel->SetSizer(dimSizer);
    m_runnerDimsPanel->Hide();   // hidden by default — "No Override" is selected
    sizer->Add(m_runnerDimsPanel, 0, wxEXPAND);

    // Show/hide the dims panel whenever the type choice changes.
    m_runnerTypeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            const bool noOverride =
                m_runnerTypeChoice->GetSelection() == 0;   // "No Override"
            m_runnerDimsPanel->Show(!noOverride);
            for (wxWindow* w = m_runnerDimsPanel->GetParent(); w; w = w->GetParent())
            {
                w->Layout();
                if (wxDynamicCast(w, wxScrolledWindow))
                {
                    wxDynamicCast(w, wxScrolledWindow)->FitInside();
                    break;
                }
            }
        });

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

    // ---- Gate half ----------------------------------------------------
    // "No Override" is index 0. When selected, gate dim fields are hidden.
    AddTypeRow(card, sizer, "Gate type:",
        { "No Override", "Tapered Cylinder" }, m_gateTypeChoice);

    m_gateDimsPanel = new wxPanel(card, wxID_ANY);
    m_gateDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* gateDimSizer = new wxBoxSizer(wxVERTICAL);

    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_gateDimsPanel, gateDimSizer, "Diameter:", m_gateDiameter, "3.0", "mm", &ul);
        m_mmFields.push_back(m_gateDiameter); m_mmUnitLabels.push_back(ul);
    }
    AddDimRow(m_gateDimsPanel, gateDimSizer, "Draft angle:", m_gateDraftAngle, "1.0", DegSym());
    gateDimSizer->AddSpacer(4);

    m_gateDimsPanel->SetSizer(gateDimSizer);
    m_gateDimsPanel->Hide();   // hidden by default — "No Override" is selected
    sizer->Add(m_gateDimsPanel, 0, wxEXPAND);

    m_gateTypeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            const bool noOverride =
                m_gateTypeChoice->GetSelection() == 0;   // "No Override"
            m_gateDimsPanel->Show(!noOverride);
            for (wxWindow* w = m_gateDimsPanel->GetParent(); w; w = w->GetParent())
            {
                w->Layout();
                if (wxDynamicCast(w, wxScrolledWindow))
                {
                    wxDynamicCast(w, wxScrolledWindow)->FitInside();
                    break;
                }
            }
        });

    // ---- Visual divider between gate and sub-runner -------------------
    sizer->AddSpacer(6);
    auto* subSep = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    subSep->SetBackgroundColour(Style::Divider);
    sizer->Add(subSep, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // ---- Sub-runner half ----------------------------------------------
    AddTypeRow(card, sizer, "Sub-runner type:",
        { "No Override", "Cylinder" }, m_subRunnerTypeChoice);

    m_subRunnerDimsPanel = new wxPanel(card, wxID_ANY);
    m_subRunnerDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* subDimSizer = new wxBoxSizer(wxVERTICAL);

    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_subRunnerDimsPanel, subDimSizer, "Diameter:", m_subRunnerDiameter, "5.0", "mm", &ul);
        m_mmFields.push_back(m_subRunnerDiameter); m_mmUnitLabels.push_back(ul);
    }
    subDimSizer->AddSpacer(4);

    m_subRunnerDimsPanel->SetSizer(subDimSizer);
    m_subRunnerDimsPanel->Hide();   // hidden by default — "No Override" is selected
    sizer->Add(m_subRunnerDimsPanel, 0, wxEXPAND);

    m_subRunnerTypeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            const bool noOverride =
                m_subRunnerTypeChoice->GetSelection() == 0;   // "No Override"
            m_subRunnerDimsPanel->Show(!noOverride);
            for (wxWindow* w = m_subRunnerDimsPanel->GetParent(); w; w = w->GetParent())
            {
                w->Layout();
                if (wxDynamicCast(w, wxScrolledWindow))
                {
                    wxDynamicCast(w, wxScrolledWindow)->FitInside();
                    break;
                }
            }
        });

    sizer->AddSpacer(10);
    return card;
}

wxPanel* FixtureEditor::CreateVentsContent(wxWindow* parent)
{
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Vents", sizer);

    // "No Override" is index 0 and the default selection. When active, all
    // dimension fields are hidden and nothing is written to the fixture file.
    AddTypeRow(card, sizer, "Vent type:",
        { "No Override", "Rectangular" }, m_ventTypeChoice);

    // Sub-panel wrapping all dim rows so they can be shown/hidden as a unit.
    m_ventDimsPanel = new wxPanel(card, wxID_ANY);
    m_ventDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* ventDimSizer = new wxBoxSizer(wxVERTICAL);

    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_ventDimsPanel, ventDimSizer, "Length:", m_ventLength, "1.0", "mm", &ul);
        m_mmFields.push_back(m_ventLength); m_mmUnitLabels.push_back(ul);
    }
    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_ventDimsPanel, ventDimSizer, "Width:", m_ventWidth, "2.0", "mm", &ul);
        m_mmFields.push_back(m_ventWidth); m_mmUnitLabels.push_back(ul);
    }
    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_ventDimsPanel, ventDimSizer, "Overrun (start):", m_ventOverrunStart, "0.5", "mm", &ul);
        m_mmFields.push_back(m_ventOverrunStart); m_mmUnitLabels.push_back(ul);
    }
    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_ventDimsPanel, ventDimSizer, "Overrun (end):", m_ventOverrunEnd, "0.5", "mm", &ul);
        m_mmFields.push_back(m_ventOverrunEnd); m_mmUnitLabels.push_back(ul);
    }
    ventDimSizer->AddSpacer(4);

    m_ventDimsPanel->SetSizer(ventDimSizer);
    m_ventDimsPanel->Hide();   // hidden by default — "No Override" is selected
    sizer->Add(m_ventDimsPanel, 0, wxEXPAND);

    // Show/hide the dims panel whenever the type choice changes.
    m_ventTypeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            const bool noOverride =
                m_ventTypeChoice->GetSelection() == 0;   // "No Override"
            m_ventDimsPanel->Show(!noOverride);
            for (wxWindow* w = m_ventDimsPanel->GetParent(); w; w = w->GetParent())
            {
                w->Layout();
                if (wxDynamicCast(w, wxScrolledWindow))
                {
                    wxDynamicCast(w, wxScrolledWindow)->FitInside();
                    break;
                }
            }
        });

    sizer->AddSpacer(10);
    return card;
}

wxPanel* FixtureEditor::CreateEjectorsContent(wxWindow* parent)
{
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Ejectors", sizer);

    // "No Override" is index 0 and the default selection. When active, all
    // dimension fields are hidden and nothing is written to the fixture file.
    AddTypeRow(card, sizer, "Ejector type:",
        { "No Override", "Cylindrical" }, m_ejectorTypeChoice);

    // Sub-panel wrapping all dim rows so they can be shown/hidden as a unit.
    m_ejectorDimsPanel = new wxPanel(card, wxID_ANY);
    m_ejectorDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* ejectorDimSizer = new wxBoxSizer(wxVERTICAL);

    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_ejectorDimsPanel, ejectorDimSizer, "Diameter:", m_ejectorDiameter, "3.0", "mm", &ul);
        m_mmFields.push_back(m_ejectorDiameter); m_mmUnitLabels.push_back(ul);
    }
    {
        wxStaticText* ul = nullptr;
        AddDimRow(m_ejectorDimsPanel, ejectorDimSizer, "Length:", m_ejectorLength, "25.0", "mm", &ul);
        m_mmFields.push_back(m_ejectorLength); m_mmUnitLabels.push_back(ul);
    }
    ejectorDimSizer->AddSpacer(4);

    m_ejectorDimsPanel->SetSizer(ejectorDimSizer);
    m_ejectorDimsPanel->Hide();   // hidden by default — "No Override" is selected
    sizer->Add(m_ejectorDimsPanel, 0, wxEXPAND);

    // Show/hide the dims panel whenever the type choice changes.
    m_ejectorTypeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            const bool noOverride =
                m_ejectorTypeChoice->GetSelection() == 0;   // "No Override"
            m_ejectorDimsPanel->Show(!noOverride);
            for (wxWindow* w = m_ejectorDimsPanel->GetParent(); w; w = w->GetParent())
            {
                w->Layout();
                if (wxDynamicCast(w, wxScrolledWindow))
                {
                    wxDynamicCast(w, wxScrolledWindow)->FitInside();
                    break;
                }
            }
        });

    sizer->AddSpacer(10);
    return card;
}

// ---------------------------------------------------------------------------
// Grid Defaults card — a summary line + an "Edit..." button that opens the
// shared GridSettingsDialog. Unlike the feature cards (which expose their
// fields inline), the grid's shape-dependent field set is handled entirely by
// the dialog; the card just shows the current choice and gates whether it is
// written into the fixture via the override checkbox.
// ---------------------------------------------------------------------------
wxPanel* FixtureEditor::CreateGridDefaultsContent(wxWindow* parent)
{
    wxSizer* sizer = nullptr;
    auto* card = MakeCardWithTitle(parent, "Grid Defaults", sizer);

    // Off by default — mirrors the "No Override" stance of the feature cards.
    // Only when ticked does OnGenerateFixture emit a [grid_defaults] section.
    m_gridOverride = new wxCheckBox(card, wxID_ANY,
        "Save grid defaults with this fixture");
    m_gridOverride->SetForegroundColour(Style::TextPrimary);
    m_gridOverride->SetValue(false);
    sizer->Add(m_gridOverride, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // Human-readable digest of m_gridDefaults (unit follows the editor toggle).
    m_gridSummary = new wxStaticText(card, wxID_ANY, wxEmptyString);
    m_gridSummary->SetForegroundColour(Style::TextSubtle);
    sizer->Add(m_gridSummary, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    auto* btnEdit = new RoundedButton(card, wxID_ANY, "Edit...",
        wxDefaultPosition, wxSize(90, 28), wxBORDER_NONE);
    btnEdit->SetBackgroundColour(Style::BtnSecondary);
    btnEdit->SetForegroundColour(Style::TextPrimary);
    sizer->Add(btnEdit, 0, wxLEFT | wxTOP, 12);

    // Edit... seeds the dialog with the current defaults and the editor's unit.
    btnEdit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
        {
            GridSettingsDialog dlg(this, m_gridDefaults, !m_isMetric);
            if (dlg.ShowModal() != wxID_OK)
                return;
            m_gridDefaults = dlg.GetSettings();
            // Editing the grid implies the user wants it saved.
            if (m_gridOverride) m_gridOverride->SetValue(true);
            UpdateGridSummary();
            // Preview the change on the editor's own grid so it's not a blind
            // edit.
            if (m_canvas) m_canvas->SetGridSettings(m_gridDefaults);
        });

    UpdateGridSummary();
    sizer->AddSpacer(10);
    return card;
}

// ---------------------------------------------------------------------------
// UpdateGridSummary — render m_gridDefaults into the card's summary label.
// Values are stored in mm; the display unit follows the editor's toggle.
// ---------------------------------------------------------------------------
void FixtureEditor::UpdateGridSummary()
{
    if (!m_gridSummary) return;

    const double toDisp = m_isMetric ? 1.0 : (1.0 / 25.4);
    const wxString u = m_isMetric ? "mm" : "in";
    auto len = [&](float mm) { return wxString::Format("%.4g", mm * toDisp); };

    wxString s;
    if (m_gridDefaults.shape == GridShape::Circular)
    {
        s = wxString::Format(
            "Circular - r %s %s - %d spokes - spacing %s %s - major /%d",
            len(m_gridDefaults.radius), u,
            m_gridDefaults.spokes,
            len(m_gridDefaults.spacing), u,
            m_gridDefaults.majorEvery);
    }
    else
    {
        s = wxString::Format(
            "Rectangular - %s x %s %s - spacing %s %s - major /%d",
            len(m_gridDefaults.sizeX), len(m_gridDefaults.sizeY), u,
            len(m_gridDefaults.spacing), u,
            m_gridDefaults.majorEvery);
    }
    m_gridSummary->SetLabel(s);
}
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
// Fixture pre-population + Select-button handlers
//
// SetInitialFixture is the entry point CreateFixtureDialog uses to push its
// gathered name + paths into a fresh editor. Below it, the OnSelectModelA/B
// handlers drive the per-half "Select" buttons in the top ribbon — both go
// through the same wxFileDialog → field → canvas pipeline. The canvas call
// is guarded so a Select press before the canvas is initialised (unlikely
// but possible during startup) is a no-op rather than a crash.
// ---------------------------------------------------------------------------
void FixtureEditor::SetInitialFixture(const std::string& fixtureName,
    const std::string& modelAPath,
    const std::string& modelBPath,
    ProgressCallback   progress)
{
    // Local helper — invokes the caller's progress callback if one was
    // supplied, swallows otherwise. Keeps the call sites readable.
    auto report = [&progress](int pct, const std::string& status)
        {
            if (progress) progress(pct, status);
        };

    // Stash the name unconditionally — OnGenerateFixture reads it later as
    // the default save filename. Empty just falls back to the hardcoded
    // "NewFixture.fixture" default.
    m_fixtureName = fixtureName;

    // Progress steps are coarse because there's no per-feature hook into
    // FixtureCanvas::LoadHalf yet. Each LoadHalf is one synchronous chunk;
    // we report before and after so the bar moves twice. If LoadHalf
    // grows a callback later, the percent can become finer-grained
    // without changing this call's shape.
    report(0, "Preparing fixture...");

    // For each half: mirror exactly what OnSelectModelA/B do (set the path
    // member + load into the canvas), and additionally push the path into
    // the text field so the top ribbon displays what the user picked in
    // CreateFixtureDialog. SetValue does NOT fire the wxEVT_BUTTON binding
    // (that only fires on the Select button click), so we won't double-
    // trigger LoadHalf.
    if (!modelAPath.empty())
    {
        report(10, "Loading Mould Half A...");
        m_modelAPath = modelAPath;
        if (m_pathACtrl)
            m_pathACtrl->SetValue(modelAPath);
        if (m_canvas)
            m_canvas->LoadHalf(FixtureCanvas::HalfSlot::A, modelAPath);
        // Fresh import — keep the Hide checkbox in sync with the
        // canvas's Destroy-resets-to-visible behaviour.
        if (m_hideACheck) m_hideACheck->SetValue(false);
        report(50, "Loaded Mould Half A");
    }
    if (!modelBPath.empty())
    {
        report(55, "Loading Mould Half B...");
        m_modelBPath = modelBPath;
        if (m_pathBCtrl)
            m_pathBCtrl->SetValue(modelBPath);
        if (m_canvas)
            m_canvas->LoadHalf(FixtureCanvas::HalfSlot::B, modelBPath);
        if (m_hideBCheck) m_hideBCheck->SetValue(false);
        report(95, "Loaded Mould Half B");
    }

    report(100, "Opening editor...");
}

// ---------------------------------------------------------------------------
// Select handlers — wxFileDialog → text field → canvas
//
// Shared "pick a STEP/IGES file" helper kept inline because it's only two
// call sites and threading another file/header just for that would be
// overkill. Mirrors CreateFixtureDialog::PickModelPath; if a third site
// ever needs it, factor into a shared utility header.
// ---------------------------------------------------------------------------
namespace
{
    bool PickModelPath(wxWindow* parent, wxTextCtrl* target, std::string& outPath)
    {
        // Default the open dialog to the directory of the field's current
        // value (if any) — so picking half B after half A doesn't drop
        // the user back at their home folder.
        wxString defaultDir;
        const wxString current = target->GetValue();
        if (!current.IsEmpty())
        {
            std::error_code ec;
            namespace fs = std::filesystem;
            const auto parentDir = fs::path(current.ToStdString()).parent_path();
            if (!parentDir.empty() && fs::exists(parentDir, ec))
                defaultDir = parentDir.string();
        }

        wxFileDialog dlg(parent, "Select Mould Half",
            defaultDir, wxEmptyString,
            "STEP files (*.step;*.stp)|*.step;*.stp"
            "|IGES files (*.iges;*.igs)|*.iges;*.igs"
            "|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return false;

        outPath = dlg.GetPath().ToStdString();
        target->SetValue(outPath);
        return true;
    }
}

void FixtureEditor::OnSelectModelA(wxCommandEvent&)
{
    std::string picked;
    if (!PickModelPath(this, m_pathACtrl, picked)) return;
    m_modelAPath = picked;
    if (m_canvas)
        m_canvas->LoadHalf(FixtureCanvas::HalfSlot::A, m_modelAPath);
    // LoadHalf's Destroy resets visible=true on the new geometry; mirror
    // that on the UI side so a previously-hidden slot doesn't look
    // (checkbox-wise) hidden after a fresh import.
    if (m_hideACheck) m_hideACheck->SetValue(false);
}

void FixtureEditor::OnSelectModelB(wxCommandEvent&)
{
    std::string picked;
    if (!PickModelPath(this, m_pathBCtrl, picked)) return;
    m_modelBPath = picked;
    if (m_canvas)
        m_canvas->LoadHalf(FixtureCanvas::HalfSlot::B, m_modelBPath);
    if (m_hideBCheck) m_hideBCheck->SetValue(false);
}

// ---------------------------------------------------------------------------
// Hide handlers — forward the checkbox state into FixtureCanvas. Checked
// means "hide", so visibility = !checked. Both handlers are guarded on
// m_canvas because BuildTopRibbon (which creates the checkboxes) runs
// before BuildCanvasArea — if Bind fired during construction for any
// reason, the canvas pointer wouldn't be live yet.
// ---------------------------------------------------------------------------
void FixtureEditor::OnHideHalfA(wxCommandEvent& evt)
{
    if (m_canvas)
        m_canvas->SetHalfVisible(FixtureCanvas::HalfSlot::A, !evt.IsChecked());
}

void FixtureEditor::OnHideHalfB(wxCommandEvent& evt)
{
    if (m_canvas)
        m_canvas->SetHalfVisible(FixtureCanvas::HalfSlot::B, !evt.IsChecked());
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

    // Default save filename — if CreateFixtureDialog supplied a fixture
    // name via SetInitialFixture, use it (with a .fixture suffix); otherwise
    // fall back to the historical placeholder. The name comes in human-
    // readable form ("ASPX Rev 2") and we just append the extension; spaces
    // and mixed case are valid on Windows and the user can still rename in
    // the save dialog if they want a more conventional filename. Filesystem
    // characters that aren't valid in NTFS filenames (\ / : * ? " < > |)
    // would surface here as an OS-level rename failure rather than a silent
    // bad-name; can revisit with explicit sanitisation if it shows up in
    // practice.
    const std::string defaultFile = m_fixtureName.empty()
        ? std::string("NewFixture.fixture")
        : (m_fixtureName + ".fixture");

    wxFileDialog dlg(this, "Save Fixture",
        defaultDir, defaultFile,
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
    // Base units in the fixture file are always mm. If the user switched to
    // imperial, displayed values are in inches and must be converted back.
    // Degree fields (draft angles) are never scaled.
    const double toMM = m_isMetric ? 1.0 : 25.4;

    auto readFieldMM = [toMM](wxTextCtrl* ctrl, std::optional<float>& dst)
        {
            if (!ctrl) return;
            double v = 0.0;
            if (ctrl->GetValue().ToDouble(&v))
                dst = static_cast<float>(v * toMM);
        };
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

    // Sprues — skipped entirely when type is "No Override" (index 0).
    if (m_sprueTypeChoice && m_sprueTypeChoice->GetSelection() != 0)
    {
        readChoice(m_sprueTypeChoice, def.sprueDefaults.type);
        readFieldMM(m_sprueDiameter, def.sprueDefaults.diameter);
        readField(m_sprueDraftAngle, def.sprueDefaults.draftAngle);
        readFieldMM(m_sprueColdSlugDepth, def.sprueDefaults.coldSlugLength);
        readFieldMM(m_sprueLength, def.sprueDefaults.length);
        readFieldMM(m_sprueOverrun, def.sprueDefaults.overrun);
    }

    // Runners — skipped entirely when type is "No Override" (index 0).
    if (m_runnerTypeChoice && m_runnerTypeChoice->GetSelection() != 0)
    {
        readChoice(m_runnerTypeChoice, def.runnerDefaults.type);
        readFieldMM(m_runnerDiameter, def.runnerDefaults.diameter);
        readFieldMM(m_runnerColdSlugDepth, def.runnerDefaults.coldSlugLength);
    }

    // Gates + sub-runner — both feed off the gate card.
    // Each half is skipped independently when its type is "No Override".
    if (m_gateTypeChoice && m_gateTypeChoice->GetSelection() != 0)
    {
        readChoice(m_gateTypeChoice, def.gateDefaults.type);
        readFieldMM(m_gateDiameter, def.gateDefaults.diameter);
        readField(m_gateDraftAngle, def.gateDefaults.draftAngle);
    }
    if (m_subRunnerTypeChoice && m_subRunnerTypeChoice->GetSelection() != 0)
    {
        readChoice(m_subRunnerTypeChoice, def.subRunnerDefaults.type);
        readFieldMM(m_subRunnerDiameter, def.subRunnerDefaults.diameter);
    }

    // Vents — skipped entirely when type is "No Override" (index 0).
    if (m_ventTypeChoice && m_ventTypeChoice->GetSelection() != 0)
    {
        readChoice(m_ventTypeChoice, def.ventDefaults.type);
        readFieldMM(m_ventLength, def.ventDefaults.length);
        readFieldMM(m_ventWidth, def.ventDefaults.width);
        readFieldMM(m_ventOverrunStart, def.ventDefaults.overrunStart);
        readFieldMM(m_ventOverrunEnd, def.ventDefaults.overrunEnd);
    }

    // Ejectors — skipped entirely when type is "No Override" (index 0).
    if (m_ejectorTypeChoice && m_ejectorTypeChoice->GetSelection() != 0)
    {
        readChoice(m_ejectorTypeChoice, def.ejectorDefaults.type);
        readFieldMM(m_ejectorDiameter, def.ejectorDefaults.diameter);
        readFieldMM(m_ejectorLength, def.ejectorDefaults.length);
    }

    // Grid defaults — only baked in when the override checkbox is ticked.
    // m_gridDefaults is already stored in mm (the dialog handles unit display),
    // so there's no unit conversion to do here.
    if (m_gridOverride && m_gridOverride->GetValue())
    {
        const GridSettings& g = m_gridDefaults;
        def.gridDefaults.shape = (g.shape == GridShape::Circular)
            ? std::string("circular") : std::string("rectangular");
        def.gridDefaults.sizeX = g.sizeX;
        def.gridDefaults.sizeY = g.sizeY;
        def.gridDefaults.radius = g.radius;
        def.gridDefaults.spokes = g.spokes;
        def.gridDefaults.spacing = g.spacing;
        def.gridDefaults.majorEvery = g.majorEvery;
    }

    // Injection points — straight copy of the editor's live list. Order
    // is preserved so the saved file's [injection_point.0..N] sections
    // come out in the same order the user added them in the sidebar.
    def.injectionPoints = m_injectionPoints;
    def.allowPerimeterInjection =
        m_allowPerimeterInjection && m_allowPerimeterInjection->GetValue();

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
