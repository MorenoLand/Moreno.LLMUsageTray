#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

#include "svg_icons.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#ifdef small
#undef small
#endif
#endif

static SDL_Texture* g_provider[5]{};
static SDL_Texture* g_gear = nullptr;
static SDL_Texture* g_pin = nullptr;
static SDL_Renderer* g_renderer = nullptr;

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string exe_dir() {
#if defined(_WIN32)
    wchar_t wbuf[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
    if (!n) return {};
    std::string path;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, nullptr, 0, nullptr, nullptr);
    path.resize(bytes);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, path.data(), bytes, nullptr, nullptr);
    auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(0, slash);
#else
    const char* base = SDL_GetBasePath();
    return base ? std::string(base) : std::string();
#endif
}

static std::string find_svg(const char* name) {
    std::string exe = exe_dir();
    const char* bases[] = { "", "icons/", "resources/icons/" };
    std::string candidates[] = {
        exe + "/icons/" + name,
        exe + "\\icons\\" + name,
        std::string("icons/") + name,
        std::string("resources/icons/") + name,
        std::string("G:/Development/c++/LLMUsageTray/resources/icons/") + name,
    };
    for (const auto& path : candidates) {
        std::string body = read_file(path);
        if (!body.empty() && body.find("<svg") != std::string::npos) return body;
    }
    (void)bases;
    return {};
}

static void paint_white(std::string& svg) {
    auto replace = [&](const std::string& from, const std::string& to) {
        std::size_t pos = 0;
        while ((pos = svg.find(from, pos)) != std::string::npos) {
            svg.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace("currentColor", "#FFFFFF");
    replace("#000000", "#FFFFFF");
    replace("fill=\"#000\"", "fill=\"#FFFFFF\"");
    replace("fill='#000000'", "fill='#FFFFFF'");
}

static SDL_Texture* rasterize(const std::string& raw, bool white, int size) {
    if (raw.empty()) return nullptr;
    std::string svg = raw;
    if (white) paint_white(svg);
    std::vector<char> buf(svg.begin(), svg.end());
    buf.push_back(0);
    NSVGimage* image = nsvgParse(buf.data(), "px", 96.0f);
    if (!image) return nullptr;
    float iw = image->width > 1 ? image->width : 24.0f;
    float ih = image->height > 1 ? image->height : 24.0f;
    float scale = static_cast<float>(size) / std::max(iw, ih);
    std::vector<unsigned char> pixels(size * size * 4, 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return nullptr;
    }
    nsvgRasterize(rast, image, 0, 0, scale, pixels.data(), size, size, size * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    SDL_Surface* surface = SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32, pixels.data(), size * 4);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    return texture;
}

void icons_load(SDL_Renderer* renderer) {
    icons_unload();
    g_renderer = renderer;
    int size = 64;
    g_provider[0] = rasterize(find_svg("codex.svg"), true, size);
    g_provider[1] = rasterize(find_svg("claude-color.svg"), false, size);
    g_provider[2] = rasterize(find_svg("zai.svg"), true, size);
    g_provider[3] = rasterize(find_svg("gemini-color.svg"), false, size);
    g_provider[4] = rasterize(find_svg("grok.svg"), true, size);
    g_gear = rasterize(find_svg("settings.svg"), true, size);
    g_pin = rasterize(find_svg("pin.svg"), true, size);
}

void icons_unload() {
    for (int i = 0; i < 5; ++i) {
        if (g_provider[i]) SDL_DestroyTexture(g_provider[i]);
        g_provider[i] = nullptr;
    }
    if (g_gear) SDL_DestroyTexture(g_gear);
    if (g_pin) SDL_DestroyTexture(g_pin);
    g_gear = nullptr;
    g_pin = nullptr;
    g_renderer = nullptr;
}

SDL_Texture* icon_provider(int index) {
    if (index < 0 || index > 4) return nullptr;
    return g_provider[index];
}

SDL_Texture* icon_gear() { return g_gear; }
SDL_Texture* icon_pin() { return g_pin; }

void icons_draw(SDL_Texture* texture, float cx, float cy, float size) {
    if (!texture || !g_renderer) return;
    SDL_FRect dst{cx - size * 0.5f, cy - size * 0.5f, size, size};
    SDL_RenderTexture(g_renderer, texture, nullptr, &dst);
}
