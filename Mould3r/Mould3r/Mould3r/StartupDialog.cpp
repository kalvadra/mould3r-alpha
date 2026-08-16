#include "StartupDialog.h"
#include <wx/stdpaths.h>
#include <filesystem>
#include <vector>
#include "style.h"
#include "FixtureEditor.h"
#include "CreateFixtureDialog.h"
#include "ProceduralFixtureDialog.h"  // dimension/clearance prompt for procedural rows
#include "RoundedButton.h"     // rounded button for the dialog's action buttons
#include "WindowEffects.h"     // DWM corner rounding for the frameless dialog

namespace fs = std::filesystem;

namespace
{
    // Sentinels stored in m_fixturePaths for the two synthetic, non-file rows
    // at the top of the list. They can't collide with a real fixture path
    // (no filesystem path contains these), so a simple string compare in the
    // selection handlers distinguishes a procedural row from a library file.
    constexpr const char* kParametricRow = "<<procedural:parametric>>";
    constexpr const char* kDynamicRow    = "<<procedural:dynamic>>";

    // Shared font for all dialog buttons — matches the ribbon buttons in
    // MainFrame (9pt Segoe UI semibold).
    wxFont DialogBtnFont()
    {
        return wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI");
    }

    // Style a button as the "primary" action — indigo, white label,
    // semibold, no system border. Matches the Import button in the main
    // ribbon. Used here for the left-side "New Fixture..." action.
    void StylePrimaryButton(RoundedButton* btn)
    {
        btn->SetBackgroundColour(Style::BtnSecondary);
        btn->SetForegroundColour(*wxWHITE);
        btn->SetFont(DialogBtnFont());
    }

    // Style a button as the "confirm" action — green, white label,
    // semibold, no system border. Mirrors the "Generate Mould" button in
    // the main ribbon; used here for the dialog's Select action.
    void StyleConfirmButton(RoundedButton* btn)
    {
        btn->SetBackgroundColour(Style::BtnGenerate);
        btn->SetForegroundColour(*wxWHITE);
        btn->SetFont(DialogBtnFont());
    }

    // Style a button to read as "text only" — background blended into the
    // dialog's AppBg so the rectangle disappears, leaving just a label.
    // Used for Cancel in dialogs where it's the dismissive (not destructive)
    // action and shouldn't compete visually with the primary confirm.
    void StyleTextButton(RoundedButton* btn)
    {
        btn->SetBackgroundColour(Style::AppBg);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(DialogBtnFont());
    }
}

