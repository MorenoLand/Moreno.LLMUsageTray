#include "oauth.h"
#include "usage.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_tray.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kPanelWidth = 340;
constexpr int kPanelCollapsedHeight = 200;
constexpr int kPanelApiKeyHeight = 204;
constexpr int kPanelExpandedHeight = 344;
constexpr int kProviderCount = 3;

struct Rect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct ProviderState {
    bool busy = false;
    bool logged_in = false;
    std::string status = "Not logged in";
    std::string account;
    std::string primary = "5h: unknown";
    std::string secondary = "Weekly: unknown";
    double primary_left = 0;
    double secondary_left = 0;
    long long last_refresh_ms = 0;
};

struct AppState {
    std::mutex mutex;
    ProviderState providers[kProviderCount];
    int selected = 0;
};

struct UiState {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Tray* tray = nullptr;
    SDL_Surface* icon = nullptr;
    TTF_Font* font = nullptr;
    TTF_Font* font_bold = nullptr;
    bool visible = false;
    bool pinned = false;
    bool drawer_open = false;
    bool api_key_mode = false;
    bool api_input_focused = false;
    bool dragging = false;
    bool drag_moved = false;
    int drag_offset_x = 0;
    int drag_offset_y = 0;
    int panel_height = kPanelCollapsedHeight;
    int target_height = kPanelCollapsedHeight;
    int anchor_bottom = 0;
    double drawer_anim = 0.0;
    std::string api_key_input;
    Rect gpt_tab, claude_tab, glm_tab;
    Rect pin_button, drawer_button;
    Rect login_button, refresh_button, warm_button, logout_button, quit_button;
    Rect api_input, api_ok, api_cancel;
};

AppState g_app;
UiState g_ui;
std::atomic_bool g_quit{false};
std::atomic_bool g_show_requested{false};
std::atomic_bool g_refresh_requested{false};
std::atomic_bool g_warm_requested{false};

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* provider_key(int index) {
    if (index == 2) return "glm";
    return index == 1 ? "anthropic" : "openai";
}

const char* provider_label(int index) {
    if (index == 2) return "GLM";
    return index == 1 ? "Claude" : "GPT";
}

const char* primary_label(int index) {
    return index == 2 ? "5h" : "5h";
}

const char* secondary_label(int index) {
    if (index == 2) return "Requests";
    return "Weekly";
}

const char* primary_row_label(int index) {
    return "5 hour";
}

