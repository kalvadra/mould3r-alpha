#pragma once

#include <wx/colour.h>

// =============================================================================
// Mould3r — centralised UI colour palette
//
// Every wxColour used in the application lives here.  To re-skin the app,
// adjust the hex values below — no other file needs to change.
// =============================================================================

namespace Style
{

	// ---- Backgrounds -----------------------------------------------------------
	inline const wxColour AppBg(0x36, 0x40, 0x53);   // main app / ribbon background
	inline const wxColour SectionHeaderBg(0x25, 0x2B, 0x36);   // collapsible section header
	inline const wxColour CardBg(0x3D, 0x4B, 0x64);   // mould-tool card / panel
	inline const wxColour InputBg(0x2A, 0x30, 0x3C);   // text fields, browse buttons

	// ---- Buttons ---------------------------------------------------------------
	inline const wxColour BtnSecondary(0x79,0x92,0xC5);
	inline const wxColour BtnSecondaryHover(0x4A, 0x6F, 0xBA);
	inline const wxColour BtnSecondarySelected(0x32, 0x5F, 0xBC);
	inline const wxColour BtnSecondarySelectedBorder(0x1F, 0x24, 0x2E);

	inline const wxColour BtnDefault(0x2A, 0x30, 0x3C);   // default button / toggle off
	inline const wxColour BtnHover(0x38, 0x42, 0x52);   // button hover state
	inline const wxColour BtnActive(0x00, 0x7A, 0xCC);   // accent blue — active toggle
	inline const wxColour BtnPlace(0x79, 0x92, 0xC5);   // "Place …" button indigo
	inline const wxColour BtnSmall(0x3B, 0x40, 0x52);   // small action buttons / tool buttons

	inline const wxColour BtnGenerate(0x26, 0xAB, 0x36);   // "Generate Mould" green
	inline const wxColour BtnGenerateHover(0x07, 0x89, 0x17);   // "Generate Mould" green on hover

	// ---- Text ------------------------------------------------------------------
	inline const wxColour TextPrimary(0xFF, 0xFF, 0xFF);   // primary text / labels
	inline const wxColour TextActive(0xFF, 0xFF, 0xFF);   // active / selected text (white)
	inline const wxColour TextMuted(0xB0, 0xB8, 0xC8);   // muted labels (Edit/Remove/Clear)
	inline const wxColour TextSubtle(0xC3, 0xD1, 0xED);   // settings / sub-header text
	inline const wxColour TextDim(0x44, 0x55, 0x66);   // section titles, placeholder text
	inline const wxColour TextSubtext(0x66, 0x77, 0x88);   // subtitles (startup dialog)

	// ---- Accent / decoration ---------------------------------------------------
	inline const wxColour Accent(0x75, 0x86, 0xA7);   // accent blue (same as BtnActive)
	inline const wxColour Divider(0x2A, 0x38, 0x4A);   // separator / border lines

}  // namespace Style