StartupDialog::StartupDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(560, 520),
        wxBORDER_NONE)
{
    // Hairline border around the dialog. The dialog's own background acts
    // as the border colour, and the contentPanel below sits inside with a
    // 1-px inset on all four sides — the inset is what makes the border
    // visible. All previously-direct children of the dialog (title row,
    // subtitle, list, preview card, buttons) get reparented to
    // contentPanel so its AppBg fills the interior cleanly instead of the
    // sky-blue showing through gaps. Win11 auto-rounds the borderless
    // dialog and the border follows; Win10 sees a sharp rectangle (same
    // trade-off as the rest of the frameless behaviour).
    SetBackgroundColour(wxColour(0x6C, 0xB6, 0xF0));  // soft sky-blue border

    auto* contentPanel = new wxPanel(this, wxID_ANY);
    contentPanel->SetBackgroundColour(Style::AppBg);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(contentPanel, 1, wxEXPAND | wxALL, 1);  // 1-px hairline
    SetSizer(outer);

    // Default fixtures folder: 'fixtures/' next to the executable
    m_fixturesFolder = (fs::path(wxStandardPaths::Get()
        .GetExecutablePath().ToStdString())
        .parent_path() / "fixtures").string();

    auto* main = new wxBoxSizer(wxVERTICAL);

    // ---- Title row (stands in for the system title bar) -------------------
    // wxBORDER_NONE drops the system chrome, so we render our own title text
    // and close X. This row also serves as the drag handle — see the four
    // OnTitle* handlers below — so the user can still move the dialog.
    // Trade-offs of going frameless:
    //   * No system caption → Esc and our X are the only ways to close
    //     (both route to wxID_CANCEL, which wxDialog auto-ends as CANCEL)
    //   * No system resize border (fine for a fixed-size dialog)
    //   * Windows 11 rounds borderless windows automatically; Windows 10
    //     will render sharp corners (acceptable degradation)
    m_titleRow = new wxPanel(contentPanel, wxID_ANY);
    m_titleRow->SetBackgroundColour(Style::AppBg);
    auto* titleRowSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* title = new wxStaticText(m_titleRow, wxID_ANY, "Select Fixture");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(wxFont(16, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));

    // Close button — keeps wxID_CANCEL because (a) wxDialog's default
    // Escape handler routes there, so Esc still closes the dialog, and
    // (b) this single-glyph button is small enough that the Windows
    // stock-button styling that wxID_CANCEL pulls in doesn't visibly
    // interfere. The bottom-row "Cancel" button uses a private ID_Cancel
    // for that reason — see its binding below. ✕ is U+2715 (MULTIPLICATION
    // X), passed as UTF-8 bytes to avoid source-encoding assumptions.
    auto* btnClose = new RoundedButton(m_titleRow, wxID_CANCEL,
        wxString::FromUTF8("\xE2\x9C\x95"),
        wxDefaultPosition, wxSize(30, 30), wxBORDER_NONE);
    btnClose->SetBackgroundColour(Style::AppBg);
    btnClose->SetForegroundColour(Style::TextMuted);
    btnClose->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    // Subtle hover — brighten the X on enter, restore on leave. wxButton on
    // Windows doesn't expose a built-in hover colour when a custom
    // background is set, so we paint it via enter/leave events.
    btnClose->Bind(wxEVT_ENTER_WINDOW, [btnClose](wxMouseEvent& e) {
        btnClose->SetForegroundColour(Style::TextPrimary);
        btnClose->Refresh();
        e.Skip();
        });
    btnClose->Bind(wxEVT_LEAVE_WINDOW, [btnClose](wxMouseEvent& e) {
        btnClose->SetForegroundColour(Style::TextMuted);
        btnClose->Refresh();
        e.Skip();
        });

    titleRowSizer->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP, 18);
    titleRowSizer->Add(btnClose, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxTOP, 12);
    m_titleRow->SetSizer(titleRowSizer);

    main->Add(m_titleRow, 0, wxEXPAND);

    // Drag bindings. Also bind on the title wxStaticText — it consumes its
    // own mouse events instead of forwarding them to the panel underneath,
    // so without these bindings clicks on the text wouldn't initiate a drag.
    // Capture is taken on m_titleRow regardless of which child generated the
    // initial down-event, so motion/up/capture-lost only need to be bound
    // on m_titleRow.
    m_titleRow->Bind(wxEVT_LEFT_DOWN, &StartupDialog::OnTitleMouseDown, this);
    m_titleRow->Bind(wxEVT_LEFT_UP, &StartupDialog::OnTitleMouseUp, this);
    m_titleRow->Bind(wxEVT_MOTION, &StartupDialog::OnTitleMouseMove, this);
    m_titleRow->Bind(wxEVT_MOUSE_CAPTURE_LOST, &StartupDialog::OnTitleCaptureLost, this);
    title->Bind(wxEVT_LEFT_DOWN, &StartupDialog::OnTitleMouseDown, this);

    // ---- Subtitle ---------------------------------------------------------
    // Description text — white per the prototype (was Style::TextSubtext
    // which rendered as a muted grey).
    auto* subtitle = new wxStaticText(contentPanel, wxID_ANY,
        "Choose a fixture to load, or create a new one.\n"
        "A fixture is a set of blank mould halves that the design features will be cut from.\n"
        "It may also include ejector information.");
    subtitle->SetForegroundColour(Style::TextPrimary);
    subtitle->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    main->Add(subtitle, 0, wxLEFT | wxTOP | wxBOTTOM, 18);

    // ---- Fixture list -----------------------------------------------------
    // Plain wxListCtrl, mirroring the original setup — the wxDataViewCtrl
    // experiment (custom model + custom renderer for selection styling and
    // per-column bold) has been rolled back. The selection contrast and
    // header theming compromises that come with wxListCtrl are accepted for
    // now; we'll revisit the table presentation as a focused pass later.
    m_list = new wxListCtrl(contentPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    m_list->SetBackgroundColour(Style::SectionHeaderBg);
    m_list->SetForegroundColour(Style::TextPrimary);
    m_list->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    // Original column widths (240/130/130). The "Mould Half A/B" labels
    // are kept (rather than reverting to "Model A/B") so the table stays
    // consistent with the renamed preview labels below.
    m_list->InsertColumn(0, "Fixture", wxLIST_FORMAT_LEFT, 240);
    m_list->InsertColumn(1, "Mould Half A", wxLIST_FORMAT_LEFT, 130);
    m_list->InsertColumn(2, "Mould Half B", wxLIST_FORMAT_LEFT, 130);

    main->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // ---- Preview card -----------------------------------------------------
    auto* previewPanel = new wxPanel(contentPanel, wxID_ANY);
    previewPanel->SetBackgroundColour(Style::CardBg);
    auto* previewInner = new wxBoxSizer(wxVERTICAL);

    auto* previewHeader = new wxStaticText(previewPanel, wxID_ANY,
        "SELECTED FIXTURE");
    previewHeader->SetForegroundColour(Style::TextSubtle);
    previewHeader->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    previewInner->Add(previewHeader, 0, wxLEFT | wxTOP, 10);

    m_lblModelA = new wxStaticText(previewPanel, wxID_ANY, "Mould Half A: —",
        wxDefaultPosition, wxDefaultSize,
        wxST_ELLIPSIZE_END);
    m_lblModelB = new wxStaticText(previewPanel, wxID_ANY, "Mould Half B: —",
        wxDefaultPosition, wxDefaultSize,
        wxST_ELLIPSIZE_END);

    for (auto* lbl : { m_lblModelA, m_lblModelB })
    {
        lbl->SetForegroundColour(Style::TextPrimary);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        previewInner->Add(lbl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }
    previewInner->AddSpacer(10);

    previewPanel->SetSizer(previewInner);
    main->Add(previewPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // ---- Buttons ----------------------------------------------------------
    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* btnNew = new RoundedButton(contentPanel, ID_NewFixture, "New Fixture...",
        wxDefaultPosition, wxSize(130, 32), wxBORDER_NONE);
    StylePrimaryButton(btnNew);

    btnSizer->Add(btnNew, 0);
    btnSizer->AddStretchSpacer();

    auto* btnCancel = new RoundedButton(contentPanel, ID_Cancel, "Cancel",
        wxDefaultPosition, wxSize(90, 32), wxBORDER_NONE);
    StyleTextButton(btnCancel);

    auto* btnOK = new RoundedButton(contentPanel, wxID_OK, "Select",
        wxDefaultPosition, wxSize(90, 32), wxBORDER_NONE);
    StyleConfirmButton(btnOK);

    btnSizer->Add(btnCancel, 0, wxRIGHT, 8);
    btnSizer->Add(btnOK, 0);

    main->Add(btnSizer, 0, wxEXPAND | wxALL, 14);

    contentPanel->SetSizer(main);
    CentreOnScreen();

    Bind(wxEVT_BUTTON, &StartupDialog::OnNewFixture, this, ID_NewFixture);
    Bind(wxEVT_BUTTON, &StartupDialog::OnOK, this, wxID_OK);
    // Cancel uses a private ID (rather than wxID_CANCEL) so that the
    // wxButton stock-handling on Windows doesn't merge its visual styling
    // with the title-row close X — wxID_CANCEL buttons can pick up some
    // system-themed treatment that overrides the explicit colours we set
    // via StyleTextButton, making the two converge. The dialog still ends
    // with wxID_CANCEL so the calling code in MainFrame::OnChangeFixture
    // etc. continues to work unchanged. Escape still routes to the close
    // X (which keeps wxID_CANCEL) via wxDialog's built-in escape handling.
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); },
        ID_Cancel);
    Bind(wxEVT_LIST_ITEM_SELECTED,
        &StartupDialog::OnListSelect, this, m_list->GetId());
    Bind(wxEVT_LIST_ITEM_ACTIVATED,
        &StartupDialog::OnListDoubleClick, this, m_list->GetId());

    ScanFixturesFolder();

    // Round the dialog's corners via DWM on Win11 (no-op on Win10). The
    // 1-px sky-blue hairline border this dialog draws around its content
    // panel still works — DWM rounds the outer window edge and the
    // hairline gets clipped along that arc, giving a subtle rounded
    // accent ring.
    WindowEffects::ApplyRoundedCorners(this);
}