const char* secondary_row_label(int index) {
    return index == 2 ? "Requests" : "Weekly";
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

bool over_click_target(float x, float y) {
    if (g_ui.api_key_mode) {
        return contains(g_ui.api_input, x, y) || contains(g_ui.api_ok, x, y) || contains(g_ui.api_cancel, x, y);
    }
    if (contains(g_ui.gpt_tab, x, y) || contains(g_ui.claude_tab, x, y) || contains(g_ui.glm_tab, x, y)) return true;
    if (contains(g_ui.pin_button, x, y) || contains(g_ui.drawer_button, x, y)) return true;
    if (!g_ui.drawer_open) return false;
    return contains(g_ui.login_button, x, y) || contains(g_ui.refresh_button, x, y) ||
        contains(g_ui.warm_button, x, y) || contains(g_ui.logout_button, x, y) ||
        contains(g_ui.quit_button, x, y);
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

void update_window_shape() {
    SDL_Surface* shape = SDL_CreateSurface(kPanelWidth, g_ui.panel_height, SDL_PIXELFORMAT_RGBA32);
    if (!shape) return;
    SDL_ClearSurface(shape, 0, 0, 0, 0);
    fill_surface_round(shape, kPanelWidth, g_ui.panel_height, 9, SDL_MapSurfaceRGBA(shape, 255, 255, 255, 255));
    SDL_SetWindowShape(g_ui.window, shape);
    SDL_DestroySurface(shape);
}

int wanted_panel_height() {
    if (g_ui.api_key_mode) return kPanelApiKeyHeight;
    return g_ui.drawer_open ? kPanelExpandedHeight : kPanelCollapsedHeight;
}

void anchor_current_bottom() {
    int wx = 0;
    int wy = 0;
    SDL_GetWindowPosition(g_ui.window, &wx, &wy);
    g_ui.anchor_bottom = wy + g_ui.panel_height;
}

void set_target_height(int height, bool immediate = false) {
    if (g_ui.visible && g_ui.anchor_bottom <= 0) anchor_current_bottom();
    g_ui.target_height = height;
    if (!immediate) return;
    g_ui.panel_height = height;
    SDL_SetWindowSize(g_ui.window, kPanelWidth, g_ui.panel_height);
    if (g_ui.anchor_bottom > 0) {
        int wx = 0;
        int wy = 0;
        SDL_GetWindowPosition(g_ui.window, &wx, &wy);
        SDL_SetWindowPosition(g_ui.window, wx, g_ui.anchor_bottom - g_ui.panel_height);
    }
    update_window_shape();
}

void show_panel() {
    g_show_requested = false;
    g_ui.visible = true;
    g_ui.panel_height = wanted_panel_height();
    g_ui.target_height = g_ui.panel_height;
    SDL_SetWindowSize(g_ui.window, kPanelWidth, g_ui.panel_height);
    float mx = 0, my = 0;
    SDL_GetGlobalMouseState(&mx, &my);
    g_ui.anchor_bottom = static_cast<int>(my) - 12;
    SDL_SetWindowPosition(g_ui.window, static_cast<int>(mx) - kPanelWidth + 20, g_ui.anchor_bottom - g_ui.panel_height);
    update_window_shape();
    SDL_ShowWindow(g_ui.window);
    SDL_RaiseWindow(g_ui.window);
}

void hide_panel() {
    if (!g_ui.pinned && !g_ui.api_key_mode) {
        g_ui.visible = false;
        SDL_HideWindow(g_ui.window);
    }
}

void refresh_usage_async_for(int provider_index, bool force = false) {
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[provider_index];
        if (state.busy) return;
        if (!force && state.last_refresh_ms > 0 && now_ms() - state.last_refresh_ms < 5 * 60 * 1000) return;
        state.busy = true;
        state.status = "Refreshing " + std::string(provider_label(provider_index)) + "...";
    }

    std::thread([provider_index] {
        try {
            UsageInfo info = fetch_usage_with_auth_provider(provider_key(provider_index));
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[provider_index];
            state.logged_in = true;
            std::string plan = pretty_plan(info.plan_type);
            if (provider_index == 1) state.account = "Claude";
            else if (provider_index == 2) state.account = "GLM API key";
            else state.account = info.email.empty() ? plan : info.email + " (" + plan + ")";
            state.primary = format_usage_line(primary_label(provider_index), info.primary);
            state.secondary = format_usage_line(secondary_label(provider_index), info.secondary);
            state.primary_left = std::clamp(100.0 - info.primary.used_percent, 0.0, 100.0);
            state.secondary_left = std::clamp(100.0 - info.secondary.used_percent, 0.0, 100.0);
            state.status = "Updated";
            state.last_refresh_ms = now_ms();
            state.busy = false;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[provider_index];
            state.logged_in = provider_has_auth(provider_index);
            state.status = e.what();
            state.busy = false;
        }
    }).detach();
}

void warm_async_for(int provider_index) {
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[provider_index];
        if (state.busy || !state.logged_in) return;
        state.busy = true;
        state.status = "Warming " + std::string(provider_label(provider_index)) + "...";
    }

    std::thread([provider_index] {
        try {
            warm_provider(provider_key(provider_index));
            {
                std::lock_guard<std::mutex> lock(g_app.mutex);
                auto& state = g_app.providers[provider_index];
                state.status = "Warm request sent";
                state.busy = false;
                state.last_refresh_ms = 0;
            }
            refresh_usage_async_for(provider_index, true);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[provider_index];
            state.status = e.what();
            state.busy = false;
        }
    }).detach();
}

