#include "oauth.h"
#include "credential_store.h"
#include "diagnostics.h"
#include "json_util.h"
#include "platform.h"
#include "usage.h"
#include "svg_icons.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_tray.h>
#include <SDL3_ttf/SDL_ttf.h>

#if defined(_WIN32)
#include <windows.h>
#include <dwmapi.h>
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifdef small
#undef small
#endif
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kCalloutWidth = 336;
constexpr int kDockWidth = 92;
constexpr int kTailWidth = 10;
constexpr int kCardGap = 8;
constexpr int kPanelWidth = kCalloutWidth + kCardGap + kTailWidth + kDockWidth;
constexpr int kDockPad = 14;
constexpr int kRingSize = 52;
constexpr int kRingSlot = 78;
constexpr int kDockFooter = 56;
constexpr int kCalloutHeight = 168;
constexpr int kSettingsRowHeight = 52;
constexpr int kSettingsHeader = 48;
constexpr int kSettingsExtra = 44;
constexpr int kSettingsFooter = 56;
constexpr int kProviderCount = 5;
constexpr int kCardRadius = 20;
constexpr int kDockRadius = 24;

struct Rect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct ProviderState {
    bool busy = false;
    unsigned long long operation_id = 0;
    bool logged_in = false;
    std::string status = "Not logged in";
    std::string account;
    std::string account_label;
    std::string primary_row = "Current session";
    std::string secondary_row = "All models";
    std::string tertiary_row = "Requests";
    bool primary_available = true;
    bool secondary_available = true;
    bool tertiary_available = false;
    double primary_used = 0;
    double secondary_used = 0;
    double tertiary_used = 0;
    long long primary_reset = 0;
    long long secondary_reset = 0;
    long long tertiary_reset = 0;
    long long last_refresh_ms = 0;
};

struct AppState {
    std::mutex mutex;
    ProviderState providers[kProviderCount];
    bool enabled[kProviderCount] = {true, true, false, false, true};
    int selected = 0;
};

struct UiState {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Tray* tray = nullptr;
    SDL_Surface* icon = nullptr;
    TTF_Font* font = nullptr;
    TTF_Font* font_bold = nullptr;
    TTF_Font* font_small = nullptr;
    TTF_Font* font_small_bold = nullptr;
    std::filesystem::path font_path;
    std::filesystem::path font_bold_path;
    float panel_scale = 1.0f;
    float render_scale = 1.0f;
    float font_scale = 0.0f;
    bool visible = false;
    bool pinned = false;
    bool callout_open = true;
    bool settings_open = false;
    bool api_key_mode = false;
    bool api_input_focused = false;
    bool oauth_code_mode = false;
    bool oauth_code_input_focused = false;
    bool dragging = false;
    bool drag_moved = false;
    int drag_offset_x = 0;
    int drag_offset_y = 0;
    int panel_height = 280;
    int target_height = 280;
    int anchor_bottom = 0;
    int anchor_right = 0;
    long long shown_at_ms = 0;
    std::string api_key_input;
    std::string oauth_code_input;
    OAuthLoginSession oauth_session;
    Rect dock_rect, callout_rect;
    Rect ring_slots[kProviderCount];
    Rect pin_button, gear_button, callout_pin_button;
    Rect model_callout_rect[kProviderCount];
    Rect model_pin_button[kProviderCount];
    bool model_open[kProviderCount]{};
    bool model_pinned[kProviderCount]{};
    bool model_detached[kProviderCount]{};
    float model_callout_y[kProviderCount]{-1, -1, -1, -1, -1};
    bool show_remaining = false;
    int dragging_model = -1;
    Rect settings_toggle[kProviderCount];
    Rect settings_action[kProviderCount];
    Rect settings_quit, settings_refresh, settings_fill_toggle;
    Rect api_input, api_ok, api_cancel;
    Rect oauth_code_input_box, oauth_code_ok, oauth_code_cancel;
    float used_anim[kProviderCount]{};
    float hover_anim[kProviderCount]{};
    float gear_hot = 0;
    float pin_hot = 0;
    float left_anim = 0;
    float card_y_anim = 0;
    int hover_ring = -1;
    bool gear_hovered = false;
    bool pin_hovered = false;
    bool prev_global_down = false;
    bool click_armed = false;
    SDL_BlendMode premul = SDL_BLENDMODE_BLEND;
};

AppState g_app;
UiState g_ui;
std::atomic_bool g_quit{false};
std::atomic_bool g_show_requested{false};
std::atomic_bool g_refresh_requested{false};
std::atomic_bool g_warm_requested{false};

int layout_width() {
    return kPanelWidth;
}
int panel_width_px() { return static_cast<int>(std::round(layout_width() * g_ui.panel_scale)); }
int panel_height_px() { return static_cast<int>(std::round(g_ui.panel_height * g_ui.panel_scale)); }

float window_panel_scale() {
    SDL_Rect bounds{};
    SDL_DisplayID display = SDL_GetDisplayForWindow(g_ui.window);
    float resolution_scale = display && SDL_GetDisplayBounds(display, &bounds) ? std::clamp(static_cast<float>(bounds.h) / 1440.0f, 1.0f, 1.5f) : 1.0f;
    return std::max(SDL_GetWindowDisplayScale(g_ui.window), resolution_scale);
}

void window_to_logical(float wx, float wy, float* lx, float* ly) {
    if (!g_ui.renderer || !SDL_RenderCoordinatesFromWindow(g_ui.renderer, wx, wy, lx, ly)) {
        *lx = wx / std::max(0.01f, g_ui.panel_scale);
        *ly = wy / std::max(0.01f, g_ui.panel_scale);
    }
}

void event_logical(SDL_Event& event, float* x, float* y) {
    SDL_ConvertEventToRenderCoordinates(g_ui.renderer, &event);
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        *x = event.motion.x;
        *y = event.motion.y;
    } else {
        *x = event.button.x;
        *y = event.button.y;
    }
}

float approach(float value, float target, float dt, float rate = 14.0f) {
    float t = 1.0f - std::exp(-rate * dt);
    return value + (target - value) * t;
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* provider_key(int index) {
    if (index == 4) return "grok";
    if (index == 3) return "gemini";
    if (index == 2) return "glm";
    return index == 1 ? "anthropic" : "openai";
}

const char* provider_label(int index) {
    if (index == 4) return "Grok";
    if (index == 3) return "Gemini";
    if (index == 2) return "GLM";
    return index == 1 ? "Claude" : "GPT";
}

const char* primary_row_label(int index) {
    if (index == 4) return "Grok CLI";
    if (index == 2) return "5 hour";
    if (index == 3) return "5 hour";
    return "Current session";
}

const char* secondary_row_label(int index) {
    if (index == 4) return "Grok Bot";
    if (index == 2) return "Weekly";
    if (index == 3) return "Weekly";
    return "All models";
}

SDL_Color provider_accent(int index) {
    if (index == 1) return SDL_Color{232, 114, 42, 255};
    if (index == 2) return SDL_Color{45, 212, 191, 255};
    if (index == 3) return SDL_Color{168, 139, 250, 255};
    if (index == 4) return SDL_Color{236, 236, 239, 255};
    return SDL_Color{232, 232, 234, 255};
}

bool provider_has_auth(int index) {
    return index == 2 ? load_api_key_provider("glm").has_value() : load_credentials_provider(provider_key(index)).has_value();
}

std::string trim_copy(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), value.end());
    return value;
}

int selected_provider() {
    std::lock_guard<std::mutex> lock(g_app.mutex);
    return g_app.selected;
}

std::string pretty_plan(std::string plan) {
    if (plan == "prolite") return "Pro";
    if (plan.empty()) return "unknown";
    plan[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(plan[0])));
    return plan;
}

bool contains(Rect r, float x, float y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

float dock_x() {
    return static_cast<float>(kCalloutWidth + kCardGap + kTailWidth);
}
int settings_height();

bool left_sheet_open() {
    return g_ui.settings_open || g_ui.api_key_mode || g_ui.oauth_code_mode;
}

void sync_callout_open() {
    g_ui.callout_open = false;
    for (int i = 0; i < kProviderCount; ++i) if (g_ui.model_open[i]) g_ui.callout_open = true;
}

bool any_model_pinned_open() {
    for (int i = 0; i < kProviderCount; ++i) if (g_ui.model_open[i] && g_ui.model_pinned[i]) return true;
    return false;
}

bool model_is_snapped(int index) {
    return g_ui.model_open[index] && !g_ui.model_pinned[index] && !g_ui.model_detached[index];
}

double display_percent(double used) {
    double value = g_ui.show_remaining ? 100.0 - used : used;
    return std::clamp(value, 0.0, 100.0);
}

float ring_cy_for(int index) {
    bool enabled[kProviderCount]{};
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        for (int i = 0; i < kProviderCount; ++i) enabled[i] = g_app.enabled[i];
    }
    int n = 0;
    for (int i = 0; i < kProviderCount; ++i) {
        if (!enabled[i]) continue;
        if (i == index) return static_cast<float>(kDockPad + n * kRingSlot + kRingSize * 0.5f);
        ++n;
    }
    return static_cast<float>(kDockPad + kRingSize * 0.5f);
}

float snap_callout_y(int index) {
    float h = static_cast<float>(kCalloutHeight);
    return std::clamp(ring_cy_for(index) - h * 0.5f, 0.0f, std::max(0.0f, static_cast<float>(g_ui.panel_height) - h));
}

float callout_y_for(int index) {
    float h = static_cast<float>(kCalloutHeight);
    if ((g_ui.dragging_model == index || g_ui.model_pinned[index] || g_ui.model_detached[index]) && g_ui.model_callout_y[index] >= 0) {
        return std::clamp(g_ui.model_callout_y[index], 0.0f, std::max(0.0f, static_cast<float>(g_ui.panel_height) - h));
    }
    return snap_callout_y(index);
}

void left_card_geom(float* y, float* h) {
    bool sheet = left_sheet_open();
    *h = sheet ? static_cast<float>(settings_height()) : static_cast<float>(kCalloutHeight);
    if (sheet) {
        *y = 0;
        return;
    }
    *y = callout_y_for(selected_provider());
}

int enabled_count() {
    int n = 0;
    for (int i = 0; i < kProviderCount; ++i) if (g_app.enabled[i]) ++n;
    return n;
}

int dock_height() {
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        n = enabled_count();
    }
    if (n < 1) n = 1;
    return kDockPad + n * kRingSlot + kDockFooter;
}

int settings_height() {
    return kSettingsHeader + kProviderCount * kSettingsRowHeight + kSettingsExtra + kSettingsFooter;
}

std::tm localtime_portable(std::time_t t) {
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return local;
}