// ---------------------------------------------------------------------------
// Frameless drag handlers
//
// Strategy: at MouseDown we record the screen-space offset from the dialog's
// top-left to the click point and capture the mouse on m_titleRow. Subsequent
// motion events compute the new dialog position as cursor − offset, which
// gives one-to-one cursor-to-window movement regardless of where on the title
// row the drag started. wxGetMousePosition() returns absolute screen coords
// so we don't have to translate between control-local coordinate systems
// (the initial click may originate from m_titleRow OR from the title
// wxStaticText, which have different local origins).
// ---------------------------------------------------------------------------
void StartupDialog::OnTitleMouseDown(wxMouseEvent&)
{
    m_dragOffset = wxGetMousePosition() - GetPosition();
    m_dragging = true;
    if (!m_titleRow->HasCapture())
        m_titleRow->CaptureMouse();
}

void StartupDialog::OnTitleMouseUp(wxMouseEvent&)
{
    if (m_dragging)
    {
        m_dragging = false;
        if (m_titleRow->HasCapture())
            m_titleRow->ReleaseMouse();
    }
}

void StartupDialog::OnTitleMouseMove(wxMouseEvent& evt)
{
    if (!m_dragging) { evt.Skip(); return; }
    SetPosition(wxGetMousePosition() - m_dragOffset);
}