void login_async_for(int provider_index) {
    if (provider_index == 2) {
        if (g_ui.visible) anchor_current_bottom();
        g_ui.api_key_mode = true;
        g_ui.api_input_focused = true;
        g_ui.api_key_input.clear();
        set_target_height(kPanelApiKeyHeight);
        SDL_StartTextInput(g_ui.window);
        if (!g_ui.visible) show_panel();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[provider_index];
        if (state.busy) return;
        state.busy = true;
        state.status = "Waiting for " + std::string(provider_label(provider_index)) + " browser login...";
    }

    std::thread([provider_index] {
        try {
            oauth_login_browser_provider(provider_key(provider_index));
            {
                std::lock_guard<std::mutex> lock(g_app.mutex);
                auto& state = g_app.providers[provider_index];
                state.logged_in = true;
                state.status = "Login complete";
                state.busy = false;
            }
            refresh_usage_async_for(provider_index, true);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(g_app.mutex);
            auto& state = g_app.providers[provider_index];
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
        state.status = "API key saved";
        state.last_refresh_ms = 0;
    }
    g_ui.api_key_mode = false;
    g_ui.api_input_focused = false;
    set_target_height(g_ui.drawer_open ? kPanelExpandedHeight : kPanelCollapsedHeight);
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

void aa_round_rect(Rect r, float radius, SDL_Color fill_color, SDL_Color border_color, float border_width = 1.0f) {
    constexpr int scale = 3;
    int sw = std::max(1, static_cast<int>(std::ceil(r.w * scale)));
    int sh = std::max(1, static_cast<int>(std::ceil(r.h * scale)));
    SDL_Surface* surface = SDL_CreateSurface(sw, sh, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return;
    SDL_ClearSurface(surface, 0, 0, 0, 0);

    Uint32 fill_px = SDL_MapSurfaceRGBA(surface, fill_color.r, fill_color.g, fill_color.b, fill_color.a);
    Uint32 border_px = SDL_MapSurfaceRGBA(surface, border_color.r, border_color.g, border_color.b, border_color.a);
    auto* pixels = static_cast<Uint32*>(surface->pixels);
    int stride = surface->pitch / static_cast<int>(sizeof(Uint32));
    for (int py = 0; py < sh; ++py) {
        for (int px = 0; px < sw; ++px) {
            float x = (static_cast<float>(px) + 0.5f) / scale;
            float y = (static_cast<float>(py) + 0.5f) / scale;
            if (!inside_round_rect(x, y, r.w, r.h, radius)) continue;
            bool inner = x >= border_width && y >= border_width &&
                x < r.w - border_width && y < r.h - border_width &&
                inside_round_rect(x - border_width, y - border_width,
                    r.w - border_width * 2.0f, r.h - border_width * 2.0f,
                    std::max(0.0f, radius - border_width));
            pixels[py * stride + px] = inner ? fill_px : border_px;
        }
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_ui.renderer, surface);
    if (texture) {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
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

TTF_Font* pick_font(bool bold) {
    return bold && g_ui.font_bold ? g_ui.font_bold : g_ui.font;
}

std::pair<int, int> measure_text(const std::string& s, bool bold = false) {
    int w = 0;
    int h = 0;
    TTF_Font* font = pick_font(bold);
    if (!font || s.empty()) return {0, 0};
    TTF_GetStringSize(font, s.c_str(), s.size(), &w, &h);
    return {w, h};
}

void text(float x, float y, const std::string& s, Uint8 r = 231, Uint8 g = 238, Uint8 b = 234, bool bold = false) {
    if (s.empty()) return;
    TTF_Font* font = pick_font(bold);
    if (!font) return;
    SDL_Surface* surface = TTF_RenderText_Blended(font, s.c_str(), s.size(), color(r, g, b));
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_ui.renderer, surface);
    if (texture) {
        SDL_FRect dst{x, y, static_cast<float>(surface->w), static_cast<float>(surface->h)};
        SDL_RenderTexture(g_ui.renderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }
    SDL_DestroySurface(surface);
}

std::string clip_text(const std::string& s, int max_width, bool bold = false) {
    if (measure_text(s, bold).first <= max_width) return s;
    std::string out = s;
    while (!out.empty() && measure_text(out + "...", bold).first > max_width) out.pop_back();
    return out.empty() ? "..." : out + "...";
}

void button(Rect r, const std::string& label, bool enabled = true) {
    aa_round_rect(r, 4,
        color(enabled ? 41 : 35, enabled ? 48 : 39, enabled ? 52 : 42),
        color(enabled ? 82 : 55, enabled ? 96 : 61, enabled ? 90 : 64));
    auto [tw, th] = measure_text(label);
    float tx = r.x + std::max(6.0f, (r.w - static_cast<float>(tw)) / 2.0f);
    float ty = r.y + std::max(2.0f, (r.h - static_cast<float>(th)) / 2.0f);
    text(tx, ty, label, enabled ? 238 : 115, enabled ? 243 : 123, enabled ? 240 : 119);
}

void tab(Rect r, const std::string& label, bool selected) {
    aa_round_rect(r, 4,
        color(selected ? 46 : 30, selected ? 57 : 35, selected ? 55 : 38),
        color(selected ? 82 : 62, selected ? 172 : 70, selected ? 123 : 74));
    auto [tw, th] = measure_text(label);
    text(r.x + (r.w - tw) / 2.0f, r.y + (r.h - th) / 2.0f - 1.0f,
        label, selected ? 246 : 171, selected ? 249 : 181, selected ? 247 : 176);
}

void usage_bar(float x, float y, float width, const std::string& label, const std::string& detail, double left, bool blue) {
    std::string shown = detail;
    if (auto p = shown.find(": "); p != std::string::npos) shown = shown.substr(p + 2);
    shown = clip_text(shown, 235);
    text(x, y, label);
    text(x + 70, y, shown, 174, 184, 179);
    fill({x, y + 26, width, 16}, 47, 54, 58);
    float fw = static_cast<float>(std::clamp(left, 0.0, 100.0) / 100.0 * width);
    fill({x, y + 26, fw, 16}, blue ? 82 : 68, blue ? 145 : 188, blue ? 224 : 126);
    outline({x, y + 26, width, 16}, 78, 88, 84);
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

void pushpin_icon(Rect r, bool active) {
    static constexpr int w = 18;
    static constexpr int h = 18;
    static constexpr unsigned char mask[w * h] = {
        0,0,0,0,0,0,0,0,0,0,0,170,255,80,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,255,255,255,255,170,0,0,0,
        0,0,0,0,0,0,0,0,0,170,255,255,255,255,255,170,0,0,
        0,0,0,0,0,0,0,0,80,255,255,255,255,255,255,255,170,0,
        0,0,0,0,0,255,255,255,255,255,255,255,255,255,255,255,255,0,
        0,0,0,0,255,255,255,255,255,255,255,255,255,255,255,255,170,0,
        0,0,0,0,255,255,255,255,255,255,255,255,255,255,255,255,0,0,
        0,0,0,0,170,255,255,255,255,255,255,255,255,255,170,0,0,0,
        0,0,0,0,80,255,255,255,255,255,255,255,255,80,0,0,0,0,
        0,0,0,0,0,80,255,255,255,255,255,255,255,0,0,0,0,0,
        0,0,0,0,0,170,255,255,255,255,255,255,255,0,0,0,0,0,
        0,0,0,0,170,255,170,80,255,255,255,255,170,0,0,0,0,0,
        0,0,0,170,255,170,0,0,80,255,255,255,0,0,0,0,0,0,
        0,0,0,170,170,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };

    SDL_Color c = active ? color(123, 222, 159) : color(178, 184, 180);
    float scale = 0.72f;
    float px = r.x + (r.w - w * scale) / 2.0f;
    float py = r.y + (r.h - h * scale) / 2.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char a = mask[y * w + x];
            if (!a) continue;
            fill({px + x * scale, py + y * scale, scale + 0.25f, scale + 0.25f},
                c.r, c.g, c.b, a);
        }
    }
}

void draw_pin(Rect r, bool active) {
    button(r, "", true);
    pushpin_icon(r, active);
}

void draw_chevron(Rect r, bool open) {
    button(r, "", true);
    set_color(231, 238, 234, 255);
    float cx = r.x + r.w / 2.0f;
    float cy = r.y + r.h / 2.0f;
    if (open) {
        SDL_RenderLine(g_ui.renderer, cx - 5, cy + 3, cx, cy - 3);
        SDL_RenderLine(g_ui.renderer, cx, cy - 3, cx + 5, cy + 3);
    } else {
        SDL_RenderLine(g_ui.renderer, cx - 5, cy - 3, cx, cy + 3);
        SDL_RenderLine(g_ui.renderer, cx, cy + 3, cx + 5, cy - 3);
    }
}

void draw_panel() {
    set_color(0, 0, 0, 0);
    SDL_RenderClear(g_ui.renderer);
    aa_round_rect({0, 0, static_cast<float>(kPanelWidth), static_cast<float>(g_ui.panel_height)}, 9,
        color(23, 26, 28), color(64, 74, 78));

    int selected;
    bool busy, logged_in;
    std::string status, account, primary, secondary;
    double primary_left, secondary_left;
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        selected = g_app.selected;
        const auto& state = g_app.providers[selected];
        busy = state.busy;
        logged_in = state.logged_in;
        status = state.status;
        account = state.account;
        primary = state.primary;
        secondary = state.secondary;
        primary_left = state.primary_left;
        secondary_left = state.secondary_left;
    }

    g_ui.gpt_tab = {18, 12, 50, 24};
    g_ui.claude_tab = {74, 12, 68, 24};
    g_ui.glm_tab = {148, 12, 50, 24};
    tab(g_ui.gpt_tab, "GPT", selected == 0);
    tab(g_ui.claude_tab, "Claude", selected == 1);
    tab(g_ui.glm_tab, "GLM", selected == 2);

    g_ui.pin_button = {270, 12, 24, 24};
    g_ui.drawer_button = {300, 12, 24, 24};
    draw_pin(g_ui.pin_button, g_ui.pinned);
    draw_chevron(g_ui.drawer_button, g_ui.drawer_open);

    if (g_ui.api_key_mode) {
        text(18, 52, "GLM API key");
        text(18, 76, "Paste key. It is saved by the platform secret store.", 174, 184, 179);
        g_ui.api_input = {18, 108, 304, 30};
        fill(g_ui.api_input, 30, 35, 38);
        outline(g_ui.api_input, g_ui.api_input_focused ? 123 : 82, g_ui.api_input_focused ? 222 : 172, g_ui.api_input_focused ? 159 : 123);
        std::string masked(g_ui.api_key_input.size(), '*');
        text(g_ui.api_input.x + 8, g_ui.api_input.y + 10, masked);
        if (g_ui.api_input_focused && ((SDL_GetTicks() / 500) % 2 == 0)) {
            auto [tw, th] = measure_text(masked);
            float cx = std::min(g_ui.api_input.x + 8 + static_cast<float>(tw) + 2.0f, g_ui.api_input.x + g_ui.api_input.w - 10.0f);
            fill({cx, g_ui.api_input.y + 8, 1.5f, 16}, 231, 238, 234);
        }
        g_ui.api_ok = {18, 154, 132, 30};
        g_ui.api_cancel = {190, 154, 132, 30};
        button(g_ui.api_ok, "Save", !g_ui.api_key_input.empty());
        button(g_ui.api_cancel, "Cancel", true);
        SDL_RenderPresent(g_ui.renderer);
        return;
    }

    std::string subtitle = account.empty() ? status : account;
    if (!logged_in) subtitle = selected == 2 ? "Open drawer to add a GLM API key." : "Open drawer to connect " + std::string(provider_label(selected)) + ".";
    subtitle = clip_text(subtitle, 304);
    text(18, 48, subtitle, 171, 181, 176);

    usage_bar(18, 82, 304, primary_row_label(selected), primary, primary_left, false);
    usage_bar(18, 138, 304, secondary_row_label(selected), secondary, secondary_left, true);

    g_ui.login_button = {18, 204, 132, 30};
    g_ui.refresh_button = {164, 204, 158, 30};
    g_ui.warm_button = {18, 244, 304, 30};
    g_ui.logout_button = {18, 284, 132, 30};
    g_ui.quit_button = {164, 284, 158, 30};

    if (g_ui.drawer_open) {
        button(g_ui.login_button, logged_in ? (selected == 2 ? "Key saved" : "Logged in") : (selected == 2 ? "Set API key" : "Login"), !busy && !logged_in);
        button(g_ui.refresh_button, busy ? "Refreshing..." : "Refresh now", !busy && logged_in);
        button(g_ui.warm_button, busy ? "Working..." : "Warm now", !busy && logged_in);
        button(g_ui.logout_button, "Logout", !busy && logged_in);
        button(g_ui.quit_button, "Quit", true);
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
    SDL_SetPointerProperty(props, SDL_PROP_TRAY_CREATE_RIGHTCLICK_CALLBACK_POINTER, reinterpret_cast<void*>(on_tray_right_click));
    g_ui.tray = SDL_CreateTrayWithProperties(props);
    SDL_DestroyProperties(props);
#else
    g_ui.tray = SDL_CreateTray(g_ui.icon, "LLM Usage Tray");
#endif
}

void handle_click(float x, float y) {
    if (g_ui.api_key_mode) {
        if (contains(g_ui.api_input, x, y)) {
            g_ui.api_input_focused = true;
            SDL_StartTextInput(g_ui.window);
        } else if (contains(g_ui.api_ok, x, y)) save_glm_key();
        else if (contains(g_ui.api_cancel, x, y)) {
            g_ui.api_key_mode = false;
            g_ui.api_input_focused = false;
            set_target_height(g_ui.drawer_open ? kPanelExpandedHeight : kPanelCollapsedHeight);
            SDL_StopTextInput(g_ui.window);
        }
        return;
    }

    int selected = selected_provider();
    bool busy, logged_in;
    {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        busy = g_app.providers[selected].busy;
        logged_in = g_app.providers[selected].logged_in;
    }

    if (contains(g_ui.gpt_tab, x, y) || contains(g_ui.claude_tab, x, y) || contains(g_ui.glm_tab, x, y)) {
        std::lock_guard<std::mutex> lock(g_app.mutex);
        g_app.selected = contains(g_ui.glm_tab, x, y) ? 2 : (contains(g_ui.claude_tab, x, y) ? 1 : 0);
    } else if (contains(g_ui.pin_button, x, y)) {
        g_ui.pinned = !g_ui.pinned;
    } else if (contains(g_ui.drawer_button, x, y)) {
        anchor_current_bottom();
        g_ui.drawer_open = !g_ui.drawer_open;
        set_target_height(wanted_panel_height());
    } else if (g_ui.drawer_open && contains(g_ui.login_button, x, y) && !busy && !logged_in) {
        login_async_for(selected);
    } else if (g_ui.drawer_open && contains(g_ui.refresh_button, x, y) && !busy && logged_in) {
        refresh_usage_async_for(selected, true);
    } else if (g_ui.drawer_open && contains(g_ui.warm_button, x, y) && !busy && logged_in) {
        warm_async_for(selected);
    } else if (g_ui.drawer_open && contains(g_ui.logout_button, x, y) && !busy && logged_in) {
        clear_credentials_provider(provider_key(selected));
        std::lock_guard<std::mutex> lock(g_app.mutex);
        auto& state = g_app.providers[selected];
        state.logged_in = false;
        state.account.clear();
        state.status = "Logged out";
        state.primary = std::string(primary_label(selected)) + ": unknown";
        state.secondary = std::string(secondary_label(selected)) + ": unknown";
        state.primary_left = 0;
        state.secondary_left = 0;
        state.last_refresh_ms = 0;
    } else if (g_ui.drawer_open && contains(g_ui.quit_button, x, y)) {
        g_quit = true;
    }
}

void handle_mouse_down(float x, float y) {
    if (over_click_target(x, y)) return;
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
    if (!g_ui.dragging) return;
    float gx = 0;
    float gy = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    int nx = static_cast<int>(gx) - g_ui.drag_offset_x;
    int ny = static_cast<int>(gy) - g_ui.drag_offset_y;
    SDL_SetWindowPosition(g_ui.window, nx, ny);
    g_ui.anchor_bottom = ny + g_ui.panel_height;
    g_ui.drag_moved = true;
}

void handle_mouse_up(float x, float y) {
    bool was_dragging = g_ui.dragging;
    bool moved = g_ui.drag_moved;
    g_ui.dragging = false;
    g_ui.drag_moved = false;
    if (!was_dragging || !moved) handle_click(x, y);
}

void init_state() {
    std::lock_guard<std::mutex> lock(g_app.mutex);
    for (int i = 0; i < kProviderCount; ++i) {
        auto& state = g_app.providers[i];
        state.logged_in = provider_has_auth(i);
        state.status = state.logged_in ? "Ready to refresh " + std::string(provider_label(i)) : "Not logged in";
        if (i == 1) {
            state.primary = "5h: unknown";
            state.secondary = "Weekly: unknown";
        } else if (i == 2) {
            state.status = state.logged_in ? "Ready to refresh GLM" : "No GLM API key saved";
            state.account = state.logged_in ? "GLM API key" : "";
            state.primary = "5h: unknown";
            state.secondary = "Requests: unknown";
        }
    }
}

std::optional<std::filesystem::path> first_existing_font(const std::vector<std::filesystem::path>& paths) {
    for (const auto& path : paths) {
        if (std::filesystem::exists(path)) return path;
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
    g_ui.font = TTF_OpenFont(regular->string().c_str(), 14);
    g_ui.font_bold = TTF_OpenFont((bold ? *bold : *regular).string().c_str(), 14);
    return g_ui.font != nullptr;
}

} // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        return 1;
    }
    if (!TTF_Init()) {
        SDL_Quit();
        return 1;
    }
    SDL_SetAppMetadata("LLM Usage Tray", LLM_USAGE_TRAY_VERSION, "works.tward.llm-usage-tray");

    g_ui.window = SDL_CreateWindow("LLM Usage Tray", kPanelWidth, kPanelCollapsedHeight,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_UTILITY);
    if (!g_ui.window) return 1;
    g_ui.renderer = SDL_CreateRenderer(g_ui.window, nullptr);
    if (!g_ui.renderer) return 1;
    SDL_SetRenderVSync(g_ui.renderer, 1);
    SDL_SetRenderDrawBlendMode(g_ui.renderer, SDL_BLENDMODE_BLEND);
    if (!init_fonts()) return 1;

    init_state();
    create_tray();
    for (int i = 0; i < kProviderCount; ++i) {
        if (provider_has_auth(i)) refresh_usage_async_for(i, true);
    }

    long long last_poll = now_ms();
    while (!g_quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) g_quit = true;
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) handle_mouse_down(event.button.x, event.button.y);
            else if (event.type == SDL_EVENT_MOUSE_MOTION) handle_mouse_motion();
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) handle_mouse_up(event.button.x, event.button.y);
            else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) hide_panel();
            else if (event.type == SDL_EVENT_TEXT_INPUT && g_ui.api_key_mode && g_ui.api_input_focused) g_ui.api_key_input += event.text.text;
            else if (event.type == SDL_EVENT_KEY_DOWN && g_ui.api_key_mode) {
                bool paste = ((event.key.mod & SDL_KMOD_CTRL) && event.key.key == SDLK_V) ||
                    ((event.key.mod & SDL_KMOD_SHIFT) && event.key.key == SDLK_INSERT);
                if (paste) {
                    char* clip = SDL_GetClipboardText();
                    if (clip) {
                        std::string pasted = trim_copy(clip);
                        SDL_free(clip);
                        g_ui.api_key_input += pasted;
                        g_ui.api_input_focused = true;
                    }
                } else if (event.key.key == SDLK_BACKSPACE && !g_ui.api_key_input.empty() && g_ui.api_input_focused) g_ui.api_key_input.pop_back();
                else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) save_glm_key();
                else if (event.key.key == SDLK_ESCAPE) {
                    g_ui.api_key_mode = false;
                    g_ui.api_input_focused = false;
                    set_target_height(g_ui.drawer_open ? kPanelExpandedHeight : kPanelCollapsedHeight);
                    SDL_StopTextInput(g_ui.window);
                }
            }
        }

        if (g_show_requested) show_panel();
        if (g_refresh_requested.exchange(false)) refresh_usage_async_for(selected_provider(), true);
        if (g_warm_requested.exchange(false)) warm_async_for(selected_provider());

        if (g_ui.panel_height != g_ui.target_height) {
            int delta = g_ui.target_height - g_ui.panel_height;
            g_ui.panel_height += std::abs(delta) <= 2 ? delta : std::max(1, std::abs(delta) / 5) * (delta > 0 ? 1 : -1);
            SDL_SetWindowSize(g_ui.window, kPanelWidth, g_ui.panel_height);
            if (g_ui.anchor_bottom > 0) {
                int wx = 0;
                int wy = 0;
                SDL_GetWindowPosition(g_ui.window, &wx, &wy);
                SDL_SetWindowPosition(g_ui.window, wx, g_ui.anchor_bottom - g_ui.panel_height);
            }
            update_window_shape();
        }

        if (now_ms() - last_poll > 5 * 60 * 1000) {
            last_poll = now_ms();
            for (int i = 0; i < kProviderCount; ++i) {
                if (provider_has_auth(i)) refresh_usage_async_for(i);
            }
        }

        if (g_ui.visible) draw_panel();
        SDL_Delay(16);
    }

    if (g_ui.tray) SDL_DestroyTray(g_ui.tray);
    if (g_ui.icon) SDL_DestroySurface(g_ui.icon);
    if (g_ui.font_bold) TTF_CloseFont(g_ui.font_bold);
    if (g_ui.font) TTF_CloseFont(g_ui.font);
    if (g_ui.renderer) SDL_DestroyRenderer(g_ui.renderer);
    if (g_ui.window) SDL_DestroyWindow(g_ui.window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
