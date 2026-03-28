#include "display_thread.hpp"
#include "engine_types.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>

// ---------------------------------------------------------------------------
// Configurable constants
// ---------------------------------------------------------------------------
static constexpr float HIST_S              = 30.f;   // plot history window (seconds)
static constexpr float IMBAL_EMA_ALPHA     = 0.05f;  // imbalance EMA smoothing factor
static constexpr float BOOK_UPDATE_HZ      = 2.f;    // order book refresh rate (configurable)
static constexpr float BOOK_UPDATE_INTERVAL = 1.f / BOOK_UPDATE_HZ;
static constexpr float SLA_TARGET          = 5.0f;   // maximum acceptable latency

// ---------------------------------------------------------------------------
// Large font for the spread display (set in DisplayThread::run)
// ---------------------------------------------------------------------------
static ImFont* g_font_large = nullptr;

// ---------------------------------------------------------------------------
// Custom Y-axis formatter: 1000 → 1k, 1000000 → 1.0M
// ---------------------------------------------------------------------------
static int format_metric_suffix(double value, char* buf, int size, void*) {
    double abs_v = std::abs(value);
    if (abs_v >= 1e6)
        return std::snprintf(buf, size, "%.1fM", value / 1e6);
    if (abs_v >= 1e3)
        return std::snprintf(buf, size, "%.0fk", value / 1e3);
    return std::snprintf(buf, size, "%.0f", value);
}

// ---------------------------------------------------------------------------
// Scrolling ring buffer — one entry per display frame (~60 fps)
// ---------------------------------------------------------------------------
struct PlotRing {
    static constexpr int N = 5000;  // ~20 s at 60 fps
    float t[N]{};
    float bid[N]{}, ask[N]{}, vwmid[N]{};
    float micro_trend[N]{};    // to_display(ema) - vwmid
    float imbalance[N]{};
    float imbalance_ema[N]{};  // smoothed imbalance
    float tick_rate[N]{};
    float lat_p50[N]{};        // latency P50 µs
    float lat_p99[N]{};        // latency P99 µs
    float ring_occ[N]{};       // ring occupancy 0-100%
    int ins = 0;
    int sz  = 0;

    void push(float t_now, const BookSnapshot& s) {
        t[ins]           = t_now;
        bid[ins]         = (float)to_display(s.best_bid);
        ask[ins]         = (float)to_display(s.best_ask);
        vwmid[ins]       = (float)s.vwmid;
        micro_trend[ins] = (float)(to_display(s.ema) - s.vwmid);
        imbalance[ins]   = (float)s.imbalance;
        tick_rate[ins]   = (float)s.tick_rate;
        lat_p50[ins]     = (float)s.latency_p50_us;
        lat_p99[ins]     = (float)s.latency_p99_us;
        ring_occ[ins]    = (float)(s.ring_occupancy * 100.0);

        // Imbalance EMA (exponential smoothing across display frames)
        if (sz > 0) {
            int prev = (ins - 1 + N) % N;
            imbalance_ema[ins] = imbalance_ema[prev] * (1.f - IMBAL_EMA_ALPHA)
                               + (float)s.imbalance * IMBAL_EMA_ALPHA;
        } else {
            imbalance_ema[ins] = (float)s.imbalance;
        }

        ins = (ins + 1) % N;
        if (sz < N) ++sz;
    }

    int oldest() const { return sz < N ? 0 : ins; }
};

