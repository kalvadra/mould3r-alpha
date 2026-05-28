#include "CreateFixtureDialog.h"
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <filesystem>
#include "style.h"
#include "RoundedButton.h"     // rounded button for Select / Create / Cancel
#include "WindowEffects.h"     // DWM corner rounding for the frameless dialog

namespace fs = std::filesystem;

namespace
{
    // Shared font for all dialog buttons — matches StartupDialog and the
    // ribbon buttons in MainFrame (9pt Segoe UI semibold).
    wxFont DialogBtnFont()
    {
        return wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI");
    }

    // Style a button as the "primary" action — indigo, white label,
    // semibold. Used here for the per-row Select buttons (same colour the
    // Import button in the main ribbon and StartupDialog's New Fixture
    // button use).
    void StylePrimaryButton(RoundedButton* btn)
    {
        btn->SetBackgroundColour(Style::BtnSecondary);
        btn->SetForegroundColour(*wxWHITE);
        btn->SetFont(DialogBtnFont());
    }

    // Style a button as the "confirm" action — green, white label.
    // Mirrors StartupDialog's Select button and the main ribbon's
    // "Generate Mould". Used here for Create.
    void StyleConfirmButton(RoundedButton* btn)
    {
        btn->SetBackgroundColour(Style::BtnGenerate);
        btn->SetForegroundColour(*wxWHITE);
        btn->SetFont(DialogBtnFont());
    }

    // Style a button to read as "text only" — background blended into the
    // dialog's AppBg so the rectangle disappears, leaving just a label.
    // Used for Cancel, same as StartupDialog's bottom-row Cancel.
    void StyleTextButton(RoundedButton* btn)
    {
        btn->SetBackgroundColour(Style::AppBg);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(DialogBtnFont());
    }

    // Field-label font (uppercase small caps look). 8pt bold matches
    // StartupDialog's "SELECTED FIXTURE" preview-card header.
    wxFont FieldLabelFont()
    {
        return wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI");
    }
}