std::string format_reset_phrase(long long reset_at) {
    if (reset_at <= 0) return "";
    std::time_t now = static_cast<std::time_t>(now_ms() / 1000);
    std::time_t reset = static_cast<std::time_t>(reset_at);
    long long delta = static_cast<long long>(reset) - static_cast<long long>(now);
    if (delta <= 0) return "Resetting";
    if (delta < 2 * 3600) {
        int minutes = static_cast<int>((delta + 59) / 60);
        return "Resets in " + std::to_string(std::max(1, minutes)) + " min";
    }
    std::tm local = localtime_portable(reset);
    std::ostringstream out;
    out << "Resets " << std::put_time(&local, "%a, ");
    if (local.tm_mday < 10) out << local.tm_mday;
    else out << std::put_time(&local, "%d");
    out << std::put_time(&local, " %b at %H:%M");
    return out.str();
}

bool over_click_target(float x, float y) {
    if (g_ui.api_key_mode) {
        return contains(g_ui.api_input, x, y) || contains(g_ui.api_ok, x, y) || contains(g_ui.api_cancel, x, y);
    }
    if (g_ui.oauth_code_mode) {
        return contains(g_ui.oauth_code_input_box, x, y) || contains(g_ui.oauth_code_ok, x, y) || contains(g_ui.oauth_code_cancel, x, y);
    }
    if (contains(g_ui.pin_button, x, y) || contains(g_ui.gear_button, x, y) || contains(g_ui.callout_pin_button, x, y)) return true;
    for (int i = 0; i < kProviderCount; ++i) {
        if (contains(g_ui.ring_slots[i], x, y) || contains(g_ui.model_pin_button[i], x, y)) return true;
    }
    if (g_ui.settings_open) {
        for (int i = 0; i < kProviderCount; ++i) {
            if (contains(g_ui.settings_toggle[i], x, y) || contains(g_ui.settings_action[i], x, y)) return true;
        }
        return contains(g_ui.settings_quit, x, y) || contains(g_ui.settings_refresh, x, y) || contains(g_ui.settings_fill_toggle, x, y) || contains(g_ui.callout_rect, x, y);
    }
    return false;
}

bool over_widget(float x, float y) {
    if (contains(g_ui.dock_rect, x, y)) return true;
    if (left_sheet_open() && g_ui.left_anim > 0.05f && contains(g_ui.callout_rect, x, y)) return true;
    for (int i = 0; i < kProviderCount; ++i) {
        if (g_ui.model_open[i] && g_ui.left_anim > 0.05f && contains(g_ui.model_callout_rect[i], x, y)) return true;
    }
    return false;
}

SDL_HitTestResult SDLCALL hit_test(SDL_Window*, const SDL_Point* area, void*) {
    float lx = 0, ly = 0;
    window_to_logical(static_cast<float>(area->x), static_cast<float>(area->y), &lx, &ly);
    return over_click_target(lx, ly) ? SDL_HITTEST_NORMAL : SDL_HITTEST_DRAGGABLE;
}

void fill_surface_rect(SDL_Surface* surface, SDL_Rect rect, Uint32 color) {
    SDL_FillSurfaceRect(surface, &rect, color);
}

void fill_surface_round(SDL_Surface* surface, int width, int height, int radius, Uint32 value) {
    SDL_Rect middle{radius, 0, width - radius * 2, height};
    SDL_Rect body{0, radius, width, height - radius * 2};
    fill_surface_rect(surface, middle, value);
    fill_surface_rect(surface, body, value);
    for (int y = 0; y < radius; ++y) {
        float dy = static_cast<float>(radius - y) - 0.5f;
        int dx = static_cast<int>(std::sqrt(std::max(0.0f, static_cast<float>(radius * radius) - dy * dy)));
        SDL_Rect top{radius - dx, y, width - (radius - dx) * 2, 1};
        SDL_Rect bottom{radius - dx, height - y - 1, width - (radius - dx) * 2, 1};
        fill_surface_rect(surface, top, value);
        fill_surface_rect(surface, bottom, value);
    }
}

void fill_surface_round_rect(SDL_Surface* surface, SDL_Rect rect, int radius, Uint32 value) {
    SDL_Rect middle{rect.x + radius, rect.y, rect.w - radius * 2, rect.h};
    SDL_Rect body{rect.x, rect.y + radius, rect.w, rect.h - radius * 2};
    fill_surface_rect(surface, middle, value);
    fill_surface_rect(surface, body, value);
    for (int y = 0; y < radius; ++y) {
        float dy = static_cast<float>(radius - y) - 0.5f;
        int dx = static_cast<int>(std::sqrt(std::max(0.0f, static_cast<float>(radius * radius) - dy * dy)));
        SDL_Rect top{rect.x + radius - dx, rect.y + y, rect.w - (radius - dx) * 2, 1};
        SDL_Rect bottom{rect.x + radius - dx, rect.y + rect.h - y - 1, rect.w - (radius - dx) * 2, 1};
        fill_surface_rect(surface, top, value);
        fill_surface_rect(surface, bottom, value);
    }
}

int px(float value) { return static_cast<int>(std::round(value * g_ui.panel_scale)); }

void update_window_shape() {
#if defined(_WIN32)
    return;
#else
    if (!g_ui.window) return;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_ui.window, &pw, &ph);
    if (pw < 8 || ph < 8) {
        pw = panel_width_px();
        ph = panel_height_px();
    }
    SDL_Surface* shape = SDL_CreateSurface(pw, ph, SDL_PIXELFORMAT_RGBA32);
    if (!shape) return;
    SDL_ClearSurface(shape, 0, 0, 0, 0);
    Uint32 on = SDL_MapSurfaceRGBA(shape, 255, 255, 255, 255);
    float sx = static_cast<float>(pw) / static_cast<float>(std::max(1, kPanelWidth));
    float sy = static_cast<float>(ph) / static_cast<float>(std::max(1, g_ui.panel_height));
    auto SX = [&](float v) { return static_cast<int>(std::lround(v * sx)); };
    auto SY = [&](float v) { return static_cast<int>(std::lround(v * sy)); };
    int dock_h = std::max(dock_height(), g_ui.callout_open && !left_sheet_open() ? kCalloutHeight : 0);
    SDL_Rect dock{SX(dock_x()), 0, SX(static_cast<float>(kDockWidth)), SY(static_cast<float>(dock_h))};
    fill_surface_round_rect(shape, dock, std::max(1, SX(static_cast<float>(kDockRadius))), on);
    if (g_ui.callout_open || left_sheet_open()) {
        float card_y = 0, card_h = 0;
        left_card_geom(&card_y, &card_h);
        SDL_Rect callout{0, SY(card_y), SX(static_cast<float>(kCalloutWidth)), SY(card_h)};
        fill_surface_round_rect(shape, callout, std::max(1, SX(static_cast<float>(kCardRadius))), on);
        int mid = SY(card_y + card_h * 0.5f);
        int tail_x = SX(static_cast<float>(kCalloutWidth)) - 1;
        int tail_w = std::max(1, SX(static_cast<float>(kTailWidth + kCardGap)));
        for (int x = 0; x < tail_w; ++x) {
            int spread = std::max(1, SY(7.0f) - x * SY(7.0f) / tail_w);
            SDL_Rect sliver{tail_x + x, mid - spread, 1, spread * 2};
            fill_surface_rect(shape, sliver, on);
        }
    }
    SDL_SetWindowShape(g_ui.window, shape);
    SDL_DestroySurface(shape);
#endif
}

void close_fonts() {
    if (g_ui.font_small_bold) { TTF_CloseFont(g_ui.font_small_bold); g_ui.font_small_bold = nullptr; }
    if (g_ui.font_small) { TTF_CloseFont(g_ui.font_small); g_ui.font_small = nullptr; }
    if (g_ui.font_bold) { TTF_CloseFont(g_ui.font_bold); g_ui.font_bold = nullptr; }
    if (g_ui.font) { TTF_CloseFont(g_ui.font); g_ui.font = nullptr; }
}

bool load_fonts_for_scale(float scale) {
    close_fonts();
    float point_size = std::max(15.0f, std::round(15.0f * scale));
    float small_size = std::max(11.0f, std::round(11.0f * scale));
    g_ui.font = TTF_OpenFont(g_ui.font_path.string().c_str(), point_size);
    g_ui.font_bold = TTF_OpenFont(g_ui.font_bold_path.string().c_str(), point_size);
    g_ui.font_small = TTF_OpenFont(g_ui.font_path.string().c_str(), small_size);
    g_ui.font_small_bold = TTF_OpenFont(g_ui.font_bold_path.string().c_str(), small_size);
    g_ui.font_scale = scale;
    return g_ui.font != nullptr;
}

void update_render_metrics(bool reload_fonts = true) {
    if (!g_ui.window || !g_ui.renderer) return;
    SDL_SetRenderLogicalPresentation(g_ui.renderer, layout_width(), g_ui.panel_height, SDL_LOGICAL_PRESENTATION_STRETCH);

    int ww = 0, wh = 0, rw = 0, rh = 0;
    SDL_GetWindowSize(g_ui.window, &ww, &wh);
    SDL_GetRenderOutputSize(g_ui.renderer, &rw, &rh);
    float sx = ww > 0 ? static_cast<float>(rw) / static_cast<float>(ww) : 1.0f;
    float sy = wh > 0 ? static_cast<float>(rh) / static_cast<float>(wh) : sx;
    g_ui.render_scale = std::max(1.0f, std::max(sx, sy));
    if (reload_fonts && !g_ui.font_path.empty() && std::abs(g_ui.render_scale - g_ui.font_scale) > 0.05f) {
        load_fonts_for_scale(g_ui.render_scale);
    }
}

int wanted_panel_height() {
    int dock = dock_height();
    if (g_ui.settings_open || g_ui.api_key_mode || g_ui.oauth_code_mode) return std::max(dock, settings_height());
    int height = dock;
    for (int i = 0; i < kProviderCount; ++i) {
        if (!g_ui.model_open[i]) continue;
        height = std::max(height, static_cast<int>(std::ceil(callout_y_for(i) + static_cast<float>(kCalloutHeight) + 8.0f)));
    }
    return height;
}

void anchor_current_bottom() {
    int wx = 0;
    int wy = 0;
    SDL_GetWindowPosition(g_ui.window, &wx, &wy);
    g_ui.anchor_bottom = wy + panel_height_px();
}