void StartupDialog::OnTitleCaptureLost(wxMouseCaptureLostEvent&)
{
    // Required handler: wxWidgets asserts in debug if capture is lost
    // (Alt+Tab, system modal, etc.) without an explicit handler. Just
    // clear the drag flag; the system has already released the capture.
    m_dragging = false;
}

// ---------------------------------------------------------------------------
// Fixture list population
// ---------------------------------------------------------------------------
void StartupDialog::ScanFixturesFolder()
{
    m_list->DeleteAllItems();
    m_fixturePaths.clear();
    m_fixture = FixtureDefinition{};
    RefreshPreview();

    // Two synthetic rows at the top: the procedural fixtures. They're always
    // available (independent of the fixtures folder) and carry a sentinel in
    // m_fixturePaths instead of a file path — the selection handlers branch on
    // it. Inserting them first means the scanned file rows below naturally
    // continue from index 2 via the existing m_fixturePaths.size() pattern.
    auto addProceduralRow = [&](const char* sentinel, const wxString& name,
        const wxString& halfLabel)
    {
        const long idx = m_list->InsertItem((long)m_fixturePaths.size(), name);
        m_list->SetItem(idx, 1, halfLabel);
        m_list->SetItem(idx, 2, halfLabel);
        m_fixturePaths.push_back(sentinel);
    };
    addProceduralRow(kParametricRow, "Parametric Box", "(fixed size)");
    addProceduralRow(kDynamicRow, "Dynamic Box", "(auto-fit)");

    if (!fs::exists(m_fixturesFolder) || !fs::is_directory(m_fixturesFolder))
    {
        // Folder missing — surface the empty-state via the preview labels
        // since the prototype no longer carries an inline folder-status row.
        m_lblModelA->SetLabel("Mould Half A: (fixtures folder not found)");
        return;
    }

    for (const auto& entry : fs::recursive_directory_iterator(m_fixturesFolder))
    {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".fixture") continue;

        const std::string path = entry.path().string();

        FixtureDefinition def;
        std::string error;
        if (!FixtureFile::Load(path, def, error)) continue;

        const std::string nameA = fs::path(def.modelAPath).stem().string();
        const std::string nameB = fs::path(def.modelBPath).stem().string();
        const std::string fixtureName = entry.path().stem().string();

        const long idx = m_list->InsertItem(
            (long)m_fixturePaths.size(), fixtureName);
        m_list->SetItem(idx, 1, nameA);
        m_list->SetItem(idx, 2, nameB);

        m_fixturePaths.push_back(path);
    }

    if (m_fixturePaths.empty())
        m_lblModelA->SetLabel("Mould Half A: (no fixtures found)");
}