CreateFixtureDialog::CreateFixtureDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(460, 410),
        wxBORDER_NONE)
{
    // Hairline sky-blue border, exactly as StartupDialog: the dialog's own
    // background acts as the border colour and contentPanel sits inside
    // with a 1-px inset. See StartupDialog.cpp for the longer comment on
    // Win10 vs Win11 corner-rounding behaviour.
    SetBackgroundColour(wxColour(0x6C, 0xB6, 0xF0));

    auto* contentPanel = new wxPanel(this, wxID_ANY);
    contentPanel->SetBackgroundColour(Style::AppBg);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(contentPanel, 1, wxEXPAND | wxALL, 1);  // 1-px hairline
    SetSizer(outer);

    auto* main = new wxBoxSizer(wxVERTICAL);

    // ---- Title row (stands in for the system title bar) -------------------
    // Same frameless approach as StartupDialog — see that file for the full
    // rationale and trade-offs (no system caption, no resize border,
    // platform-dependent corner rounding).
    m_titleRow = new wxPanel(contentPanel, wxID_ANY);
    m_titleRow->SetBackgroundColour(Style::AppBg);
    auto* titleRowSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* title = new wxStaticText(m_titleRow, wxID_ANY, "Create Fixture");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(wxFont(16, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));

    // Close ✕ — wxID_CANCEL so Escape and this button share the same exit
    // route via wxDialog's default Escape handler. The bottom-row Cancel
    // uses a private ID for the same reason StartupDialog does: to keep
    // Windows stock-button styling from interfering with the explicit
    // colours we set.
    auto* btnClose = new RoundedButton(m_titleRow, wxID_CANCEL,
        wxString::FromUTF8("\xE2\x9C\x95"),
        wxDefaultPosition, wxSize(30, 30), wxBORDER_NONE);
    btnClose->SetBackgroundColour(Style::AppBg);
    btnClose->SetForegroundColour(Style::TextMuted);
    btnClose->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

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

    // Drag bindings — title text consumes its own mouse events so it
    // needs its own LEFT_DOWN binding (motion/up/capture-lost only need
    // to be bound on m_titleRow since capture is taken there).
    m_titleRow->Bind(wxEVT_LEFT_DOWN, &CreateFixtureDialog::OnTitleMouseDown, this);
    m_titleRow->Bind(wxEVT_LEFT_UP, &CreateFixtureDialog::OnTitleMouseUp, this);
    m_titleRow->Bind(wxEVT_MOTION, &CreateFixtureDialog::OnTitleMouseMove, this);
    m_titleRow->Bind(wxEVT_MOUSE_CAPTURE_LOST, &CreateFixtureDialog::OnTitleCaptureLost, this);
    title->Bind(wxEVT_LEFT_DOWN, &CreateFixtureDialog::OnTitleMouseDown, this);

    // ---- Form panel (input fields + buttons) ------------------------------
    // Everything below the title row lives inside m_formPanel so OnCreate
    // can hide it in a single call when the load handler starts. Its
    // sibling m_loadingPanel (built further down) sits in the same sizer
    // slot and is shown in place.
    m_formPanel = new wxPanel(contentPanel, wxID_ANY);
    m_formPanel->SetBackgroundColour(Style::AppBg);
    auto* formSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Fixture name -----------------------------------------------------
    auto* lblName = new wxStaticText(m_formPanel, wxID_ANY, "FIXTURE NAME");
    lblName->SetForegroundColour(Style::TextSubtle);
    lblName->SetFont(FieldLabelFont());
    formSizer->Add(lblName, 0, wxLEFT | wxRIGHT | wxTOP, 18);

    m_nameCtrl = new wxTextCtrl(m_formPanel, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(-1, 28), wxBORDER_NONE);
    m_nameCtrl->SetHint("e.g. MyMould Rev 2");
    m_nameCtrl->SetBackgroundColour(Style::InputBg);
    m_nameCtrl->SetForegroundColour(Style::TextPrimary);
    m_nameCtrl->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    formSizer->Add(m_nameCtrl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 18);
    formSizer->AddSpacer(6);

    // ---- Section header ---------------------------------------------------
    auto* sectionHdr = new wxStaticText(m_formPanel, wxID_ANY,
        "Select Mould Halves");
    sectionHdr->SetForegroundColour(Style::TextPrimary);
    sectionHdr->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    formSizer->Add(sectionHdr, 0, wxLEFT | wxRIGHT | wxTOP, 18);
    formSizer->AddSpacer(4);

    // ---- Mould Half A ----------------------------------------------------
    // Pattern: small uppercase label on top, then a horizontal row with
    // the path text field (expanding) + Select button. Same shape repeats
    // for half B below — could be DRY'd into a helper, but inlining keeps
    // the layout obvious at a glance and these are the only two rows.
    auto* lblA = new wxStaticText(m_formPanel, wxID_ANY, "MOULD HALF A");
    lblA->SetForegroundColour(Style::TextSubtle);
    lblA->SetFont(FieldLabelFont());
    formSizer->Add(lblA, 0, wxLEFT | wxRIGHT | wxTOP, 18);

    auto* rowA = new wxBoxSizer(wxHORIZONTAL);
    m_pathACtrl = new wxTextCtrl(m_formPanel, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(-1, 28), wxBORDER_NONE);
    m_pathACtrl->SetBackgroundColour(Style::InputBg);
    m_pathACtrl->SetForegroundColour(Style::TextPrimary);
    m_pathACtrl->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    auto* btnSelectA = new RoundedButton(m_formPanel, ID_SelectA, "Select",
        wxDefaultPosition, wxSize(70, 28), wxBORDER_NONE);
    StylePrimaryButton(btnSelectA);

    rowA->Add(m_pathACtrl, 1, wxALIGN_CENTER_VERTICAL);
    rowA->Add(btnSelectA, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    formSizer->Add(rowA, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 18);

    // ---- Mould Half B ----------------------------------------------------
    auto* lblB = new wxStaticText(m_formPanel, wxID_ANY, "MOULD HALF B");
    lblB->SetForegroundColour(Style::TextSubtle);
    lblB->SetFont(FieldLabelFont());
    formSizer->Add(lblB, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    auto* rowB = new wxBoxSizer(wxHORIZONTAL);
    m_pathBCtrl = new wxTextCtrl(m_formPanel, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(-1, 28), wxBORDER_NONE);
    m_pathBCtrl->SetBackgroundColour(Style::InputBg);
    m_pathBCtrl->SetForegroundColour(Style::TextPrimary);
    m_pathBCtrl->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    auto* btnSelectB = new RoundedButton(m_formPanel, ID_SelectB, "Select",
        wxDefaultPosition, wxSize(70, 28), wxBORDER_NONE);
    StylePrimaryButton(btnSelectB);

    rowB->Add(m_pathBCtrl, 1, wxALIGN_CENTER_VERTICAL);
    rowB->Add(btnSelectB, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    formSizer->Add(rowB, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 18);

    // Stretch eats remaining vertical space — pushes the button row to
    // the bottom regardless of dialog-height tweaks.
    formSizer->AddStretchSpacer();

    // ---- Buttons ----------------------------------------------------------
    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnSizer->AddStretchSpacer();

    auto* btnCancel = new RoundedButton(m_formPanel, ID_Cancel, "Cancel",
        wxDefaultPosition, wxSize(90, 32), wxBORDER_NONE);
    StyleTextButton(btnCancel);

    auto* btnCreate = new RoundedButton(m_formPanel, ID_Create, "Create",
        wxDefaultPosition, wxSize(90, 32), wxBORDER_NONE);
    StyleConfirmButton(btnCreate);

    btnSizer->Add(btnCancel, 0, wxRIGHT, 8);
    btnSizer->Add(btnCreate, 0);
    formSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 18);

    m_formPanel->SetSizer(formSizer);

    // ---- Loading panel ----------------------------------------------------
    // Sibling of m_formPanel; initially hidden. Centred status text +
    // wxGauge, shown by ShowLoadingState() when Create succeeds. The
    // gauge background uses InputBg so the trough reads as part of the
    // same visual family as the form fields; the bar fill itself comes
    // from the Windows theme (wxGauge doesn't expose a fill colour).
    m_loadingPanel = new wxPanel(contentPanel, wxID_ANY);
    m_loadingPanel->SetBackgroundColour(Style::AppBg);
    m_loadingPanel->Hide();

    auto* loadingSizer = new wxBoxSizer(wxVERTICAL);
    loadingSizer->AddStretchSpacer();

    m_loadingStatus = new wxStaticText(m_loadingPanel, wxID_ANY,
        "Preparing...", wxDefaultPosition, wxDefaultSize,
        wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE);
    m_loadingStatus->SetForegroundColour(Style::TextPrimary);
    m_loadingStatus->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    loadingSizer->Add(m_loadingStatus, 0, wxEXPAND | wxLEFT | wxRIGHT, 24);
    loadingSizer->AddSpacer(14);

    m_loadingGauge = new wxGauge(m_loadingPanel, wxID_ANY, 100,
        wxDefaultPosition, wxSize(-1, 12),
        wxGA_HORIZONTAL | wxGA_SMOOTH | wxBORDER_NONE);
    m_loadingGauge->SetBackgroundColour(Style::InputBg);
    m_loadingGauge->SetForegroundColour(Style::BtnGenerate);
    loadingSizer->Add(m_loadingGauge, 0, wxEXPAND | wxLEFT | wxRIGHT, 32);

    loadingSizer->AddStretchSpacer();
    m_loadingPanel->SetSizer(loadingSizer);

    // Stack both panels into the same vertical slot. proportion=1 so
    // whichever is visible fills the area below the title row.
    main->Add(m_formPanel, 1, wxEXPAND);
    main->Add(m_loadingPanel, 1, wxEXPAND);

    contentPanel->SetSizer(main);
    CentreOnScreen();

    // Win11 corner rounding via DWM (no-op on Win10). Same idiom as
    // StartupDialog — see that file's matching call for the hairline-
    // border interaction note.
    WindowEffects::ApplyRoundedCorners(this);

    // Bindings. Cancel uses a private ID (rather than wxID_CANCEL) for the
    // same reason StartupDialog does — see that file's comment. The dialog
    // still ends with wxID_CANCEL so calling code stays unchanged.
    Bind(wxEVT_BUTTON, &CreateFixtureDialog::OnSelectModelA, this, ID_SelectA);
    Bind(wxEVT_BUTTON, &CreateFixtureDialog::OnSelectModelB, this, ID_SelectB);
    Bind(wxEVT_BUTTON, &CreateFixtureDialog::OnCreate, this, ID_Create);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); },
        ID_Cancel);
}