void set_target_height(int height, bool immediate = false) {
    if (g_ui.visible && g_ui.anchor_bottom <= 0) anchor_current_bottom();
    int old_w = panel_width_px();
    g_ui.target_height = height;
    if (!immediate) {
        SDL_SetWindowSize(g_ui.window, panel_width_px(), panel_height_px());
        update_render_metrics();
        int new_w = panel_width_px();
        if (g_ui.visible && new_w != old_w) {
            int wx = 0, wy = 0;
            SDL_GetWindowPosition(g_ui.window, &wx, &wy);
            SDL_SetWindowPosition(g_ui.window, wx + old_w - new_w, wy);
        }
        update_window_shape();
        return;
    }
    g_ui.panel_height = height;
    SDL_SetWindowSize(g_ui.window, panel_width_px(), panel_height_px());
    update_render_metrics();
    if (g_ui.anchor_bottom > 0) {
        int wx = 0;
        int wy = 0;
        SDL_GetWindowPosition(g_ui.window, &wx, &wy);
        SDL_SetWindowPosition(g_ui.window, wx, g_ui.anchor_bottom - panel_height_px());
    }
    update_window_shape();
}

void close_menus() {
    g_ui.settings_open = false;
    for (int i = 0; i < kProviderCount; ++i) {
        if (g_ui.model_open[i] && !g_ui.model_pinned[i]) {
            g_ui.model_open[i] = false;
            g_ui.model_detached[i] = false;
        }
    }
    sync_callout_open();
    set_target_height(wanted_panel_height());
}

void show_panel() {
    g_show_requested = false;
    g_ui.visible = true;
    g_ui.shown_at_ms = now_ms();
    g_ui.settings_open = false;
    g_ui.callout_open = false;
    for (int i = 0; i < kProviderCount; ++i) {
        if (g_ui.model_pinned[i]) {
            g_ui.model_open[i] = true;
            g_ui.callout_open = true;
        } else {
            g_ui.model_open[i] = false;
            g_ui.model_detached[i] = false;
        }
    }
    g_ui.prev_global_down = true;
    g_ui.panel_height = wanted_panel_height();
    g_ui.target_height = g_ui.panel_height;
    SDL_SetWindowSize(g_ui.window, panel_width_px(), panel_height_px());
    update_render_metrics();
    float mx = 0, my = 0;
    SDL_GetGlobalMouseState(&mx, &my);
    g_ui.anchor_bottom = static_cast<int>(my) - static_cast<int>(std::round(12 * g_ui.panel_scale));
    SDL_SetWindowPosition(g_ui.window, static_cast<int>(mx) - panel_width_px() + static_cast<int>(std::round(20 * g_ui.panel_scale)), g_ui.anchor_bottom - panel_height_px());
    update_window_shape();
    SDL_ShowWindow(g_ui.window);
    SDL_RaiseWindow(g_ui.window);
}

void hide_panel() {
    if (g_ui.api_key_mode || g_ui.oauth_code_mode) return;
    if (g_ui.pinned || any_model_pinned_open()) {
        g_ui.settings_open = false;
        for (int i = 0; i < kProviderCount; ++i) {
            if (g_ui.model_open[i] && !g_ui.model_pinned[i]) {
                g_ui.model_open[i] = false;
                g_ui.model_detached[i] = false;
            }
        }
        sync_callout_open();
        set_target_height(wanted_panel_height());
        return;
    }
    g_ui.visible = false;
    g_ui.callout_open = false;
    g_ui.settings_open = false;
    for (int i = 0; i < kProviderCount; ++i) {
        g_ui.model_open[i] = false;
        g_ui.model_detached[i] = false;
    }
    SDL_HideWindow(g_ui.window);
}

void polish_native_window() {
#if defined(_WIN32)
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(g_ui.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TOOLWINDOW;
    ex &= ~(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_APPWINDOW);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    COLORREF border = RGB(18, 18, 20);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &border, sizeof(border));
    int corners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    SDL_SetWindowFocusable(g_ui.window, true);
#endif
}

void tick_ui(float dt) {
    double used[kProviderCount]{};
    bool logged[kProviderCount]{};
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        for (int i = 0; i < kProviderCount; ++i) {
            logged[i] = g_app.providers[i].logged_in;
            used[i] = logged[i] ? g_app.providers[i].primary_used : 0;
        }
    }
    for (int i = 0; i < kProviderCount; ++i) {
        g_ui.used_anim[i] = approach(g_ui.used_anim[i], static_cast<float>(used[i]), dt, 10.0f);
        g_ui.hover_anim[i] = approach(g_ui.hover_anim[i], g_ui.hover_ring == i ? 1.0f : 0.0f, dt, 16.0f);
    }
    g_ui.gear_hot = approach(g_ui.gear_hot, g_ui.gear_hovered ? 1.0f : 0.0f, dt, 16.0f);
    g_ui.pin_hot = approach(g_ui.pin_hot, (g_ui.pin_hovered || g_ui.pinned) ? 1.0f : 0.0f, dt, 16.0f);
    if (g_ui.oauth_code_mode) {
        int index = g_ui.oauth_session.provider == "grok" ? 4 : 3;
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            done = g_app.providers[index].logged_in && !g_app.providers[index].busy;
        }
        if (done) {
            g_ui.oauth_code_mode = false;
            g_ui.oauth_code_input_focused = false;
            g_ui.oauth_code_input.clear();
            SDL_StopTextInput(g_ui.window);
            set_target_height(wanted_panel_height());
        }
    }
    bool left = g_ui.callout_open || left_sheet_open() || any_model_pinned_open();
    g_ui.left_anim = approach(g_ui.left_anim, left ? 1.0f : 0.0f, dt, 13.0f);
    float target_y = 0, target_h = 0;
    left_card_geom(&target_y, &target_h);
    if (g_ui.left_anim < 0.05f) g_ui.card_y_anim = target_y;
    else g_ui.card_y_anim = approach(g_ui.card_y_anim, target_y, dt, 12.0f);
}

void update_hover(float x, float y) {
    g_ui.hover_ring = -1;
    g_ui.gear_hovered = contains(g_ui.gear_button, x, y);
    g_ui.pin_hovered = contains(g_ui.pin_button, x, y);
    for (int i = 0; i < kProviderCount; ++i) if (contains(g_ui.ring_slots[i], x, y)) g_ui.hover_ring = i;
}

void poll_dismiss() {
    float gx = 0, gy = 0;
    bool down = (SDL_GetGlobalMouseState(&gx, &gy) & SDL_BUTTON_LMASK) != 0;
    bool pressed = down && !g_ui.prev_global_down;
    g_ui.prev_global_down = down;
    if (!g_ui.visible || g_ui.dragging || g_ui.dragging_model >= 0 || g_ui.click_armed) return;
    if (now_ms() - g_ui.shown_at_ms < 800) return;
    if (!pressed) return;
    if (SDL_GetMouseFocus() == g_ui.window) return;
    int wx = 0, wy = 0, ww = 0, wh = 0;
    SDL_GetWindowPosition(g_ui.window, &wx, &wy);
    SDL_GetWindowSize(g_ui.window, &ww, &wh);
    if (gx >= static_cast<float>(wx) && gx < static_cast<float>(wx + ww) && gy >= static_cast<float>(wy) && gy < static_cast<float>(wy + wh)) return;
    hide_panel();
}

void refresh_usage_async_for(int provider_index, bool force = false) {
    unsigned long long operation_id;
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[provider_index];
        if (state.busy) return;
        if (!force && state.last_refresh_ms > 0 && now_ms() - state.last_refresh_ms < 5 * 60 * 1000) return;
        operation_id = ++state.operation_id;
        state.busy = true;
        state.status = "Refreshing " + std::string(provider_label(provider_index)) + "...";
    }

    std::thread([provider_index, operation_id] {
        try {
            UsageInfo info = fetch_usage_with_auth_provider(provider_key(provider_index));
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[provider_index];
            if (state.operation_id != operation_id) return;
            state.logged_in = true;
            std::string plan = pretty_plan(info.plan_type);
            if (provider_index == 1) {
                state.account = "Claude";
                state.account_label = "Claude";
            } else if (provider_index == 2) {
                state.account = "GLM API key";
                state.account_label = "GLM";
            } else if (provider_index == 3) {
                state.account = info.email.empty() ? "Gemini" : info.email;
                state.account_label = "Gemini";
            } else if (provider_index == 4) {
                state.account = info.email.empty() ? (plan.empty() || plan == "unknown" ? "Grok" : plan) : info.email + " (" + plan + ")";
                state.account_label = plan.empty() || plan == "unknown" ? "Grok" : plan;
            } else {
                state.account = info.email.empty() ? plan : info.email + " (" + plan + ")";
                state.account_label = plan.empty() ? "GPT" : plan;
            }
            state.primary_row = primary_row_label(provider_index);
            state.secondary_row = secondary_row_label(provider_index);
            state.tertiary_row = "Requests";
            state.primary_available = info.primary.available;
            state.secondary_available = info.secondary.available;
            state.tertiary_available = info.tertiary.available;
            state.primary_used = std::clamp(info.primary.used_percent, 0.0, 100.0);
            state.secondary_used = std::clamp(info.secondary.used_percent, 0.0, 100.0);
            state.tertiary_used = std::clamp(info.tertiary.used_percent, 0.0, 100.0);
            state.primary_reset = info.primary.reset_at;
            state.secondary_reset = info.secondary.reset_at;
            state.tertiary_reset = info.tertiary.reset_at;
            state.status = "Updated";
            state.last_refresh_ms = now_ms();
            state.busy = false;
            diagnostics_log("provider refresh success provider=" + std::string(provider_key(provider_index)) + " primary_row=" + state.primary_row + " secondary_row=" + state.secondary_row + " primary_used=" + std::to_string(state.primary_used) + " secondary_used=" + std::to_string(state.secondary_used));
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[provider_index];
            if (state.operation_id != operation_id) return;
            state.logged_in = provider_has_auth(provider_index);
            state.status = e.what();
            state.busy = false;
            diagnostics_log("provider refresh error provider=" + std::string(provider_key(provider_index)) + " message=" + e.what());
        }
    }).detach();
}

std::string compact_code(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }), value.end());
    return value;
}

std::string extract_oauth_code(std::string value) {
    value = trim_copy(value);
    std::size_t pos = value.find("code=");
    if (pos != std::string::npos) {
        std::size_t start = pos + 5;
        std::size_t end = value.find_first_of("&#", start);
        value = end == std::string::npos ? value.substr(start) : value.substr(start, end - start);
    }
    return compact_code(value);
}

int oauth_provider_index(const std::string& provider) {
    if (provider == "grok") return 4;
    if (provider == "gemini") return 3;
    if (provider == "glm") return 2;
    return provider == "anthropic" ? 1 : 0;
}

