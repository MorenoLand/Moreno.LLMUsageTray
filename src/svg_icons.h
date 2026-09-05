#pragma once

struct SDL_Renderer;
struct SDL_Texture;

void icons_load(SDL_Renderer* renderer);
void icons_unload();
SDL_Texture* icon_provider(int index);
SDL_Texture* icon_gear();
SDL_Texture* icon_pin();
void icons_draw(SDL_Texture* texture, float cx, float cy, float size);