// ---------------------------------------------------------------------------
// Frameless drag handlers — identical to StartupDialog's. See that file's
// matching block for the full rationale (screen-space offset captured at
// down, motion as cursor − offset, capture-lost required to avoid debug
// asserts on Alt+Tab / system modals).
// ---------------------------------------------------------------------------
void CreateFixtureDialog::OnTitleMouseDown(wxMouseEvent&)
{
    m_dragOffset = wxGetMousePosition() - GetPosition();
    m_dragging = true;
    if (!m_titleRow->HasCapture())
        m_titleRow->CaptureMouse();
}

void CreateFixtureDialog::OnTitleMouseUp(wxMouseEvent&)
{
    if (m_dragging)
    {
        m_dragging = false;
        if (m_titleRow->HasCapture())
            m_titleRow->ReleaseMouse();
    }
}

void CreateFixtureDialog::OnTitleMouseMove(wxMouseEvent& evt)
{
    if (!m_dragging) { evt.Skip(); return; }
    SetPosition(wxGetMousePosition() - m_dragOffset);
}

void CreateFixtureDialog::OnTitleCaptureLost(wxMouseCaptureLostEvent&)
{
    m_dragging = false;
}

// ---------------------------------------------------------------------------
// Select handlers
// ---------------------------------------------------------------------------
void CreateFixtureDialog::PickModelPath(wxTextCtrl* target)
{
    // Default the open dialog to the directory of the field's current
    // value (if any) — so picking half B after half A doesn't drop the
    // user back at their home folder. If the field is empty or its parent
    // doesn't exist, wxFileDialog falls back to the OS default.
    wxString defaultDir;
    const wxString current = target->GetValue();
    if (!current.IsEmpty())
    {
        std::error_code ec;
        const auto parent = fs::path(current.ToStdString()).parent_path();
        if (!parent.empty() && fs::exists(parent, ec))
            defaultDir = parent.string();
    }

    // Filter matches the existing FixtureEditor import flow (STEP / IGES
    // are the formats LoadHalf understands via FileImporter).
    wxFileDialog dlg(this, "Select Mould Half",
        defaultDir, wxEmptyString,
        "STEP files (*.step;*.stp)|*.step;*.stp"
        "|IGES files (*.iges;*.igs)|*.iges;*.igs"
        "|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;

    target->SetValue(dlg.GetPath());
}

void CreateFixtureDialog::OnSelectModelA(wxCommandEvent&)
{
    PickModelPath(m_pathACtrl);
}

void CreateFixtureDialog::OnSelectModelB(wxCommandEvent&)
{
    PickModelPath(m_pathBCtrl);
}

// ---------------------------------------------------------------------------
// Create — validate, swap to loading view, invoke handler, end modal
// ---------------------------------------------------------------------------
void CreateFixtureDialog::OnCreate(wxCommandEvent&)
{
    // Capture into locals before validating so we can trim before the
    // non-empty check — Windows file pickers occasionally return paths
    // with surrounding whitespace on copy/paste.
    wxString name = m_nameCtrl->GetValue();  name.Trim(true).Trim(false);
    wxString pathA = m_pathACtrl->GetValue(); pathA.Trim(true).Trim(false);
    wxString pathB = m_pathBCtrl->GetValue(); pathB.Trim(true).Trim(false);

    if (name.IsEmpty())
    {
        wxMessageBox("Please enter a fixture name.",
            "Missing Information", wxOK | wxICON_WARNING, this);
        m_nameCtrl->SetFocus();
        return;
    }
    if (pathA.IsEmpty() || pathB.IsEmpty())
    {
        wxMessageBox("Please select both mould halves.",
            "Missing Information", wxOK | wxICON_WARNING, this);
        (pathA.IsEmpty() ? m_pathACtrl : m_pathBCtrl)->SetFocus();
        return;
    }

    // Existence check — catches the typed-a-bad-path-by-hand case (the
    // Select button's wxFD_FILE_MUST_EXIST already guarantees existence
    // for files chosen through it, but the text fields are editable).
    std::error_code ec;
    if (!fs::exists(pathA.ToStdString(), ec))
    {
        wxMessageBox("Mould Half A file does not exist:\n" + pathA,
            "File Not Found", wxOK | wxICON_ERROR, this);
        m_pathACtrl->SetFocus();
        return;
    }
    if (!fs::exists(pathB.ToStdString(), ec))
    {
        wxMessageBox("Mould Half B file does not exist:\n" + pathB,
            "File Not Found", wxOK | wxICON_ERROR, this);
        m_pathBCtrl->SetFocus();
        return;
    }

    m_fixtureName = name.ToStdString();
    m_modelAPath = pathA.ToStdString();
    m_modelBPath = pathB.ToStdString();

    // If a load handler is registered, swap to the progress view and run
    // it with a ProgressFn that forwards into UpdateProgress. Otherwise
    // (e.g. unit tests, or any caller that prefers to load after the
    // modal closes) just EndModal immediately — backwards compatible
    // with the pre-progress behaviour.
    if (m_loadHandler)
    {
        ShowLoadingState();
        // Bind UpdateProgress through `this` rather than copying the
        // pointer into the lambda — keeps the captured state minimal and
        // the lambda small.
        m_loadHandler([this](int percent, const std::string& status)
            {
                UpdateProgress(percent, status);
            });
    }

    EndModal(wxID_OK);
}

void CreateFixtureDialog::ShowLoadingState()
{
    m_formPanel->Hide();
    m_loadingPanel->Show();
    Layout();
    // Force a paint pass so the user sees the progress view before the
    // (synchronous) load handler starts churning. wxYield gives the
    // wx event loop a chance to process the pending paint event;
    // without it the swap and the first long load would happen back-to-
    // back on the same UI thread tick, and the user would only see the
    // progress view appear when the load is already done.
    wxYield();
}

void CreateFixtureDialog::UpdateProgress(int percent, const std::string& status)
{
    // Clamp — wxGauge::SetValue would assert on out-of-range in debug,
    // and silently clamping is the more forgiving behaviour for callers
    // that compute percent from a fractional count.
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (m_loadingGauge)
        m_loadingGauge->SetValue(percent);

    // Empty status means "leave it as-is" — lets callers issue progress
    // ticks without re-setting the label every time.
    if (!status.empty() && m_loadingStatus)
        m_loadingStatus->SetLabel(status);

    // Yield so the gauge and label repaint before the handler continues
    // with the next chunk of synchronous work. Same rationale as in
    // ShowLoadingState.
    wxYield();
}