void cancel_oauth_code_login() {
    int index = oauth_provider_index(g_ui.oauth_session.provider);
    g_ui.oauth_code_mode = false;
    g_ui.oauth_code_input_focused = false;
    g_ui.oauth_code_input.clear();
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[index];
        ++state.operation_id;
        state.busy = false;
        state.status = "Login canceled";
    }
    set_target_height(wanted_panel_height());
    SDL_StopTextInput(g_ui.window);
}

void complete_oauth_code(std::string code, OAuthLoginSession session, unsigned long long operation_id) {
    int index = oauth_provider_index(session.provider);
    std::thread([code = std::move(code), session = std::move(session), operation_id, index] {
        try {
            OAuthCredentials credentials = oauth_finish_manual_login_provider(session, code);
            {
                std::lock_guard<std::mutex> lock(g_app.mutex);
                auto& state = g_app.providers[index];
                if (state.operation_id != operation_id) return;
                state.logged_in = true;
                state.account = provider_label(index);
                state.account_label = provider_label(index);
                state.status = "Login complete";
                state.busy = false;
                state.last_refresh_ms = 0;
            }
            refresh_usage_async_for(index, true);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[index];
            if (state.operation_id != operation_id || state.logged_in) return;
            state.status = e.what();
            state.busy = false;
            diagnostics_log(std::string(provider_key(index)) + " oauth verify error message=" + e.what());
        }
    }).detach();
}

void save_oauth_code() {
    std::string code = extract_oauth_code(g_ui.oauth_code_input);
    if (code.empty()) return;
    OAuthLoginSession session = g_ui.oauth_session;
    unsigned long long operation_id;
    int index = oauth_provider_index(session.provider);
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[index];
        operation_id = state.operation_id;
        state.busy = true;
        state.status = std::string("Verifying ") + provider_label(index) + "...";
    }
    g_ui.oauth_code_mode = false;
    g_ui.oauth_code_input_focused = false;
    g_ui.oauth_code_input.clear();
    set_target_height(wanted_panel_height());
    SDL_StopTextInput(g_ui.window);
    complete_oauth_code(std::move(code), std::move(session), operation_id);
}

void begin_oauth_code_login(int index) {
    try {
        unsigned long long operation_id = 0;
        {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[index];
            if (state.busy || state.logged_in) return;
            operation_id = ++state.operation_id;
            state.busy = true;
            state.status = std::string("Waiting for ") + provider_label(index) + " verification code...";
        }
        OAuthLoginSession session = oauth_begin_manual_login_provider(provider_key(index));
        g_ui.oauth_session = session;
        g_ui.oauth_code_mode = true;
        g_ui.oauth_code_input_focused = true;
        g_ui.oauth_code_input.clear();
        set_target_height(wanted_panel_height());
        SDL_StartTextInput(g_ui.window);
        if (!g_ui.visible) show_panel();
        if (index == 4) {
            std::thread([session, operation_id] {
                try {
                    std::string code = wait_for_oauth_code_on(56121, "/callback", session.state, "Grok");
                    complete_oauth_code(std::move(code), session, operation_id);
                } catch (const std::exception&) {
                }
            }).detach();
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        g_app.providers[index].busy = false;
        g_app.providers[index].status = e.what();
        diagnostics_log(std::string(provider_key(index)) + " oauth browser error message=" + e.what());
    }
}

void warm_async_for(int provider_index) {
    unsigned long long operation_id;
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[provider_index];
        if (state.busy || !state.logged_in) return;
        operation_id = ++state.operation_id;
        state.busy = true;
        state.status = "Warming " + std::string(provider_label(provider_index)) + "...";
    }

    std::thread([provider_index, operation_id] {
        try {
            warm_provider(provider_key(provider_index));
            {
                std::lock_guard<std::mutex> lock(g_app.mutex);
                auto& state = g_app.providers[provider_index];
                if (state.operation_id != operation_id) return;
                state.status = "Warm request sent";
                state.busy = false;
                state.last_refresh_ms = 0;
            }
            refresh_usage_async_for(provider_index, true);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[provider_index];
            if (state.operation_id != operation_id) return;
            state.status = e.what();
            state.busy = false;
        }
    }).detach();
}

void login_async_for(int provider_index) {
    if (provider_index == 3 || provider_index == 4) {
        begin_oauth_code_login(provider_index);
        return;
    }
    if (provider_index == 2) {
        if (g_ui.visible) anchor_current_bottom();
        g_ui.api_key_mode = true;
        g_ui.api_input_focused = true;
        g_ui.api_key_input.clear();
        set_target_height(wanted_panel_height());
        SDL_StartTextInput(g_ui.window);
        if (!g_ui.visible) show_panel();
        return;
    }

    unsigned long long operation_id;
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[provider_index];
        if (state.busy) return;
        operation_id = ++state.operation_id;
        state.busy = true;
        state.status = "Waiting for " + std::string(provider_label(provider_index)) + " browser login...";
    }

    std::thread([provider_index, operation_id] {
        try {
            oauth_login_browser_provider(provider_key(provider_index));
            {
                std::lock_guard<std::mutex> lock(g_app.mutex);
                auto& state = g_app.providers[provider_index];
                if (state.operation_id != operation_id) return;
                state.logged_in = true;
                state.status = "Login complete";
                state.busy = false;
            }
            refresh_usage_async_for(provider_index, true);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[provider_index];
            if (state.operation_id != operation_id) return;
            state.status = e.what();
            state.busy = false;
        }
    }).detach();
}

void save_glm_key() {
    std::string key = trim_copy(g_ui.api_key_input);
    if (key.empty()) return;
    save_api_key_provider("glm", key);
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[2];
        state.logged_in = true;
        state.account = "GLM API key";
        state.account_label = "GLM";
        state.status = "API key saved";
        state.last_refresh_ms = 0;
    }
    g_ui.api_key_mode = false;
    g_ui.api_input_focused = false;
    set_target_height(wanted_panel_height());
    SDL_StopTextInput(g_ui.window);
    refresh_usage_async_for(2, true);
}

void set_color(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255) {
    SDL_SetRenderDrawColor(g_ui.renderer, r, g, b, a);
}

SDL_Color color(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255) {
    return SDL_Color{r, g, b, a};
}

