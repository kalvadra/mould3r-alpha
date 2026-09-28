#pragma once
// ===========================================================================
// FlowResultsBar — the strip under the Preview canvas: the owner's view
// controls (the Sim Viewer Select dropdown) at the left, always shown; then,
// when the view uses them, a playback timeline for the fill animation (play /
// pause, a scrubber that also traces the injection pressure over the fill, and
// a time readout) and a legend for what is on screen (the colour ramp, its
// range and units, plus swatches for the other colours; or a key of swatches
// only).
//
// It is a passive view: the owner sets the timeline and legend, and is told
// through onFrameChanged when the user (or playback) moves to another frame.
// Positions run 0..N where N = frame count; position N is "end of fill" (the
// result's final fields rather than a frame).
//
// Heatmap:: holds the colour ramp shared with the 3D heat-map views, so the
// legend and the model always agree.
// ===========================================================================

#include <wx/wx.h>
#include <wx/timer.h>

#include <functional>
#include <vector>

class RoundedButton;

namespace Heatmap
{
    constexpr int kBands = 12;

    // Cool (blue) -> warm (red) ramp, v in 0..1.
    inline void Ramp(float v, float& r, float& g, float& b)
    {
        static const float stops[5][3] = {
            { 0.20f, 0.32f, 0.90f }, { 0.20f, 0.80f, 0.90f }, { 0.30f, 0.85f, 0.35f },
            { 0.95f, 0.85f, 0.20f }, { 0.92f, 0.26f, 0.18f } };
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        const float s = v * 4.0f;
        int i = (int)s; if (i > 3) i = 3;
        const float f = s - (float)i;
        r = stops[i][0] * (1.0f - f) + stops[i + 1][0] * f;
        g = stops[i][1] * (1.0f - f) + stops[i + 1][1] * f;
        b = stops[i][2] * (1.0f - f) + stops[i + 1][2] * f;
    }
    // Colour of band b (0..kBands-1): the ramp at the band centre.
    inline void Band(int b, float& r, float& g, float& bl) { Ramp(((float)b + 0.5f) / (float)kBands, r, g, bl); }

    // Fixed colours for values the ramp doesn't cover.
    constexpr float kNoValue[3] = { 0.55f, 0.55f, 0.58f };   // unfilled / unpaired / unfed
    constexpr float kWall[3]    = { 0.55f, 0.30f, 0.30f };   // wall-flagged midplane facets

    inline wxColour ToColour(float r, float g, float b)
    {
        auto c = [](float x) { return (unsigned char)(x <= 0.0f ? 0 : (x >= 1.0f ? 255 : (int)(x * 255.0f + 0.5f))); };
        return wxColour(c(r), c(g), c(b));
    }
}

class FlowResultsBar : public wxPanel
{
public:
    explicit FlowResultsBar(wxWindow* parent);
    ~FlowResultsBar() override;

    struct Swatch { wxString label; wxColour colour; };
    struct Legend
    {
        bool     valid = false;
        wxString title;                 // "Fill time"
        wxString unit;                  // "s"
        float    lo = 0.0f, hi = 1.0f;  // the ramp's range
        int      decimals = -1;         // -1: from the range
        std::vector<Swatch> swatches;   // extra colours ("Unfilled", ...)
        bool     showRamp = true;       // false: a key of the swatches only
    };
    void SetLegend(const Legend& legend);                   // valid = false hides it

    // The left-hand controls area: create controls with ControlsParent() as
    // their parent, then AddControl them (laid out left to right).
    wxWindow* ControlsParent() const { return m_controls; }
    void AddControl(wxWindow* w, int flags = wxALIGN_CENTER_VERTICAL | wxRIGHT, int border = 8);

    // Timeline: per frame its time, filled share (%), inlet pressure and phase
    // (0 filling, 1 packing, 2 cooling); endTime = the end state's time; the
    // inlet-pressure history is drawn faintly along the track. Frames are
    // spaced evenly on the track (phases shaded). `key` identifies the data: a
    // call with the same key keeps the current position (so redraws don't
    // reset playback). Empty `times` hides the timeline (static views).
    struct TimelineData
    {
        long key = -1;
        std::vector<float> times, filledPct, inletMPa;
        std::vector<int>   phases;
        float endTime = 0.0f;
        std::vector<float> historyT, historyMPa;
        wxString endTitle = "End of fill";
        wxString endNote;
    };
    void SetTimeline(const TimelineData& data);
    void ClearTimeline();

    int  GetPosition() const { return m_pos; }             // 0..N (N = end of fill)
    int  GetEndPosition() const { return (int)m_times.size(); }
    bool AtEnd() const { return m_pos >= (int)m_times.size(); }
    void SetPosition(int pos, bool notify);
    bool HasTimeline() const { return !m_times.empty(); }

    void Stop();                                            // pause playback

    // Called with the new position whenever it changes (user or playback).
    std::function<void(int)> onFrameChanged;

private:
    class Timeline;
    class LegendView;

    void UpdateReadout();
    void Relayout();
    void OnPlay();
    void OnTimer(wxTimerEvent&);

    RoundedButton* m_playBtn = nullptr;
    Timeline*      m_timeline = nullptr;
    wxStaticText*  m_readout = nullptr;
    wxPanel*       m_timeGroup = nullptr;
    wxBoxSizer*    m_row = nullptr;
    wxPanel*       m_sep = nullptr;
    wxPanel*       m_sepC = nullptr;
    wxPanel*       m_controls = nullptr;
    wxSizerItem*   m_spacer = nullptr;
    bool           m_legendValid = false;
    LegendView*    m_legend = nullptr;
    wxTimer        m_timer;

    long m_key = -1;
    std::vector<float> m_times, m_filled, m_inlet, m_histT, m_histP;
    float    m_endTime = 0.0f;
    wxString m_endTitle = "End of fill", m_endNote;
    std::vector<int> m_phases;
    int      m_pos = 0;
};