void StartupDialog::OnListSelect(wxListEvent& evt)
{
    const long idx = evt.GetIndex();
    if (idx < 0 || idx >= (long)m_fixturePaths.size()) return;

    const std::string& path = m_fixturePaths[idx];

    // Procedural rows carry a sentinel, not a file. Set the kind (with default
    // params) now; the dimension/clearance prompt is deferred to
    // AcceptSelection so a single click just previews it.
    if (path == kParametricRow || path == kDynamicRow)
    {
        m_fixture = FixtureDefinition{};
        m_fixture.kind = (path == kParametricRow)
            ? FixtureKind::Parametric : FixtureKind::Dynamic;
        RefreshPreview();
        return;
    }

    std::string error;
    if (!FixtureFile::Load(path, m_fixture, error))
    {
        wxMessageBox(error, "Load Error", wxOK | wxICON_ERROR, this);
        return;
    }

    RefreshPreview();
}

void StartupDialog::OnListDoubleClick(wxListEvent& evt)
{
    OnListSelect(evt);
    AcceptSelection();
}

void StartupDialog::OnNewFixture(wxCommandEvent&)
{
    // Two-step flow: CreateFixtureDialog collects name + both half paths
    // up front and shows a progress bar while the editor's SetInitialFixture
    // does the slow STEP loading. The editor is constructed up front (but
    // not shown) so the load handler has something to drive; on cancel we
    // tear it back down, on OK we Show() it.
    //
    // The editor is still scaffolding for the rescan-on-save loop — see
    // the longer comment that previously lived here. Once the editor
    // reports a saved-fixture path back, this handler will roughly mirror
    // the old post-save logic (ScanFixturesFolder + walk m_fixturePaths
    // to select the new entry).
    CreateFixtureDialog createDlg(this);
    FixtureEditor* editor = new FixtureEditor(this);

    // Lambda owns nothing — captures `editor` and `createDlg` by reference
    // because both outlive the handler invocation (createDlg until
    // ShowModal returns, editor until either Show or Destroy below). The
    // progress callback is forwarded straight through into SetInitialFixture.
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
        // User cancelled before Create was pressed (no loading happened),
        // or closed the dialog via the title-row X. Tear down the editor
        // we speculatively constructed.
        editor->Destroy();
        return;
    }

    editor->Show();
}

void StartupDialog::OnOK(wxCommandEvent&)
{
    AcceptSelection();
}

void StartupDialog::AcceptSelection()
{
    if (!m_fixture.IsValid())
    {
        wxMessageBox("Please select a fixture before continuing.",
            "No Fixture Selected", wxOK | wxICON_WARNING, this);
        return;
    }

    // Procedural fixtures need their dimensions / clearances before we commit.
    // A cancel here leaves the picker open so the user can choose again.
    if (m_fixture.kind != FixtureKind::Library)
    {
        ProceduralFixtureDialog dlg(this, m_fixture);
        if (dlg.ShowModal() != wxID_OK)
            return;

        if (m_fixture.kind == FixtureKind::Parametric)
            m_fixture.parametric = dlg.GetParametric();
        else
            m_fixture.dynamic = dlg.GetDynamic();

        // Procedural fixtures allow perimeter injection by default — that's the
        // feature these box fixtures are meant to make usable.
        m_fixture.allowPerimeterInjection = true;
    }

    EndModal(wxID_OK);
}

void StartupDialog::RefreshPreview()
{
    if (m_fixture.kind == FixtureKind::Parametric)
    {
        m_lblModelA->SetLabel("Parametric box — fixed size, split at the parting plane");
        m_lblModelB->SetLabel("You'll set the X/Y/Z dimensions after Select.");
    }
    else if (m_fixture.kind == FixtureKind::Dynamic)
    {
        m_lblModelA->SetLabel("Dynamic box — auto-fits the scene with clearance");
        m_lblModelB->SetLabel("You'll set the per-axis clearance after Select.");
    }
    else if (m_fixture.IsValid())
    {
        m_lblModelA->SetLabel("Mould Half A: " + m_fixture.modelAPath);
        m_lblModelB->SetLabel("Mould Half B: " + m_fixture.modelBPath);
    }
    else
    {
        m_lblModelA->SetLabel("Mould Half A: —");
        m_lblModelB->SetLabel("Mould Half B: —");
    }
    Layout();
}

void StartupDialog::PreSelectFixture(const std::string& path)
{
    if (path.empty()) return;

    for (long i = 0; i < (long)m_fixturePaths.size(); ++i)
    {
        if (m_fixturePaths[i] == path)
        {
            m_list->SetItemState(i,
                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            m_list->EnsureVisible(i);

            std::string error;
            FixtureFile::Load(path, m_fixture, error);
            RefreshPreview();
            return;
        }
    }
}