bool inside_round_rect(float x, float y, float w, float h, float radius) {
    float cx = std::clamp(x, radius, w - radius);
    float cy = std::clamp(y, radius, h - radius);
    float dx = x - cx;
    float dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

float round_rect_sd(float x, float y, float w, float h, float radius) {
    float qx = std::abs(x - w * 0.5f) - (w * 0.5f - radius);
    float qy = std::abs(y - h * 0.5f) - (h * 0.5f - radius);
    float ox = std::max(qx, 0.0f);
    float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

void aa_round_rect(Rect r, float radius, SDL_Color fill_color, SDL_Color = SDL_Color{0, 0, 0, 0}, float = 0, Uint8 alpha = 255) {
    constexpr int scale = 2;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    int sw = std::max(1, static_cast<int>(std::ceil(r.w * scale)));
    int sh = std::max(1, static_cast<int>(std::ceil(r.h * scale)));
    SDL_Surface* surface = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return;
    SDL_ClearSurface(surface, 0, 0, 0, 0);
    auto* pixels = static_cast<Uint32*>(surface->pixels);
    int stride = surface->pitch / static_cast<int>(sizeof(Uint32));
    float a_scale = alpha / 255.0f;
    for (int py = 0; py < sh; ++py) {
        for (int px = 0; px < sw; ++px) {
            float x = (static_cast<float>(px) + 0.5f) / scale;
            float y = (static_cast<float>(py) + 0.5f) / scale;
            float coverage = std::clamp(0.5f - round_rect_sd(x, y, r.w, r.h, radius) * scale, 0.0f, 1.0f) * a_scale;
            if (coverage <= 0.001f) continue;
            Uint8 a = static_cast<Uint8>(coverage * 255.0f + 0.5f);
            pixels[py * stride + px] = SDL_MapSurfaceRGBA(surface,
                static_cast<Uint8>(fill_color.r * coverage + 0.5f),
                static_cast<Uint8>(fill_color.g * coverage + 0.5f),
                static_cast<Uint8>(fill_color.b * coverage + 0.5f), a);
        }
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_ui.renderer, surface);
    if (texture) {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(texture, g_ui.premul);
        SDL_FRect dst{r.x, r.y, r.w, r.h};
        SDL_RenderTexture(g_ui.renderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }
    SDL_DestroySurface(surface);
}

void fill(Rect r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca = 255) {
    SDL_FRect fr{r.x, r.y, r.w, r.h};
    set_color(cr, cg, cb, ca);
    SDL_RenderFillRect(g_ui.renderer, &fr);
}

void outline(Rect r, Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_FRect fr{r.x, r.y, r.w, r.h};
    set_color(cr, cg, cb, 255);
    SDL_RenderRect(g_ui.renderer, &fr);
}

void fill_round(Rect r, float radius, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca = 255) {
    radius = std::min(radius, std::min(r.w, r.h) / 2.0f);
    set_color(cr, cg, cb, ca);
    fill({r.x + radius, r.y, r.w - radius * 2.0f, r.h}, cr, cg, cb, ca);
    fill({r.x, r.y + radius, r.w, r.h - radius * 2.0f}, cr, cg, cb, ca);
    int ri = static_cast<int>(std::ceil(radius));
    for (int y = 0; y < ri; ++y) {
        float dy = radius - static_cast<float>(y) - 0.5f;
        float dx = std::sqrt(std::max(0.0f, radius * radius - dy * dy));
        float left = radius - dx;
        float width = r.w - left * 2.0f;
        fill({r.x + left, r.y + static_cast<float>(y), width, 1.0f}, cr, cg, cb, ca);
        fill({r.x + left, r.y + r.h - static_cast<float>(y) - 1.0f, width, 1.0f}, cr, cg, cb, ca);
    }
}

void outline_round(Rect r, float radius, Uint8 cr, Uint8 cg, Uint8 cb) {
    for (int i = 0; i < 1; ++i) {
        outline({r.x + radius, r.y + i, r.w - radius * 2.0f, 1}, cr, cg, cb);
        outline({r.x + radius, r.y + r.h - 1 - i, r.w - radius * 2.0f, 1}, cr, cg, cb);
        outline({r.x + i, r.y + radius, 1, r.h - radius * 2.0f}, cr, cg, cb);
        outline({r.x + r.w - 1 - i, r.y + radius, 1, r.h - radius * 2.0f}, cr, cg, cb);
    }
    set_color(cr, cg, cb, 255);
    int ri = static_cast<int>(std::ceil(radius));
    for (int y = 0; y < ri; ++y) {
        float dy = radius - static_cast<float>(y) - 0.5f;
        float dx = std::sqrt(std::max(0.0f, radius * radius - dy * dy));
        SDL_RenderPoint(g_ui.renderer, r.x + radius - dx, r.y + y);
        SDL_RenderPoint(g_ui.renderer, r.x + r.w - radius + dx - 1, r.y + y);
        SDL_RenderPoint(g_ui.renderer, r.x + radius - dx, r.y + r.h - y - 1);
        SDL_RenderPoint(g_ui.renderer, r.x + r.w - radius + dx - 1, r.y + r.h - y - 1);
    }
}

TTF_Font* pick_font(bool bold, bool small = false) {
    if (small) {
        if (bold && g_ui.font_small_bold) return g_ui.font_small_bold;
        if (g_ui.font_small) return g_ui.font_small;
    }
    return bold && g_ui.font_bold ? g_ui.font_bold : g_ui.font;
}

std::pair<int, int> measure_text(const std::string& s, bool bold = false, bool small = false) {
    int w = 0;
    int h = 0;
    TTF_Font* font = pick_font(bold, small);
    if (!font || s.empty()) return {0, 0};
    TTF_GetStringSize(font, s.c_str(), s.size(), &w, &h);
    float scale = std::max(1.0f, g_ui.render_scale);
    return {
        static_cast<int>(std::ceil(static_cast<float>(w) / scale)),
        static_cast<int>(std::ceil(static_cast<float>(h) / scale))
    };
}

void text(float x, float y, const std::string& s, Uint8 r = 245, Uint8 g = 245, Uint8 b = 247, bool bold = false, bool small = false) {
    if (s.empty()) return;
    TTF_Font* font = pick_font(bold, small);
    if (!font) return;
    SDL_Surface* surface = TTF_RenderText_Blended(font, s.c_str(), s.size(), color(r, g, b));
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_ui.renderer, surface);
    if (texture) {
        float scale = std::max(1.0f, g_ui.render_scale);
        SDL_FRect dst{x, y, static_cast<float>(surface->w) / scale, static_cast<float>(surface->h) / scale};
        SDL_RenderTexture(g_ui.renderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }
    SDL_DestroySurface(surface);
}

std::string clip_text(const std::string& s, int max_width, bool bold = false, bool small = false) {
    if (measure_text(s, bold, small).first <= max_width) return s;
    std::string out = s;
    while (!out.empty() && measure_text(out + "...", bold, small).first > max_width) out.pop_back();
    return out.empty() ? "..." : out + "...";
}

std::string masked_input_text(const std::string& value, int max_width) {
    std::string masked(value.size(), '*');
    while (!masked.empty() && measure_text(masked).first > max_width) masked.erase(masked.begin());
    return masked;
}

void button(Rect r, const std::string& label, bool enabled = true) {
    aa_round_rect(r, 8,
        color(enabled ? 36 : 28, enabled ? 36 : 28, enabled ? 38 : 30),
        color(enabled ? 58 : 42, enabled ? 58 : 42, enabled ? 62 : 46));
    auto [tw, th] = measure_text(label, false, true);
    float tx = r.x + std::max(6.0f, (r.w - static_cast<float>(tw)) / 2.0f);
    float ty = r.y + std::max(2.0f, (r.h - static_cast<float>(th)) / 2.0f);
    text(tx, ty, label, enabled ? 245 : 120, enabled ? 245 : 120, enabled ? 247 : 122, false, true);
}

void usage_track(float x, float y, float width, double used, bool weekly) {
    fill_round({x, y, width, 6}, 3, 44, 44, 48);
    float fw = static_cast<float>(display_percent(used) / 100.0 * width);
    if (fw > 0.5f) fill_round({x, y, std::max(6.0f, fw), 6}, 3, weekly ? 48 : 240, weekly ? 209 : 196, weekly ? 88 : 64);
}

SDL_FPoint rotate_point(float x, float y, float cx, float cy, float radians) {
    float s = std::sin(radians);
    float c = std::cos(radians);
    x -= cx;
    y -= cy;
    return {cx + x * c - y * s, cy + x * s + y * c};
}

void thick_line(SDL_FPoint a, SDL_FPoint b, float width, SDL_Color c) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    float ox = -dy / len * width * 0.5f;
    float oy = dx / len * width * 0.5f;
    SDL_FColor fc{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
    SDL_Vertex verts[4] = {
        {{a.x + ox, a.y + oy}, fc, {0, 0}},
        {{a.x - ox, a.y - oy}, fc, {0, 0}},
        {{b.x - ox, b.y - oy}, fc, {0, 0}},
        {{b.x + ox, b.y + oy}, fc, {0, 0}},
    };
    int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(g_ui.renderer, nullptr, verts, 4, indices, 6);
}

void filled_quad(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_Color col) {
    SDL_FColor fc{col.r / 255.0f, col.g / 255.0f, col.b / 255.0f, col.a / 255.0f};
    SDL_Vertex verts[4] = {
        {a, fc, {0, 0}},
        {b, fc, {0, 0}},
        {c, fc, {0, 0}},
        {d, fc, {0, 0}},
    };
    int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(g_ui.renderer, nullptr, verts, 4, indices, 6);
}

void filled_polygon(const std::vector<SDL_FPoint>& points, SDL_Color col) {
    if (points.size() < 3) return;
    SDL_FColor fc{col.r / 255.0f, col.g / 255.0f, col.b / 255.0f, col.a / 255.0f};
    std::vector<SDL_Vertex> verts;
    std::vector<int> indices;
    verts.reserve(points.size());
    for (const auto& point : points) {
        verts.push_back({point, fc, {0, 0}});
    }
    for (int i = 1; i + 1 < static_cast<int>(points.size()); ++i) {
        indices.push_back(0);
        indices.push_back(i);
        indices.push_back(i + 1);
    }
    SDL_RenderGeometry(g_ui.renderer, nullptr, verts.data(), static_cast<int>(verts.size()),
        indices.data(), static_cast<int>(indices.size()));
}

void stroke_arc(float cx, float cy, float radius, float thickness, float t0, float t1, SDL_Color c) {
    if (t1 <= t0) return;
    int steps = std::max(8, static_cast<int>(std::ceil(std::abs(t1 - t0) * radius * 1.6f)));
    SDL_FPoint prev{};
    for (int i = 0; i <= steps; ++i) {
        float t = t0 + (t1 - t0) * (static_cast<float>(i) / static_cast<float>(steps));
        SDL_FPoint p{cx + std::sin(t) * radius, cy - std::cos(t) * radius};
        if (i) thick_line(prev, p, thickness, c);
        prev = p;
    }
}

void draw_ring(float cx, float cy, float radius, double used, SDL_Color accent) {
    const float thickness = 4.5f;
    const int scale = 3;
    int size = static_cast<int>(std::ceil((radius + thickness + 2.0f) * 2.0f * scale));
    SDL_Surface* surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return;
    SDL_ClearSurface(surface, 0, 0, 0, 0);
    auto* pixels = static_cast<Uint32*>(surface->pixels);
    int stride = surface->pitch / static_cast<int>(sizeof(Uint32));
    float ocx = static_cast<float>(size) * 0.5f;
    float orad = radius * scale;
    float othick = thickness * scale;
    float sweep = static_cast<float>(std::clamp(used, 0.0, 100.0) / 100.0 * 6.2831853f);
    SDL_Color track = color(52, 52, 56);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            float dx = static_cast<float>(px) + 0.5f - ocx;
            float dy = static_cast<float>(py) + 0.5f - ocx;
            float d = std::sqrt(dx * dx + dy * dy);
            float cover = std::clamp(othick * 0.5f + 0.85f - std::abs(d - orad), 0.0f, 1.0f);
            if (cover <= 0.001f) continue;
            float ang = std::atan2(dx, -dy);
            if (ang < 0) ang += 6.2831853f;
            bool on_progress = sweep > 0.02f && ang <= sweep;
            SDL_Color c = on_progress ? accent : track;
            float a = cover;
            pixels[py * stride + px] = SDL_MapSurfaceRGBA(surface,
                static_cast<Uint8>(c.r * a + 0.5f),
                static_cast<Uint8>(c.g * a + 0.5f),
                static_cast<Uint8>(c.b * a + 0.5f),
                static_cast<Uint8>(a * 255.0f + 0.5f));
        }
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_ui.renderer, surface);
    SDL_DestroySurface(surface);
    if (!texture) return;
    SDL_SetTextureBlendMode(texture, g_ui.premul);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    float draw = static_cast<float>(size) / static_cast<float>(scale);
    SDL_FRect dst{cx - draw * 0.5f, cy - draw * 0.5f, draw, draw};
    SDL_RenderTexture(g_ui.renderer, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
}

void draw_provider_glyph(float cx, float cy, int index, SDL_Color) {
    icons_draw(icon_provider(index), cx, cy, 18.0f);
}

void draw_gear_icon(Rect r, float = 0) {
    icons_draw(icon_gear(), r.x + r.w * 0.5f, r.y + r.h * 0.5f, 18.0f);
}

void draw_pin_icon(Rect r, bool, float = 0) {
    icons_draw(icon_pin(), r.x + r.w * 0.5f, r.y + r.h * 0.5f, 18.0f);
}

void draw_status_dot(float x, float y, bool on) {
    fill_round({x, y, 7, 7}, 3.5f, on ? 48 : 72, on ? 209 : 72, on ? 88 : 76);
}

void draw_left_card_chrome(float y, float h, Uint8 alpha) {
    g_ui.callout_rect = {0, y, static_cast<float>(kCalloutWidth), h};
    aa_round_rect(g_ui.callout_rect, static_cast<float>(kCardRadius), color(18, 18, 20), color(18, 18, 20), 0, alpha);
    float cy = y + h * 0.5f;
    SDL_Color tail = color(18, 18, 20);
    tail.a = alpha;
    filled_polygon({
        {static_cast<float>(kCalloutWidth) - 2.0f, cy - 8.0f},
        {static_cast<float>(kCalloutWidth) - 2.0f, cy + 8.0f},
        {static_cast<float>(kCalloutWidth + kTailWidth), cy}
    }, tail);
}

void draw_input_field(Rect box, const std::string& masked, bool focused) {
    aa_round_rect(box, 8, color(28, 28, 32), focused ? color(80, 80, 86) : color(52, 52, 56));
    text(box.x + 10, box.y + 8, masked, 245, 245, 247, false, true);
    if (focused && ((SDL_GetTicks() / 500) % 2 == 0)) {
        auto [tw, th] = measure_text(masked, false, true);
        fill({box.x + 10 + static_cast<float>(tw) + 2.0f, box.y + 7, 1.5f, 16}, 245, 245, 247);
    }
}

void draw_panel() {
    set_color(0, 0, 0, 0);
    SDL_RenderClear(g_ui.renderer);
    int selected = 0;
    bool enabled[kProviderCount]{};
    ProviderState states[kProviderCount]{};
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        selected = g_app.selected;
        for (int i = 0; i < kProviderCount; ++i) {
            enabled[i] = g_app.enabled[i];
            states[i] = g_app.providers[i];
        }
    }
    int visible = 0;
    int visible_index[kProviderCount];
    for (int i = 0; i < kProviderCount; ++i) if (enabled[i]) visible_index[visible++] = i;
    if (visible == 0) {
        visible_index[0] = selected;
        visible = 1;
    }
    float dx = dock_x();
    float dock_h = static_cast<float>(std::max(kDockPad + visible * kRingSlot + kDockFooter, kCalloutHeight));
    g_ui.dock_rect = {dx, 0, static_cast<float>(kDockWidth), dock_h};
    g_ui.callout_pin_button = {};
    g_ui.settings_fill_toggle = {};
    for (int i = 0; i < kProviderCount; ++i) {
        g_ui.model_callout_rect[i] = {};
        g_ui.model_pin_button[i] = {};
    }
    aa_round_rect(g_ui.dock_rect, static_cast<float>(kDockRadius), color(18, 18, 20));
    for (int slot = 0; slot < kProviderCount; ++slot) g_ui.ring_slots[slot] = {};
    for (int n = 0; n < visible; ++n) {
        int i = visible_index[n];
        float cy = static_cast<float>(kDockPad + n * kRingSlot + kRingSize * 0.5f);
        float cx = dx + kDockWidth * 0.5f;
        g_ui.ring_slots[i] = {dx + 6, cy - 30, static_cast<float>(kDockWidth) - 12, 68};
        float pop = 1.0f + 0.08f * g_ui.hover_anim[i];
        SDL_Color accent = provider_accent(i);
        if (g_ui.hover_anim[i] > 0.01f) {
            accent.r = static_cast<Uint8>(std::min(255.0f, accent.r + 28 * g_ui.hover_anim[i]));
            accent.g = static_cast<Uint8>(std::min(255.0f, accent.g + 28 * g_ui.hover_anim[i]));
            accent.b = static_cast<Uint8>(std::min(255.0f, accent.b + 28 * g_ui.hover_anim[i]));
        }
        double used = display_percent(g_ui.used_anim[i]);
        draw_ring(cx, cy, 23.0f * pop, used, accent);
        draw_provider_glyph(cx, cy, i, accent);
        std::string pct = states[i].logged_in ? (std::to_string(static_cast<int>(std::round(used))) + "%") : "--";
        auto [tw, th] = measure_text(pct, false, true);
        text(cx - tw * 0.5f, cy + 26.0f, pct, 245, 245, 247, false, true);
    }
    float footer_y = static_cast<float>(kDockPad + visible * kRingSlot + 2);
    g_ui.gear_button = {dx + 8, footer_y, 36, 36};
    g_ui.pin_button = {dx + 48, footer_y, 36, 36};
    draw_gear_icon(g_ui.gear_button, g_ui.gear_hot);
    draw_pin_icon(g_ui.pin_button, g_ui.pinned, g_ui.pin_hot);
    bool left_open = g_ui.callout_open || left_sheet_open();
    if (left_open && g_ui.left_anim > 0.02f) {
        Uint8 alpha = static_cast<Uint8>(std::clamp(g_ui.left_anim, 0.0f, 1.0f) * 255.0f);
        auto draw_model_card = [&](int index, float card_y) {
            const auto& state = states[index];
            float card_h = static_cast<float>(kCalloutHeight);
            draw_left_card_chrome(card_y, card_h, alpha);
            g_ui.model_callout_rect[index] = g_ui.callout_rect;
            draw_provider_glyph(28, card_y + 26, index, provider_accent(index));
            text(44, card_y + 16, provider_label(index), 245, 245, 247, true);
            g_ui.model_pin_button[index] = {static_cast<float>(kCalloutWidth) - 58, card_y + 10, 28, 28};
            if (index == selected) g_ui.callout_pin_button = g_ui.model_pin_button[index];
            draw_pin_icon(g_ui.model_pin_button[index], g_ui.model_pinned[index], g_ui.model_pinned[index] ? 1.0f : 0.0f);
            draw_status_dot(static_cast<float>(kCalloutWidth) - 24, card_y + 20, state.logged_in && !state.busy);
            auto row = [&](float y, const std::string& label, bool available, double used, long long reset, bool weekly) {
                if (!available) return;
                text(18, y, label, 245, 245, 247, false, true);
                std::string reset_text = format_reset_phrase(reset);
                auto [rw, rh] = measure_text(reset_text, false, true);
                text(static_cast<float>(kCalloutWidth) - 18 - rw, y, reset_text, 142, 142, 147, false, true);
                usage_track(18, y + 20, static_cast<float>(kCalloutWidth) - 36, used, weekly);
                text(18, y + 30, std::to_string(static_cast<int>(std::round(display_percent(used)))) + (g_ui.show_remaining ? "% Left" : "% Used"), 174, 174, 178, false, true);
            };
            float y = card_y + 48;
            if (state.primary_available) { row(y, state.primary_row, true, state.primary_used, state.primary_reset, false); y += 56; }
            if (state.secondary_available) row(y, state.secondary_row, true, state.secondary_used, state.secondary_reset, true);
        };
        if (left_sheet_open()) {
            float card_y = 0, card_h = 0;
            left_card_geom(&card_y, &card_h);
            draw_left_card_chrome(card_y, card_h, alpha);
            if (g_ui.api_key_mode) {
                text(18, card_y + 16, "GLM API key", 245, 245, 247, true);
                text(18, card_y + 40, "Paste key. Saved in the platform secret store.", 142, 142, 147, false, true);
                g_ui.api_input = {18, card_y + 68, 300, 32};
                draw_input_field(g_ui.api_input, masked_input_text(g_ui.api_key_input, 280), g_ui.api_input_focused);
                g_ui.api_ok = {18, card_y + 112, 144, 30};
                g_ui.api_cancel = {174, card_y + 112, 144, 30};
                button(g_ui.api_ok, "Save", !g_ui.api_key_input.empty());
                button(g_ui.api_cancel, "Cancel", true);
            } else if (g_ui.oauth_code_mode) {
                bool grok = g_ui.oauth_session.provider == "grok";
                text(18, card_y + 16, grok ? "Grok verification" : "Gemini verification", 245, 245, 247, true);
                text(18, card_y + 40, grok ? "Paste the code from the browser." : "Paste the code shown by Antigravity.", 142, 142, 147, false, true);
                g_ui.oauth_code_input_box = {18, card_y + 68, 300, 32};
                draw_input_field(g_ui.oauth_code_input_box, masked_input_text(g_ui.oauth_code_input, 280), g_ui.oauth_code_input_focused);
                g_ui.oauth_code_ok = {18, card_y + 112, 144, 30};
                g_ui.oauth_code_cancel = {174, card_y + 112, 144, 30};
                button(g_ui.oauth_code_ok, "Verify", !g_ui.oauth_code_input.empty());
                button(g_ui.oauth_code_cancel, "Cancel", true);
            } else if (g_ui.settings_open) {
                text(18, card_y + 16, "Settings", 245, 245, 247, true);
                for (int i = 0; i < kProviderCount; ++i) {
                    float y = card_y + static_cast<float>(kSettingsHeader + i * kSettingsRowHeight);
                    SDL_Color accent = provider_accent(i);
                    draw_provider_glyph(32, y + 22, i, accent);
                    text(48, y + 8, provider_label(i), 245, 245, 247, true, true);
                    text(48, y + 26, states[i].logged_in ? (states[i].account_label.empty() ? "Connected" : clip_text(states[i].account_label, 140, false, true)) : "Not signed in", 142, 142, 147, false, true);
                    g_ui.settings_toggle[i] = {210, y + 12, 36, 22};
                    aa_round_rect(g_ui.settings_toggle[i], 11, enabled[i] ? color(48, 209, 88) : color(58, 58, 62), enabled[i] ? color(48, 209, 88) : color(58, 58, 62));
                    fill_round({g_ui.settings_toggle[i].x + (enabled[i] ? 18.0f : 4.0f), y + 15, 16, 16}, 8, 245, 245, 247);
                    g_ui.settings_action[i] = {252, y + 12, 68, 24};
                    button(g_ui.settings_action[i], states[i].logged_in ? "Sign out" : (i == 2 ? "Key" : "Sign in"), !states[i].busy);
                }
                float fill_y = card_y + static_cast<float>(kSettingsHeader + kProviderCount * kSettingsRowHeight);
                text(18, fill_y + 8, g_ui.show_remaining ? "Show remaining" : "Show used", 245, 245, 247, true, true);
                g_ui.settings_fill_toggle = {210, fill_y + 12, 36, 22};
                aa_round_rect(g_ui.settings_fill_toggle, 11, g_ui.show_remaining ? color(48, 209, 88) : color(58, 58, 62), g_ui.show_remaining ? color(48, 209, 88) : color(58, 58, 62));
                fill_round({g_ui.settings_fill_toggle.x + (g_ui.show_remaining ? 18.0f : 4.0f), fill_y + 15, 16, 16}, 8, 245, 245, 247);
                g_ui.settings_refresh = {18, card_y + card_h - 42, 150, 28};
                g_ui.settings_quit = {178, card_y + card_h - 42, 140, 28};
                button(g_ui.settings_refresh, "Refresh all", true);
                button(g_ui.settings_quit, "Quit", true);
            }
        } else {
            for (int i = 0; i < kProviderCount; ++i) {
                if (!g_ui.model_open[i]) continue;
                float card_y = (model_is_snapped(i) && i == selected) ? g_ui.card_y_anim : callout_y_for(i);
                draw_model_card(i, card_y);
            }
        }
    }
    SDL_RenderPresent(g_ui.renderer);
}

void on_tray_show(void*, SDL_TrayEntry*) { g_show_requested = true; }
void on_tray_refresh(void*, SDL_TrayEntry*) { g_refresh_requested = true; }
void on_tray_warm(void*, SDL_TrayEntry*) { g_warm_requested = true; }
void on_tray_quit(void*, SDL_TrayEntry*) { g_quit = true; }

bool on_tray_left_click(void*, SDL_Tray*) {
    g_show_requested = true;
    return false;
}

bool on_tray_right_click(void*, SDL_Tray*) {
    g_show_requested = true;
    return false;
}

SDL_Surface* make_icon_surface(int size) {
    SDL_Surface* icon = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!icon) return nullptr;
    SDL_ClearSurface(icon, 0, 0, 0, 0);
    Uint32 bg = SDL_MapSurfaceRGBA(icon, 23, 26, 28, 255);
    Uint32 green = SDL_MapSurfaceRGBA(icon, 68, 188, 126, 255);
    Uint32 blue = SDL_MapSurfaceRGBA(icon, 82, 145, 224, 255);
    fill_surface_round(icon, size, size, std::max(4, size / 5), bg);
    int margin = std::max(5, size / 5);
    int bar_h = std::max(3, size / 8);
    int bar_w = size - margin * 2;
    SDL_Rect bar1{margin, size / 3 - bar_h / 2, bar_w, bar_h};
    SDL_Rect bar2{margin, size * 2 / 3 - bar_h / 2, bar_w * 3 / 4, bar_h};
    fill_surface_rect(icon, bar1, green);
    fill_surface_rect(icon, bar2, blue);
    return icon;
}

