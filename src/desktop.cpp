#include "wave723/firmware.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int width = 920;
constexpr int height = 580;
constexpr int visibleLines = 24;
constexpr auto lineColumns = 105;

const SDL_FRect openButton{24, 69, 125, 37};
const SDL_FRect traceButton{161, 69, 146, 37};
const SDL_FRect panel{24, 123, 872, 427};

// SDL may deliver a file dialog result on a different thread.
std::mutex dialogMutex;
std::string pickedFile;

void SDLCALL filePicked(void*, const char* const* files, int) {
    if (files && files[0]) {
        const std::lock_guard<std::mutex> lock(dialogMutex);
        pickedFile = files[0];
    }
}

bool contains(const SDL_FRect& rect, float x, float y) {
    const SDL_FPoint point{x, y};
    return SDL_PointInRectFloat(&point, &rect);
}

void color(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b) {
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
}

void button(SDL_Renderer* renderer, const SDL_FRect& rect, const char* label, bool enabled) {
    color(renderer, enabled ? 36 : 30, enabled ? 84 : 42, enabled ? 112 : 53);
    SDL_RenderFillRect(renderer, &rect);
    color(renderer, enabled ? 243 : 139, enabled ? 248 : 153, enabled ? 255 : 170);
    SDL_RenderDebugText(renderer, rect.x + 14, rect.y + 14, label);
}

struct App {
    std::filesystem::path firmware;
    std::vector<std::string> lines{"Open or drop a Wave 723 firmware ZIP."};
    int scroll = 0;

    void show(const std::string& message) {
        lines.clear();
        scroll = 0;
        std::istringstream stream(message);
        for (std::string line; std::getline(stream, line);) {
            while (line.size() > lineColumns) {
                lines.push_back(line.substr(0, lineColumns));
                line.erase(0, lineColumns);
            }
            lines.push_back(line);
        }
    }

    void open(const char* path) {
        firmware.clear();
        try {
            const auto selected = std::filesystem::u8path(path);
            show(wave723::inspectFirmware(selected));
            firmware = selected;
        } catch (const std::exception& error) {
            show(error.what());
        }
    }

    void trace() {
        if (firmware.empty()) return;
        try {
            show(wave723::traceReset(firmware));
        } catch (const std::exception& error) {
            show(error.what());
        }
    }

    void draw(SDL_Renderer* renderer) const {
        color(renderer, 15, 23, 34);
        SDL_RenderClear(renderer);
        color(renderer, 229, 239, 249);
        SDL_RenderDebugText(renderer, 24, 23, "SAMSUNG WAVE 723   /   BCM2156");
        color(renderer, 150, 173, 196);
        SDL_RenderDebugText(renderer, 24, 43, "Firmware and reset trace");
        button(renderer, openButton, "Open ZIP", true);
        button(renderer, traceButton, "Trace reset", !firmware.empty());
        color(renderer, 22, 34, 49);
        SDL_RenderFillRect(renderer, &panel);
        color(renderer, 211, 225, 239);
        for (int row = 0; row < visibleLines && row + scroll < static_cast<int>(lines.size()); ++row) {
            SDL_RenderDebugText(renderer, 37, 136.0f + row * 17, lines[row + scroll].c_str());
        }
        SDL_RenderPresent(renderer);
    }

    void move(int offset) {
        scroll = std::clamp(scroll + offset, 0,
                            std::max(0, static_cast<int>(lines.size()) - visibleLines));
    }
};

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && std::string(argv[1]) == "--smoke";
    SDL_Window* window = nullptr;
    if (SDL_Init(SDL_INIT_VIDEO)) {
        window = SDL_CreateWindow("Wave 723", width, height,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    }
    auto* renderer = window ? SDL_CreateRenderer(window, smoke ? "software" : nullptr) : nullptr;
    if (!renderer || !SDL_SetRenderLogicalPresentation(renderer, width, height,
                                                        SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        std::cerr << "wave723-ui: " << SDL_GetError() << '\n';
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    App app;
    if (argc == 2 && !smoke) app.open(argv[1]);

    bool running = !smoke;
    app.draw(renderer);
    while (running) {
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 50)) {
            SDL_ConvertEventToRenderCoordinates(renderer, &event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data) app.open(event.drop.data);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button != SDL_BUTTON_LEFT) break;
                if (contains(openButton, event.button.x, event.button.y)) {
                    static const SDL_DialogFileFilter filters[] = {{"Firmware ZIP", "zip"}};
                    SDL_ShowOpenFileDialog(filePicked, nullptr, window, filters, 1, nullptr, false);
                } else if (contains(traceButton, event.button.x, event.button.y)) {
                    app.trace();
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (event.wheel.y != 0) app.move(event.wheel.y > 0 ? -3 : 3);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_UP) app.move(-1);
                if (event.key.key == SDLK_DOWN) app.move(1);
                if (event.key.key == SDLK_PAGEUP) app.move(-visibleLines);
                if (event.key.key == SDLK_PAGEDOWN) app.move(visibleLines);
                break;
            default:
                break;
            }
        }
        std::string selection;
        {
            const std::lock_guard<std::mutex> lock(dialogMutex);
            selection.swap(pickedFile);
        }
        if (!selection.empty()) app.open(selection.c_str());
        app.draw(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