// ---------------------------------------------------------------------------
// Compute padded [lo, hi] over ring buffer with enforced minimum span
// ---------------------------------------------------------------------------
static std::pair<float,float> ring_range(const float* ys, int sz, int oldest,
                                          float min_span)
{
    if (sz == 0) return {0.f, min_span};
    float lo = ys[oldest % PlotRing::N], hi = lo;
    for (int k = 0; k < sz; ++k) {
        float v = ys[(oldest + k) % PlotRing::N];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    float center = (lo + hi) * 0.5f;
    float half   = std::max((hi - lo) * 0.5f * 1.15f, min_span * 0.5f);
    return {center - half, center + half};
}

// ---------------------------------------------------------------------------
// Plot 1: Best Bid + Best Ask + VWMID (three lines, legend on)
// ---------------------------------------------------------------------------
static void plot_price_trio(const BookSnapshot& s, const PlotRing& ring,
                             int sz, int oldest, float t_now, ImVec2 psz)
{
    char bbuf[64], abuf[64], vbuf[64];
    std::snprintf(bbuf, sizeof(bbuf), "Bid: %.6f", to_display(s.best_bid));
    std::snprintf(abuf, sizeof(abuf), "  Ask: %.6f", to_display(s.best_ask));
    std::snprintf(vbuf, sizeof(vbuf), "  VWMID: %.6f", s.vwmid);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.9f, 0.2f, 1.f));
    ImGui::TextUnformatted(bbuf);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.2f, 0.2f, 1.f));
    ImGui::TextUnformatted(abuf);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.8f, 1.0f, 1.f));
    ImGui::TextUnformatted(vbuf);
    ImGui::PopStyleColor();

    // 2. STATE FOR HYSTERESIS
    static float sticky_lo = 0, sticky_hi = 0;
    static float last_zoom_time = 0;

    // Get the actual min/max of the data in your history buffer
    auto [data_lo, data_hi] = ring_range(ring.vwmid, sz, oldest, 0.001f);
    float data_span = data_hi - data_lo;
    float current_view_span = sticky_hi - sticky_lo;

    // --- LOGIC STEP 1: EXPANSION (INSTANT) ---
    // If price escapes the "inner 80%" of our view, we expand immediately
    bool out_of_bounds = (s.vwmid < sticky_lo + (current_view_span * 0.1f)) || 
                         (s.vwmid > sticky_hi - (current_view_span * 0.1f));

    // --- LOGIC STEP 2: CONTRACTION (DELAYED) ---
    // If the data span is less than 50% of our current view for > 2 seconds, we zoom in
    bool should_contract = (data_span < current_view_span * 0.5f) && (t_now - last_zoom_time > 2.0f);

    if (sticky_lo == 0 || out_of_bounds || should_contract) {
        // Center the new view on the current mid
        // Add 25% padding so we don't have to resize again immediately
        float padding = std::max(data_span * 0.25f, 0.005f); 
        sticky_lo = data_lo - padding;
        sticky_hi = data_hi + padding;
        last_zoom_time = t_now;
    }

    if (ImPlot::BeginPlot("##trio", psz, ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
        ImPlot::SetupAxisLimits(ImAxis_X1, t_now - HIST_S, t_now, ImGuiCond_Always);
        
        // The Y-axis only changes when the 'if' block above triggers
        ImPlot::SetupAxisLimits(ImAxis_Y1, sticky_lo, sticky_hi, ImGuiCond_Always);

        if (sz > 1) {
            // Bid/Ask Lines (lower alpha)
            ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.9f, 0.2f, 0.4f), 1.0f);
            ImPlot::PlotLine("Bid", ring.t, ring.bid, sz, 0, oldest);
            ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.2f, 0.2f, 0.4f), 1.0f);
            ImPlot::PlotLine("Ask", ring.t, ring.ask, sz, 0, oldest);
            
            // Mid Line (solid/bold)
            ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), 2.0f);
            ImPlot::PlotLine("Mid", ring.t, ring.vwmid, sz, 0, oldest);
        }
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Plot 2: Microtrend — (EMA - VWMID) area oscillator
// ---------------------------------------------------------------------------
static void plot_microtrend(const BookSnapshot& s, const PlotRing& ring,
                             int sz, int oldest, float t_now, ImVec2 psz)
{
    double cur = to_display(s.ema) - s.vwmid;
    char lbuf[64];
    std::snprintf(lbuf, sizeof(lbuf), "EMA-VWMID: %+.6f", cur);
    ImGui::TextUnformatted(lbuf);

    auto [y_lo, y_hi] = ring_range(ring.micro_trend, sz, oldest, 0.f);
    float y_abs = std::max(std::abs(y_lo), std::abs(y_hi));
    if (y_abs < 1e-8f) y_abs = 1e-6f;
    y_lo = -y_abs * 1.2f;
    y_hi =  y_abs * 1.2f;

    static float pos_arr[PlotRing::N], neg_arr[PlotRing::N];
    for (int k = 0; k < sz; ++k) {
        int i = (oldest + k) % PlotRing::N;
        float v = ring.micro_trend[i];
        pos_arr[i] = v > 0.f ? v : 0.f;
        neg_arr[i] = v < 0.f ? v : 0.f;
    }

    if (ImPlot::BeginPlot("##micro", psz, ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
        ImPlot::SetupAxisLimits(ImAxis_X1, t_now - HIST_S, t_now, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_lo, y_hi, ImGuiCond_Always);

        // Prominent zero line
        float zx[2] = {t_now - HIST_S, t_now};
        float zy[2] = {0.f, 0.f};
        ImPlot::SetNextLineStyle(ImVec4(0.7f, 0.7f, 0.7f, 0.8f), 1.5f);
        ImPlot::PlotLine("##zero", zx, zy, 2);

        if (sz > 1) {
            ImPlot::SetNextFillStyle(ImVec4(0.2f, 0.9f, 0.2f, 0.55f));
            ImPlot::PlotShaded("##pos", ring.t, pos_arr, sz, 0.0, ImPlotShadedFlags_None, oldest);
            ImPlot::SetNextFillStyle(ImVec4(0.9f, 0.2f, 0.2f, 0.55f));
            ImPlot::PlotShaded("##neg", ring.t, neg_arr, sz, 0.0, ImPlotShadedFlags_None, oldest);
            ImPlot::SetNextLineStyle(ImVec4(0.85f, 0.85f, 0.85f, 0.4f), 1.0f);
            ImPlot::PlotLine("##line", ring.t, ring.micro_trend, sz, ImPlotLineFlags_None, oldest);
        }
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Plot 3: Imbalance — raw (thin/faint) + EMA (thick/bold)
// ---------------------------------------------------------------------------
static void plot_imbalance(const BookSnapshot& s, const PlotRing& ring,
                            int sz, int oldest, float t_now, ImVec2 psz)
{
    char lbuf[64];
    int prev = (ring.ins - 1 + PlotRing::N) % PlotRing::N;
    float ema_val = ring.sz > 0 ? ring.imbalance_ema[prev] : (float)s.imbalance;
    std::snprintf(lbuf, sizeof(lbuf), "Imbalance: %+.4f  EMA: %+.4f", s.imbalance, ema_val);
    ImGui::TextUnformatted(lbuf);

    if (ImPlot::BeginPlot("##imbal", psz, ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
        ImPlot::SetupAxisLimits(ImAxis_X1, t_now - HIST_S, t_now, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.1, 1.1, ImGuiCond_Always);
        if (sz > 1) {
            // Raw imbalance: thin, lower opacity line
            ImPlot::SetNextLineStyle(ImVec4(0.6f, 0.6f, 0.8f, 0.35f), 1.0f);
            ImPlot::PlotLine("Raw", ring.t, ring.imbalance, sz, ImPlotLineFlags_None, oldest);
            // EMA: thick, bold, high contrast
            ImPlot::SetNextLineStyle(ImVec4(0.95f, 0.85f, 0.2f, 1.0f), 2.5f);
            ImPlot::PlotLine("EMA", ring.t, ring.imbalance_ema, sz, ImPlotLineFlags_None, oldest);
        }
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Plot 4: Tick rate
// ---------------------------------------------------------------------------
static void plot_tickrate(const BookSnapshot& s, const PlotRing& ring,
                           int sz, int oldest, float t_now, ImVec2 psz)
{
    char lbuf[64];
    std::snprintf(lbuf, sizeof(lbuf), "Tick Rate: %.0f /s", s.tick_rate);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.f));
    ImGui::TextUnformatted(lbuf);
    ImGui::PopStyleColor();

    auto [y_lo, y_hi] = ring_range(ring.tick_rate, sz, oldest, 100.f);

    if (ImPlot::BeginPlot("##trate", psz, ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
        ImPlot::SetupAxisLimits(ImAxis_X1, t_now - HIST_S, t_now, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_lo, y_hi, ImGuiCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_Y1, format_metric_suffix);
        if (sz > 1) {
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.6f, 0.2f, 1.f), 1.5f);
            ImPlot::PlotLine("##tr", ring.t, ring.tick_rate, sz, ImPlotLineFlags_None, oldest);
        }
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Plot 5: E2E Latency — P50 and P99
// ---------------------------------------------------------------------------
static void plot_latency(const BookSnapshot& s, const PlotRing& ring,
                          int sz, int oldest, float t_now, ImVec2 psz)
{
    char lbuf[128];
    std::snprintf(lbuf, sizeof(lbuf), "P50: %.2f us  P99: %.2f us",
                  s.latency_p50_us, s.latency_p99_us);
    ImGui::TextUnformatted(lbuf);

    auto [_, hi99] = ring_range(ring.lat_p99, sz, oldest, 1.f);

    float y_limit = 30.0f;

    if (ImPlot::BeginPlot("##lat", psz, ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
        ImPlot::SetupAxisLimits(ImAxis_X1, t_now - HIST_S, t_now, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.f, y_limit, ImGuiCond_Always);

        float tx[2] = {t_now - HIST_S, t_now};
        float ty[2] = {SLA_TARGET, SLA_TARGET};

        ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 0.2f), 1.0f);
        ImPlot::PlotLine("Latency Target", tx, ty, 2);


        if (sz > 1) {
            ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.8f, 1.0f, 1.f), 1.5f);
            ImPlot::PlotLine("P50", ring.t, ring.lat_p50, sz, ImPlotLineFlags_None, oldest);
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.7f, 0.1f, 1.f), 1.5f);
            ImPlot::PlotLine("P99", ring.t, ring.lat_p99, sz, ImPlotLineFlags_None, oldest);
        }
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Plot 6: Queue depth — ring buffer occupancy area chart 0-100%
// ---------------------------------------------------------------------------
static void plot_queue_depth(const BookSnapshot& s, const PlotRing& ring,
                              int sz, int oldest, float t_now, ImVec2 psz)
{
    float pct = (float)(s.ring_occupancy * 100.0);
    ImVec4 col = (pct < 30.f)
        ? ImVec4(0.2f, 0.85f, 0.2f, 1.f)
        : (pct < 70.f ? ImVec4(0.95f, 0.8f, 0.1f, 1.f)
                      : ImVec4(0.9f,  0.2f, 0.2f, 1.f));

    char lbuf[64];
    std::snprintf(lbuf, sizeof(lbuf), "Queue: %.1f%%", pct);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(lbuf);
    ImGui::PopStyleColor();

    if (ImPlot::BeginPlot("##queue", psz, ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
        ImPlot::SetupAxisLimits(ImAxis_X1, t_now - HIST_S, t_now, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImGuiCond_Always);
        if (sz > 1) {
            ImPlot::SetNextFillStyle(col, 0.45f);
            ImPlot::PlotShaded("##fill", ring.t, ring.ring_occ, sz, 0.0,
                               ImPlotShadedFlags_None, oldest);
            ImPlot::SetNextLineStyle(col, 1.5f);
            ImPlot::PlotLine("##line", ring.t, ring.ring_occ, sz, ImPlotLineFlags_None, oldest);
        }
        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// Order book — 6-column table, spread centered in large font
// ---------------------------------------------------------------------------
static void render_book(const BookSnapshot& s) {
    // Centered, large spread
    {
        char spread_buf[64];
        std::snprintf(spread_buf, sizeof(spread_buf), "%.6f", to_display(s.spread));

        if (g_font_large) ImGui::PushFont(g_font_large);
        float text_w = ImGui::CalcTextSize(spread_buf).x;
        float label_w = ImGui::CalcTextSize("Spread: ").x;
        float total_w = label_w + text_w;
        float avail_w = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - total_w) * 0.5f);
        ImGui::Text("Spread: %s", spread_buf);
        if (g_font_large) ImGui::PopFont();
    }

    // Stats row
    ImGui::Text("Seq: %" PRIu64 "  BidQty: %" PRIu64 "  AskQty: %" PRIu64,
                s.event_sequence, s.total_bid_qty, s.total_ask_qty);
    ImGui::Separator();

    // Max qty for quantity bar scaling
    int64_t max_qty = 1;
    for (uint32_t i = 0; i < s.bid_level_count; ++i)
        max_qty = std::max(max_qty, s.bids[i].total_qty);
    for (uint32_t i = 0; i < s.ask_level_count; ++i)
        max_qty = std::max(max_qty, s.asks[i].total_qty);

    ImVec2      table_origin = ImGui::GetCursorScreenPos();
    float       table_w      = ImGui::GetContentRegionAvail().x;
    float       half_w       = table_w * 0.5f;
    ImDrawList* dl           = ImGui::GetWindowDrawList();

    if (ImGui::BeginTable("book", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("BID PRICE");
        ImGui::TableSetupColumn("QTY");
        ImGui::TableSetupColumn("ORD");
        ImGui::TableSetupColumn("ORD");
        ImGui::TableSetupColumn("QTY");
        ImGui::TableSetupColumn("ASK PRICE");
        ImGui::TableHeadersRow();

        for (uint32_t i = 0; i < SNAPSHOT_DEPTH; ++i) {
            ImGui::TableNextRow();

            // Quantity bars drawn behind this row
            ImGui::TableSetColumnIndex(0);
            float row_y = ImGui::GetCursorScreenPos().y;
            float row_h = ImGui::GetTextLineHeightWithSpacing();
            if (i < s.bid_level_count) {
                float frac  = (float)s.bids[i].total_qty / (float)max_qty;
                float bar_w = half_w * frac;
                dl->AddRectFilled(
                    ImVec2(table_origin.x + half_w - bar_w, row_y),
                    ImVec2(table_origin.x + half_w,         row_y + row_h),
                    IM_COL32(50, 220, 50, 35));
            }
            if (i < s.ask_level_count) {
                float frac  = (float)s.asks[i].total_qty / (float)max_qty;
                float bar_w = half_w * frac;
                dl->AddRectFilled(
                    ImVec2(table_origin.x + half_w,         row_y),
                    ImVec2(table_origin.x + half_w + bar_w, row_y + row_h),
                    IM_COL32(220, 50, 50, 35));
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.9f, 0.2f, 1.f));
            ImGui::TableSetColumnIndex(0);
            if (i < s.bid_level_count) ImGui::Text("%.6f", to_display(s.bids[i].price));
            ImGui::TableSetColumnIndex(1);
            if (i < s.bid_level_count) ImGui::Text("%" PRId64, s.bids[i].total_qty);
            ImGui::TableSetColumnIndex(2);
            if (i < s.bid_level_count) ImGui::Text("%u", s.bids[i].order_count);
            ImGui::PopStyleColor();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.2f, 0.2f, 1.f));
            ImGui::TableSetColumnIndex(3);
            if (i < s.ask_level_count) ImGui::Text("%u", s.asks[i].order_count);
            ImGui::TableSetColumnIndex(4);
            if (i < s.ask_level_count) ImGui::Text("%" PRId64, s.asks[i].total_qty);
            ImGui::TableSetColumnIndex(5);
            if (i < s.ask_level_count) ImGui::Text("%.6f", to_display(s.asks[i].price));
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// 3x2 grid of metric plots
// ---------------------------------------------------------------------------
static void render_plots(const BookSnapshot& s, const PlotRing& ring) {
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float gap     = 8.f;
    const float col_w   = (avail_w - 2.f * gap) / 3.f;
    const float lbl_h   = ImGui::GetTextLineHeightWithSpacing() + 2.f;
    const float row_h   = (avail_h - gap) / 2.f;
    const ImVec2 psz(col_w, row_h - lbl_h - 6.f);
    const float  t_now  = (float)glfwGetTime();
    const int    oldest = ring.oldest();
    const int    sz     = ring.sz;

    // Row 1: Bid/Ask/VWMID | Microtrend | Imbalance
    ImGui::BeginGroup();
    plot_price_trio(s, ring, sz, oldest, t_now, psz);
    ImGui::EndGroup();
    ImGui::SameLine(0.f, gap);
    ImGui::BeginGroup();
    plot_microtrend(s, ring, sz, oldest, t_now, psz);
    ImGui::EndGroup();
    ImGui::SameLine(0.f, gap);
    ImGui::BeginGroup();
    plot_imbalance(s, ring, sz, oldest, t_now, psz);
    ImGui::EndGroup();

    // Row 2: Tick Rate | Latency | Queue Depth
    ImGui::BeginGroup();
    plot_tickrate(s, ring, sz, oldest, t_now, psz);
    ImGui::EndGroup();
    ImGui::SameLine(0.f, gap);
    ImGui::BeginGroup();
    plot_latency(s, ring, sz, oldest, t_now, psz);
    ImGui::EndGroup();
    ImGui::SameLine(0.f, gap);
    ImGui::BeginGroup();
    plot_queue_depth(s, ring, sz, oldest, t_now, psz);
    ImGui::EndGroup();
}

// ---------------------------------------------------------------------------
// DisplayThread::run
// ---------------------------------------------------------------------------
void DisplayThread::run(const BookSnapshot& snapshot, volatile sig_atomic_t& running) {
    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "GLFW error %d: %s\n", code, desc);
    });

    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);  // no OS title bar / borders
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1600, 950, "Low Latency Engine", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync ~60 fps

#ifdef __APPLE__
    const char* glsl_version = "#version 150";
#else
    const char* glsl_version = "#version 330";
#endif

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    ImGui::StyleColorsDark();

    // Fonts: base (20px) + large for spread (32px)
    ImGuiIO& io = ImGui::GetIO();
#ifdef JETBRAINS_MONO_FONT
    io.Fonts->AddFontFromFileTTF(JETBRAINS_MONO_FONT, 20.0f);
    g_font_large = io.Fonts->AddFontFromFileTTF(JETBRAINS_MONO_FONT, 24.0f);
#endif
    ImGui::GetStyle().Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);

    BookSnapshot local{};
    BookSnapshot book_local{};
    PlotRing     ring{};
    double       last_book_update = 0.0;

    while (!glfwWindowShouldClose(window) && running) {
        glfwPollEvents();

        double t_now = glfwGetTime();
        if (snapshot_read(snapshot, local)) {
            ring.push((float)t_now, local);
            if (t_now - last_book_update >= BOOK_UPDATE_INTERVAL) {
                std::memcpy(
                    reinterpret_cast<char*>(&book_local)  + sizeof(std::atomic<uint64_t>),
                    reinterpret_cast<const char*>(&local) + sizeof(std::atomic<uint64_t>),
                    sizeof(BookSnapshot) - sizeof(std::atomic<uint64_t>));
                last_book_update = t_now;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Full-screen host window
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Low Latency Engine", nullptr,
            ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoMove      |
            ImGuiWindowFlags_NoCollapse  | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoScrollbar);

        const float avail_w = ImGui::GetContentRegionAvail().x;
        float       avail_h = ImGui::GetContentRegionAvail().y;
        const float depth_h = 210.f;

        // Custom drag bar — move, maximize/restore, close
        {
            const float bar_h = 26.f;
            const float btn_w = 26.f;
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.12f, 0.12f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.18f, 0.18f, 1.f));
            float title_w = avail_w - btn_w * 2.f - ImGui::GetStyle().ItemSpacing.x * 2.f;
            ImGui::Button("Low Latency Engine##drag", ImVec2(title_w, bar_h));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                int wx, wy;
                glfwGetWindowPos(window, &wx, &wy);
                glfwSetWindowPos(window, wx + (int)d.x, wy + (int)d.y);
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) glfwRestoreWindow(window);
                else                                              glfwMaximizeWindow(window);
            }
            ImGui::SameLine();
            if (ImGui::Button("[]", ImVec2(btn_w, bar_h))) {
                if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) glfwRestoreWindow(window);
                else                                              glfwMaximizeWindow(window);
            }
            ImGui::SameLine();
            if (ImGui::Button("X", ImVec2(btn_w, bar_h)))
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            ImGui::PopStyleColor(3);
            avail_h -= bar_h + ImGui::GetStyle().ItemSpacing.y;
        }

        // Top panel — order book (slow-updated at BOOK_UPDATE_HZ)
        ImGui::BeginChild("##depth", ImVec2(avail_w, depth_h), true);
        render_book(book_local);
        ImGui::EndChild();

        // Bottom panel — metric plots (live)
        ImGui::BeginChild("##plots", ImVec2(avail_w, avail_h - depth_h - 8.f), false);
        render_plots(local, ring);
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    g_font_large = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}