void create_tray() {
    g_ui.icon = make_icon_surface(32);
    if (g_ui.icon) SDL_SetWindowIcon(g_ui.window, g_ui.icon);
#ifdef SDL_PROP_TRAY_CREATE_LEFTCLICK_CALLBACK_POINTER
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_TRAY_CREATE_ICON_POINTER, g_ui.icon);
    SDL_SetStringProperty(props, SDL_PROP_TRAY_CREATE_TOOLTIP_STRING, "LLM Usage Tray");
    SDL_SetPointerProperty(props, SDL_PROP_TRAY_CREATE_LEFTCLICK_CALLBACK_POINTER, reinterpret_cast<void*>(on_tray_left_click));
    g_ui.tray = SDL_CreateTrayWithProperties(props);
    SDL_DestroyProperties(props);
#else
    g_ui.tray = SDL_CreateTray(g_ui.icon, "LLM Usage Tray");
#endif
    if (g_ui.tray) {
        SDL_TrayMenu* menu = SDL_CreateTrayMenu(g_ui.tray);
        SDL_TrayEntry* show = menu ? SDL_InsertTrayEntryAt(menu, -1, "Show", SDL_TRAYENTRY_BUTTON) : nullptr;
        if (show) SDL_SetTrayEntryCallback(show, on_tray_show, nullptr);
        SDL_TrayEntry* quit = menu ? SDL_InsertTrayEntryAt(menu, -1, "Quit", SDL_TRAYENTRY_BUTTON) : nullptr;
        if (quit) SDL_SetTrayEntryCallback(quit, on_tray_quit, nullptr);
    }
}

void save_layout() {
    std::string json = "{";
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        json += "\"selected\":" + std::to_string(g_app.selected);
        for (int i = 0; i < kProviderCount; ++i) json += ",\"e" + std::to_string(i) + "\":" + (g_app.enabled[i] ? "1" : "0");
    }
    json += ",\"remain\":" + std::string(g_ui.show_remaining ? "1" : "0");
    json += "}";
    try { credential_save_named("layout", json); } catch (const std::exception&) { }
}

void logout_provider(int index) {
    clear_credentials_provider(provider_key(index));
    std::lock_guard<std::mutex> lock(g_app.mutex);
    auto& state = g_app.providers[index];
    ++state.operation_id;
    state.busy = false;
    state.logged_in = false;
    state.account.clear();
    state.account_label.clear();
    state.status = "Logged out";
    state.primary_used = 0;
    state.secondary_used = 0;
    state.primary_reset = 0;
    state.secondary_reset = 0;
    state.last_refresh_ms = 0;
}

void toggle_model_pin(int index) {
    g_ui.model_open[index] = true;
    g_ui.model_pinned[index] = !g_ui.model_pinned[index];
    if (g_ui.model_pinned[index]) {
        g_ui.model_detached[index] = true;
        g_ui.model_callout_y[index] = g_ui.model_callout_rect[index].w > 0 ? g_ui.model_callout_rect[index].y : callout_y_for(index);
    } else {
        for (int j = 0; j < kProviderCount; ++j) {
            if (j != index && model_is_snapped(j)) {
                g_ui.model_open[j] = false;
                g_ui.model_detached[j] = false;
            }
        }
        g_ui.model_detached[index] = false;
        g_ui.model_callout_y[index] = -1;
    }
    sync_callout_open();
}

void toggle_model_callout(int index) {
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        g_app.selected = index;
    }
    g_ui.settings_open = false;
    if (model_is_snapped(index)) {
        g_ui.model_open[index] = false;
        g_ui.model_detached[index] = false;
        sync_callout_open();
        return;
    }
    if (g_ui.model_open[index]) return;
    for (int j = 0; j < kProviderCount; ++j) {
        if (j != index && model_is_snapped(j)) {
            g_ui.model_open[j] = false;
            g_ui.model_detached[j] = false;
        }
    }
    g_ui.model_open[index] = true;
    g_ui.model_detached[index] = false;
    g_ui.model_callout_y[index] = -1;
    g_ui.callout_open = true;
}

void handle_right_click(float, float) {
    g_ui.settings_open = !g_ui.settings_open;
    set_target_height(wanted_panel_height());
}

void handle_click(float x, float y) {
    for (int i = 0; i < kProviderCount; ++i) {
        if (!contains(g_ui.model_pin_button[i], x, y)) continue;
        toggle_model_pin(i);
        return;
    }
    if (contains(g_ui.callout_pin_button, x, y)) {
        toggle_model_pin(selected_provider());
        return;
    }
    if (g_ui.pin_hovered) {
        g_ui.pinned = !g_ui.pinned;
        return;
    }
    if (g_ui.gear_hovered) {
        g_ui.settings_open = !g_ui.settings_open;
        set_target_height(wanted_panel_height());
        update_window_shape();
        return;
    }
    if (g_ui.hover_ring >= 0 && !g_ui.settings_open && !g_ui.api_key_mode && !g_ui.oauth_code_mode) {
        int i = g_ui.hover_ring;
        toggle_model_callout(i);
        if (provider_has_auth(i)) refresh_usage_async_for(i, false);
        set_target_height(wanted_panel_height());
        update_window_shape();
        return;
    }
    if (g_ui.api_key_mode) {
        if (contains(g_ui.api_input, x, y)) {
            g_ui.api_input_focused = true;
            SDL_StartTextInput(g_ui.window);
        } else if (contains(g_ui.api_ok, x, y)) save_glm_key();
        else if (contains(g_ui.api_cancel, x, y)) {
            g_ui.api_key_mode = false;
            g_ui.api_input_focused = false;
            set_target_height(wanted_panel_height());
            SDL_StopTextInput(g_ui.window);
        }
        return;
    }
    if (g_ui.oauth_code_mode) {
        if (contains(g_ui.oauth_code_input_box, x, y)) {
            g_ui.oauth_code_input_focused = true;
            SDL_StartTextInput(g_ui.window);
        } else if (contains(g_ui.oauth_code_ok, x, y)) save_oauth_code();
        else if (contains(g_ui.oauth_code_cancel, x, y)) cancel_oauth_code_login();
        return;
    }
    if (contains(g_ui.pin_button, x, y)) {
        g_ui.pinned = !g_ui.pinned;
        return;
    }
    if (contains(g_ui.gear_button, x, y)) {
        g_ui.settings_open = !g_ui.settings_open;
        set_target_height(wanted_panel_height());
        return;
    }
    if (g_ui.settings_open) {
        if (contains(g_ui.settings_fill_toggle, x, y)) {
            g_ui.show_remaining = !g_ui.show_remaining;
            save_layout();
            return;
        }
        for (int i = 0; i < kProviderCount; ++i) {
            if (contains(g_ui.settings_toggle[i], x, y)) {
                {
                    std::lock_guard<std::mutex> lock(g_app.mutex);
                    bool others = false;
                    for (int j = 0; j < kProviderCount; ++j) if (j != i && g_app.enabled[j]) others = true;
                    if (g_app.enabled[i] && !others) return;
                    g_app.enabled[i] = !g_app.enabled[i];
                    if (g_app.enabled[i]) g_app.selected = i;
                    else if (g_app.selected == i) {
                        for (int j = 0; j < kProviderCount; ++j) if (g_app.enabled[j]) { g_app.selected = j; break; }
                    }
                }
                save_layout();
                set_target_height(wanted_panel_height());
                return;
            }
            if (contains(g_ui.settings_action[i], x, y)) {
                bool logged_in = false;
                {
                    std::lock_guard<std::mutex> lock(g_app.mutex);
                    logged_in = g_app.providers[i].logged_in;
                    g_app.selected = i;
                }
                if (logged_in) logout_provider(i);
                else login_async_for(i);
                return;
            }
        }
        if (contains(g_ui.settings_refresh, x, y)) {
            for (int i = 0; i < kProviderCount; ++i) if (provider_has_auth(i)) refresh_usage_async_for(i, true);
            return;
        }
        if (contains(g_ui.settings_quit, x, y)) {
            g_quit = true;
            return;
        }
        return;
    }
    for (int i = 0; i < kProviderCount; ++i) {
        if (!contains(g_ui.ring_slots[i], x, y)) continue;
        toggle_model_callout(i);
        if (provider_has_auth(i)) refresh_usage_async_for(i, false);
        set_target_height(wanted_panel_height());
        return;
    }
}

void handle_mouse_down(float x, float y) {
    g_ui.click_armed = false;
    g_ui.dragging_model = -1;
    if (g_ui.hover_ring >= 0 || g_ui.gear_hovered || g_ui.pin_hovered || over_click_target(x, y)) {
        handle_click(x, y);
        g_ui.click_armed = true;
        return;
    }
    for (int i = kProviderCount - 1; i >= 0; --i) {
        if (!g_ui.model_open[i] || !contains(g_ui.model_callout_rect[i], x, y)) continue;
        if (contains(g_ui.model_pin_button[i], x, y)) continue;
        g_ui.dragging_model = i;
        g_ui.drag_offset_y = static_cast<int>(y - g_ui.model_callout_rect[i].y);
        return;
    }
    if (!contains(g_ui.dock_rect, x, y) && !left_sheet_open()) {
        hide_panel();
        return;
    }
    int wx = 0;
    int wy = 0;
    float gx = 0;
    float gy = 0;
    SDL_GetWindowPosition(g_ui.window, &wx, &wy);
    SDL_GetGlobalMouseState(&gx, &gy);
    g_ui.dragging = true;
    g_ui.drag_moved = false;
    g_ui.drag_offset_x = static_cast<int>(gx) - wx;
    g_ui.drag_offset_y = static_cast<int>(gy) - wy;
}

void handle_mouse_motion() {
    if (g_ui.dragging_model >= 0) {
        float gx = 0, gy = 0, lx = 0, ly = 0;
        SDL_GetGlobalMouseState(&gx, &gy);
        int wx = 0, wy = 0;
        SDL_GetWindowPosition(g_ui.window, &wx, &wy);
        window_to_logical(gx - static_cast<float>(wx), gy - static_cast<float>(wy), &lx, &ly);
        int index = g_ui.dragging_model;
        float h = static_cast<float>(kCalloutHeight);
        float y = std::clamp(ly - static_cast<float>(g_ui.drag_offset_y), 0.0f, std::max(0.0f, static_cast<float>(g_ui.panel_height) - h));
        g_ui.model_callout_y[index] = y;
        float snap = snap_callout_y(index);
        if (std::abs(y - snap) > 18.0f) g_ui.model_detached[index] = true;
        else if (!g_ui.model_pinned[index]) g_ui.model_detached[index] = false;
        set_target_height(wanted_panel_height());
        return;
    }
    if (!g_ui.dragging) return;
    float gx = 0;
    float gy = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    int nx = static_cast<int>(gx) - g_ui.drag_offset_x;
    int ny = static_cast<int>(gy) - g_ui.drag_offset_y;
    SDL_SetWindowPosition(g_ui.window, nx, ny);
    g_ui.anchor_bottom = ny + panel_height_px();
    g_ui.drag_moved = true;
}

void handle_mouse_up(float x, float y) {
    if (g_ui.click_armed) {
        g_ui.click_armed = false;
        g_ui.dragging = false;
        g_ui.dragging_model = -1;
        g_ui.drag_moved = false;
        return;
    }
    g_ui.dragging_model = -1;
    bool was_dragging = g_ui.dragging;
    bool moved = g_ui.drag_moved;
    g_ui.dragging = false;
    g_ui.drag_moved = false;
    if (!was_dragging || !moved) handle_click(x, y);
}

void load_layout() {
    auto raw = credential_load_named("layout");
    if (!raw) return;
    if (auto remain = json_number(*raw, "remain")) g_ui.show_remaining = *remain != 0;
    std::lock_guard<std::mutex> lock(g_app.mutex);
    if (auto selected = json_number(*raw, "selected")) {
        int value = static_cast<int>(*selected);
        if (value >= 0 && value < kProviderCount) g_app.selected = value;
    }
    bool any = false;
    for (int i = 0; i < kProviderCount; ++i) {
        if (auto flag = json_number(*raw, "e" + std::to_string(i))) {
            g_app.enabled[i] = *flag != 0;
            any = true;
        }
    }
    if (!any) {
        for (int i = 0; i < 3; ++i) {
            std::string key = "tab" + std::to_string(i);
            auto value = json_number(*raw, key);
            if (value && *value >= 0 && *value < kProviderCount) g_app.enabled[static_cast<int>(*value)] = true;
        }
    }
}

void init_state() {
    load_layout();
    std::lock_guard<std::mutex> lock(g_app.mutex);
    for (int i = 0; i < kProviderCount; ++i) {
        auto& state = g_app.providers[i];
        state.logged_in = provider_has_auth(i);
        state.status = state.logged_in ? "Ready to refresh " + std::string(provider_label(i)) : "Not logged in";
        state.primary_row = primary_row_label(i);
        state.secondary_row = secondary_row_label(i);
        if (i == 2) {
            state.status = state.logged_in ? "Ready to refresh GLM" : "No GLM API key saved";
            state.account = state.logged_in ? "GLM API key" : "";
            state.account_label = state.logged_in ? "GLM" : "";
        } else if (state.logged_in) {
            state.account = provider_label(i);
            state.account_label = provider_label(i);
        }
    }
    if (!g_app.enabled[g_app.selected]) {
        for (int i = 0; i < kProviderCount; ++i) if (g_app.enabled[i]) { g_app.selected = i; break; }
    }
}

std::optional<std::filesystem::path> first_existing_font(const std::vector<std::filesystem::path>& paths) {
    for (const auto& path : paths) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) return path;
    }
    return std::nullopt;
}

bool init_fonts() {
    auto regular = first_existing_font({
        "C:/Windows/Fonts/segoeui.ttf",
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"
    });
    auto bold = first_existing_font({
        "C:/Windows/Fonts/segoeuib.ttf",
        "/System/Library/Fonts/SFNS.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf"
    });
    if (!regular) return false;
    g_ui.font_path = *regular;
    g_ui.font_bold_path = bold ? *bold : *regular;
    return load_fonts_for_scale(g_ui.render_scale);
}

} // namespace

int main(int argc, char** argv) {
    bool debug = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--debug") debug = true;
    diagnostics_init(debug);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        return 1;
    }
    if (!TTF_Init()) {
        SDL_Quit();
        return 1;
    }
    SDL_SetAppMetadata("LLM Usage Tray", LLM_USAGE_TRAY_VERSION, "works.tward.llm-usage-tray");

    g_ui.window = SDL_CreateWindow("LLM Usage Tray", kPanelWidth, 280,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
        SDL_WINDOW_TRANSPARENT | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_ui.window) return 1;
#if defined(__linux__)
    SDL_SetWindowHitTest(g_ui.window, hit_test, nullptr);
#endif
    g_ui.renderer = SDL_CreateRenderer(g_ui.window, nullptr);
    if (!g_ui.renderer) return 1;
    g_ui.premul = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
    g_ui.panel_scale = window_panel_scale();
    SDL_SetWindowSize(g_ui.window, panel_width_px(), panel_height_px());
    SDL_SetRenderVSync(g_ui.renderer, 1);
    SDL_SetRenderDrawBlendMode(g_ui.renderer, SDL_BLENDMODE_BLEND);
    polish_native_window();
    update_render_metrics(false);
    if (!init_fonts()) return 1;
    icons_load(g_ui.renderer);

    init_state();
    create_tray();
    for (int i = 0; i < kProviderCount; ++i) {
        if (provider_has_auth(i)) refresh_usage_async_for(i, true);
    }

    long long last_poll = now_ms();
    long long last_tick = now_ms();
    while (!g_quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            float ex = 0, ey = 0;
            if (event.type == SDL_EVENT_QUIT) g_quit = true;
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                event_logical(event, &ex, &ey);
                handle_mouse_down(ex, ey);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
                event_logical(event, &ex, &ey);
                handle_right_click(ex, ey);
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                event_logical(event, &ex, &ey);
                handle_mouse_motion();
                if (g_ui.visible) update_hover(ex, ey);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
                event_logical(event, &ex, &ey);
                handle_mouse_up(ex, ey);
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
#if defined(__linux__)
                if (now_ms() - g_ui.shown_at_ms > 250) hide_panel();
#endif
            }
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                update_render_metrics();
                update_window_shape();
            }
            else if (event.type == SDL_EVENT_TEXT_INPUT && g_ui.api_key_mode && g_ui.api_input_focused) g_ui.api_key_input += event.text.text;
            else if (event.type == SDL_EVENT_TEXT_INPUT && g_ui.oauth_code_mode && g_ui.oauth_code_input_focused) g_ui.oauth_code_input += event.text.text;
            else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE && !g_ui.api_key_mode && !g_ui.oauth_code_mode) {
                    if (g_ui.settings_open || g_ui.callout_open) close_menus();
                    else hide_panel();
                } else if (g_ui.api_key_mode || g_ui.oauth_code_mode) {
                    bool oauth_code = g_ui.oauth_code_mode;
                    bool paste = ((event.key.mod & SDL_KMOD_CTRL) && event.key.key == SDLK_V) ||
                        ((event.key.mod & SDL_KMOD_SHIFT) && event.key.key == SDLK_INSERT);
                    if (paste) {
                        char* clip = SDL_GetClipboardText();
                        if (clip) {
                            std::string pasted = trim_copy(clip);
                            SDL_free(clip);
                            if (oauth_code) g_ui.oauth_code_input += compact_code(pasted);
                            else g_ui.api_key_input += pasted;
                            if (oauth_code) g_ui.oauth_code_input_focused = true;
                            else g_ui.api_input_focused = true;
                        }
                    } else if (event.key.key == SDLK_BACKSPACE && oauth_code && !g_ui.oauth_code_input.empty() && g_ui.oauth_code_input_focused) g_ui.oauth_code_input.pop_back();
                    else if (event.key.key == SDLK_BACKSPACE && !oauth_code && !g_ui.api_key_input.empty() && g_ui.api_input_focused) g_ui.api_key_input.pop_back();
                    else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                        if (oauth_code) save_oauth_code();
                        else save_glm_key();
                    }
                    else if (event.key.key == SDLK_ESCAPE) {
                        if (oauth_code) cancel_oauth_code_login();
                        else {
                            g_ui.api_key_mode = false;
                            g_ui.api_input_focused = false;
                            set_target_height(wanted_panel_height());
                            SDL_StopTextInput(g_ui.window);
                        }
                    }
                }
            }
        }

        if (g_show_requested) show_panel();
        if (g_refresh_requested.exchange(false)) refresh_usage_async_for(selected_provider(), true);
        if (g_warm_requested.exchange(false)) warm_async_for(selected_provider());

        int wanted_height = wanted_panel_height();
        if (g_ui.target_height != wanted_height) set_target_height(wanted_height);

        if (g_ui.panel_height != g_ui.target_height) {
            int delta = g_ui.target_height - g_ui.panel_height;
            int step = std::max(8, static_cast<int>(std::ceil(std::abs(delta) * 0.38f)));
            g_ui.panel_height += std::abs(delta) <= step ? delta : step * (delta > 0 ? 1 : -1);
            SDL_SetWindowSize(g_ui.window, panel_width_px(), panel_height_px());
            update_render_metrics();
            if (g_ui.anchor_bottom > 0) {
                int wx = 0;
                int wy = 0;
                SDL_GetWindowPosition(g_ui.window, &wx, &wy);
                SDL_SetWindowPosition(g_ui.window, wx, g_ui.anchor_bottom - panel_height_px());
            }
            update_window_shape();
        }

        if (now_ms() - last_poll > 5 * 60 * 1000) {
            last_poll = now_ms();
            for (int i = 0; i < kProviderCount; ++i) {
                if (provider_has_auth(i)) refresh_usage_async_for(i);
            }
        }

        long long tick_now = now_ms();
        float dt = std::clamp(static_cast<float>(tick_now - last_tick) / 1000.0f, 0.001f, 0.05f);
        last_tick = tick_now;
        if (g_ui.visible) {
            tick_ui(dt);
            poll_dismiss();
            draw_panel();
        }
        SDL_Delay(16);
    }

    if (g_ui.tray) SDL_DestroyTray(g_ui.tray);
    if (g_ui.icon) SDL_DestroySurface(g_ui.icon);
    icons_unload();
    close_fonts();
    if (g_ui.renderer) SDL_DestroyRenderer(g_ui.renderer);
    if (g_ui.window) SDL_DestroyWindow(g_ui.window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
