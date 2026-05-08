#include "lsystem.h"
#include "turtle.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int initial_width = 1280;
constexpr int initial_height = 900;
constexpr double render_margin = 48.0;
constexpr auto reload_interval = std::chrono::milliseconds(350);

struct RenderModel {
    LoadedLSystem loaded;
    std::string generated;
    TurtleGeometry geometry;
};

struct Bounds {
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 1.0;
    double max_y = 1.0;
};

struct Rgba {
    Uint8 r = 0;
    Uint8 g = 0;
    Uint8 b = 0;
    Uint8 a = 255;
};

struct Palette {
    Rgba background{.r = 195, .g = 213, .b = 222, .a = 255};
    std::array<Rgba, 3> lines{
        Rgba{.r = 43, .g = 72, .b = 48, .a = 255},
        Rgba{.r = 38, .g = 105, .b = 57, .a = 255},
        Rgba{.r = 230, .g = 236, .b = 229, .a = 255},
    };
    std::array<Rgba, 3> fills{
        Rgba{.r = 43, .g = 72, .b = 48, .a = 190},
        Rgba{.r = 74, .g = 142, .b = 79, .a = 220},
        Rgba{.r = 247, .g = 249, .b = 241, .a = 240},
    };
};

constexpr Palette palette;

struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const {
        SDL_DestroySurface(surface);
    }
};

using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

enum class PixelKind {
    background,
    stem,
    leaf,
    flower,
};

struct RenderScratch {
    SDL_Texture* target = nullptr;
    SDL_Texture* post_processed = nullptr;
    std::vector<Uint8> pixels;
    std::vector<int> plant_pixels;
    std::mt19937 rng{0x5eed1234U};
    int width = 0;
    int height = 0;
    bool dirty = true;

    RenderScratch(const RenderScratch&) = delete;
    RenderScratch& operator=(const RenderScratch&) = delete;

    RenderScratch() = default;

    ~RenderScratch() {
        if (target != nullptr) {
            SDL_DestroyTexture(target);
        }
        if (post_processed != nullptr) {
            SDL_DestroyTexture(post_processed);
        }
    }
};

class SdlContext {
  public:
    SdlContext() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    SdlContext(const SdlContext&) = delete;
    SdlContext& operator=(const SdlContext&) = delete;

    ~SdlContext() {
        SDL_Quit();
    }
};

class WindowRenderer {
  public:
    WindowRenderer() {
        window_ = SDL_CreateWindow("lsystems", initial_width, initial_height, SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) {
            throw std::runtime_error(SDL_GetError());
        }

        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            throw std::runtime_error(SDL_GetError());
        }

        (void)SDL_SetRenderVSync(renderer_, 1);
    }

    WindowRenderer(const WindowRenderer&) = delete;
    WindowRenderer& operator=(const WindowRenderer&) = delete;

    ~WindowRenderer() {
        if (renderer_ != nullptr) {
            SDL_DestroyRenderer(renderer_);
        }
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
        }
    }

    SDL_Window* window() const {
        return window_;
    }

    SDL_Renderer* renderer() const {
        return renderer_;
    }

  private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

std::filesystem::path default_lsystem_path() {
    return std::filesystem::path(LSYSTEMS_EXAMPLES_DIR) / "abop-page-27-inspired.lsys";
}

void print_usage(const char* executable) {
    std::cerr << "usage: " << executable << " [--print] [path/to/file.lsys]\n";
}

void add_point_to_bounds(Bounds& bounds, Vec2 point) {
    bounds.min_x = std::min(bounds.min_x, point.x);
    bounds.min_y = std::min(bounds.min_y, point.y);
    bounds.max_x = std::max(bounds.max_x, point.x);
    bounds.max_y = std::max(bounds.max_y, point.y);
}

Bounds compute_bounds(const TurtleGeometry& geometry) {
    if (geometry.lines.empty() && geometry.polygons.empty()) {
        return Bounds{};
    }

    Vec2 first_point{};
    if (!geometry.lines.empty()) {
        first_point = geometry.lines.front().from;
    } else {
        first_point = geometry.polygons.front().points.front();
    }

    Bounds bounds{
        .min_x = first_point.x,
        .min_y = first_point.y,
        .max_x = first_point.x,
        .max_y = first_point.y,
    };

    for (const LineSegment& line : geometry.lines) {
        const std::array<Vec2, 2> points = {line.from, line.to};
        for (const Vec2 point : points) {
            add_point_to_bounds(bounds, point);
        }
    }

    for (const Polygon& polygon : geometry.polygons) {
        for (const Vec2 point : polygon.points) {
            add_point_to_bounds(bounds, point);
        }
    }

    return bounds;
}

RenderModel load_render_model(const std::filesystem::path& path) {
    LoadedLSystem loaded = load_lsystem_file(path);
    std::string generated = expand_lsystem(loaded.definition);
    TurtleGeometry geometry = build_turtle_geometry(generated, loaded.definition);

    std::cerr << "loaded " << path << " | symbols: " << generated.size()
              << " | lines: " << geometry.lines.size()
              << " | polygons: " << geometry.polygons.size() << '\n';

    return RenderModel{
        .loaded = std::move(loaded),
        .generated = std::move(generated),
        .geometry = std::move(geometry),
    };
}

Vec2 transform_point(Vec2 point, const Bounds& bounds, int width, int height) {
    const double usable_width = std::max(1.0, static_cast<double>(width) - render_margin * 2.0);
    const double usable_height = std::max(1.0, static_cast<double>(height) - render_margin * 2.0);
    const double bounds_width = std::max(1.0, bounds.max_x - bounds.min_x);
    const double bounds_height = std::max(1.0, bounds.max_y - bounds.min_y);
    const double scale = std::min(usable_width / bounds_width, usable_height / bounds_height);

    const double drawn_width = bounds_width * scale;
    const double drawn_height = bounds_height * scale;
    const double offset_x = (static_cast<double>(width) - drawn_width) * 0.5;
    const double offset_y = (static_cast<double>(height) - drawn_height) * 0.5;

    return Vec2{
        .x = offset_x + (point.x - bounds.min_x) * scale,
        .y = offset_y + (point.y - bounds.min_y) * scale,
    };
}

Rgba color_at(const std::array<Rgba, 3>& colors, int index) {
    if (index < 0) {
        return colors.front();
    }

    const std::size_t clamped = std::min(static_cast<std::size_t>(index), colors.size() - 1U);
    return colors[clamped];
}

SDL_FPoint to_fpoint(Vec2 point) {
    return SDL_FPoint{
        .x = static_cast<float>(point.x),
        .y = static_cast<float>(point.y),
    };
}

int color_distance_squared(Rgba left, Rgba right) {
    const int red = static_cast<int>(left.r) - static_cast<int>(right.r);
    const int green = static_cast<int>(left.g) - static_cast<int>(right.g);
    const int blue = static_cast<int>(left.b) - static_cast<int>(right.b);
    return red * red + green * green + blue * blue;
}

Rgba blend_over_background(Rgba color) {
    const int alpha = static_cast<int>(color.a);
    const int inverse_alpha = 255 - alpha;

    return Rgba{
        .r = static_cast<Uint8>((static_cast<int>(color.r) * alpha +
                                 static_cast<int>(palette.background.r) * inverse_alpha) /
                                255),
        .g = static_cast<Uint8>((static_cast<int>(color.g) * alpha +
                                 static_cast<int>(palette.background.g) * inverse_alpha) /
                                255),
        .b = static_cast<Uint8>((static_cast<int>(color.b) * alpha +
                                 static_cast<int>(palette.background.b) * inverse_alpha) /
                                255),
        .a = 255,
    };
}

Rgba read_pixel(const std::vector<Uint8>& pixels, int index) {
    const std::size_t offset = static_cast<std::size_t>(index) * 4U;
    return Rgba{
        .r = pixels[offset],
        .g = pixels[offset + 1U],
        .b = pixels[offset + 2U],
        .a = pixels[offset + 3U],
    };
}

void write_pixel(std::vector<Uint8>& pixels, int index, Rgba color) {
    const std::size_t offset = static_cast<std::size_t>(index) * 4U;
    pixels[offset] = color.r;
    pixels[offset + 1U] = color.g;
    pixels[offset + 2U] = color.b;
    pixels[offset + 3U] = color.a;
}

Rgba mix_color(Rgba from, Rgba to, double amount) {
    const double clamped = std::clamp(amount, 0.0, 1.0);
    const auto channel = [clamped](Uint8 left, Uint8 right) {
        const double mixed =
            static_cast<double>(left) * (1.0 - clamped) + static_cast<double>(right) * clamped;
        return static_cast<Uint8>(std::clamp(mixed, 0.0, 255.0));
    };

    return Rgba{
        .r = channel(from.r, to.r),
        .g = channel(from.g, to.g),
        .b = channel(from.b, to.b),
        .a = 255,
    };
}

PixelKind classify_pixel(Rgba color) {
    if (color_distance_squared(color, palette.background) < 180) {
        return PixelKind::background;
    }

    const Rgba stem = palette.lines[0];
    const Rgba leaf = blend_over_background(palette.fills[1]);
    const Rgba flower = blend_over_background(palette.fills[2]);

    const int stem_distance = color_distance_squared(color, stem);
    const int leaf_distance = color_distance_squared(color, leaf);
    const int flower_distance = color_distance_squared(color, flower);

    if (flower_distance <= leaf_distance && flower_distance <= stem_distance) {
        return PixelKind::flower;
    }

    if (leaf_distance <= stem_distance) {
        return PixelKind::leaf;
    }

    return PixelKind::stem;
}

int smear_length_for_kind(PixelKind kind, std::mt19937& rng) {
    switch (kind) {
    case PixelKind::stem: {
        std::uniform_int_distribution<int> distribution(6, 18);
        return distribution(rng);
    }
    case PixelKind::leaf: {
        std::uniform_int_distribution<int> distribution(8, 24);
        return distribution(rng);
    }
    case PixelKind::flower: {
        std::uniform_int_distribution<int> distribution(5, 14);
        return distribution(rng);
    }
    case PixelKind::background:
        break;
    }

    return 0;
}

std::vector<int> collect_plant_pixels(const std::vector<Uint8>& pixels, int width, int height) {
    std::vector<int> plant_pixels;
    plant_pixels.reserve(static_cast<std::size_t>(width * height / 12));

    for (int index = 0; index < width * height; ++index) {
        if (classify_pixel(read_pixel(pixels, index)) != PixelKind::background) {
            plant_pixels.push_back(index);
        }
    }

    return plant_pixels;
}

void apply_vertical_paint_pulls(std::vector<Uint8>& pixels, const std::vector<int>& plant_pixels,
                                int width, int height, int strokes, std::mt19937& rng) {
    if (width <= 0 || height <= 0) {
        return;
    }

    if (plant_pixels.empty()) {
        return;
    }

    std::uniform_int_distribution<std::size_t> pixel_distribution(0U, plant_pixels.size() - 1U);
    std::uniform_int_distribution<int> horizontal_distribution(-1, 1);

    for (int stroke = 0; stroke < strokes; ++stroke) {
        const int source_index = plant_pixels[pixel_distribution(rng)];
        const int source_x = source_index % width;
        const int source_y = source_index / width;
        const PixelKind kind = classify_pixel(read_pixel(pixels, source_index));
        const int length = smear_length_for_kind(kind, rng);
        const int target_y = std::min(height - 1, source_y + length);
        const int drift = horizontal_distribution(rng);
        const Rgba source = read_pixel(pixels, source_index);
        const Rgba target = read_pixel(pixels, target_y * width + source_x);

        for (int step = 1; step <= length && source_y + step < height; ++step) {
            const double amount = static_cast<double>(step) / static_cast<double>(length + 1);
            const int x = std::clamp(source_x + (drift * step) / std::max(1, length), 0, width - 1);
            const int y = source_y + step;
            const int destination_index = y * width + x;
            const Rgba existing = read_pixel(pixels, destination_index);
            const Rgba pulled = mix_color(source, target, amount);
            const double opacity = kind == PixelKind::flower ? 0.38 : 0.28;
            write_pixel(pixels, destination_index, mix_color(existing, pulled, opacity));
        }
    }
}

void render_polygon(SDL_Renderer* renderer, const Polygon& polygon, const Bounds& bounds, int width,
                    int height) {
    if (polygon.points.size() < 3U) {
        return;
    }

    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(polygon.points.size());
    indices.reserve((polygon.points.size() - 2U) * 3U);

    for (const Vec2 point : polygon.points) {
        const Vec2 transformed = transform_point(point, bounds, width, height);
        const Rgba fill = color_at(palette.fills, polygon.color_index);
        vertices.push_back(SDL_Vertex{
            .position = to_fpoint(transformed),
            .color =
                SDL_FColor{static_cast<float>(fill.r) / 255.0F, static_cast<float>(fill.g) / 255.0F,
                           static_cast<float>(fill.b) / 255.0F,
                           static_cast<float>(fill.a) / 255.0F},
            .tex_coord = SDL_FPoint{},
        });
    }

    for (std::size_t index = 1; index + 1U < polygon.points.size(); ++index) {
        indices.push_back(0);
        indices.push_back(static_cast<int>(index));
        indices.push_back(static_cast<int>(index + 1U));
    }

    (void)SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()),
                             indices.data(), static_cast<int>(indices.size()));
}

void render_geometry_to_current_target(SDL_Renderer* renderer, const TurtleGeometry& geometry,
                                       int width, int height) {
    (void)SDL_SetRenderDrawColor(renderer, palette.background.r, palette.background.g,
                                 palette.background.b, palette.background.a);
    (void)SDL_RenderClear(renderer);

    const Bounds bounds = compute_bounds(geometry);

    for (const LineSegment& line : geometry.lines) {
        const Rgba color = color_at(palette.lines, line.color_index);
        (void)SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        const Vec2 from = transform_point(line.from, bounds, width, height);
        const Vec2 to = transform_point(line.to, bounds, width, height);
        (void)SDL_RenderLine(renderer, static_cast<float>(from.x), static_cast<float>(from.y),
                             static_cast<float>(to.x), static_cast<float>(to.y));
    }

    for (const Polygon& polygon : geometry.polygons) {
        render_polygon(renderer, polygon, bounds, width, height);
    }
}

void ensure_scratch_textures(SDL_Renderer* renderer, RenderScratch& scratch, int width,
                             int height) {
    if (scratch.width == width && scratch.height == height && scratch.target != nullptr &&
        scratch.post_processed != nullptr) {
        return;
    }

    if (scratch.target != nullptr) {
        SDL_DestroyTexture(scratch.target);
    }
    if (scratch.post_processed != nullptr) {
        SDL_DestroyTexture(scratch.post_processed);
    }

    scratch.width = width;
    scratch.height = height;
    scratch.dirty = true;
    scratch.pixels.clear();
    scratch.plant_pixels.clear();
    scratch.rng.seed(0x5eed1234U);
    scratch.target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                       width, height);
    scratch.post_processed = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                               SDL_TEXTUREACCESS_STREAMING, width, height);

    if (scratch.target == nullptr || scratch.post_processed == nullptr) {
        throw std::runtime_error(SDL_GetError());
    }
}

std::vector<Uint8> read_rgba_pixels(SDL_Renderer* renderer, int width, int height) {
    SurfacePtr raw(SDL_RenderReadPixels(renderer, nullptr));
    if (!raw) {
        throw std::runtime_error(SDL_GetError());
    }

    SurfacePtr converted(SDL_ConvertSurface(raw.get(), SDL_PIXELFORMAT_RGBA32));
    if (!converted) {
        throw std::runtime_error(SDL_GetError());
    }

    std::vector<Uint8> pixels(static_cast<std::size_t>(width * height * 4));
    const auto* source = static_cast<const Uint8*>(converted->pixels);

    for (int y = 0; y < height; ++y) {
        const std::size_t destination_offset = static_cast<std::size_t>(y * width * 4);
        const std::size_t source_offset = static_cast<std::size_t>(y * converted->pitch);
        std::copy(source + source_offset, source + source_offset + width * 4,
                  pixels.begin() + static_cast<std::ptrdiff_t>(destination_offset));
    }

    return pixels;
}

void render(SDL_Renderer* renderer, RenderScratch& scratch, const TurtleGeometry& geometry) {
    int width = initial_width;
    int height = initial_height;
    (void)SDL_GetRenderOutputSize(renderer, &width, &height);

    ensure_scratch_textures(renderer, scratch, width, height);

    if (scratch.dirty) {
        if (!SDL_SetRenderTarget(renderer, scratch.target)) {
            throw std::runtime_error(SDL_GetError());
        }

        render_geometry_to_current_target(renderer, geometry, width, height);
        scratch.pixels = read_rgba_pixels(renderer, width, height);
        scratch.plant_pixels = collect_plant_pixels(scratch.pixels, width, height);
        scratch.dirty = false;
    }

    apply_vertical_paint_pulls(scratch.pixels, scratch.plant_pixels, width, height, 1400,
                               scratch.rng);

    if (!SDL_UpdateTexture(scratch.post_processed, nullptr, scratch.pixels.data(), width * 4)) {
        throw std::runtime_error(SDL_GetError());
    }

    if (!SDL_SetRenderTarget(renderer, nullptr)) {
        throw std::runtime_error(SDL_GetError());
    }

    (void)SDL_SetRenderDrawColor(renderer, palette.background.r, palette.background.g,
                                 palette.background.b, palette.background.a);
    (void)SDL_RenderClear(renderer);
    (void)SDL_RenderTexture(renderer, scratch.post_processed, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

bool should_reload(const std::filesystem::path& path, const RenderModel& model) {
    return std::filesystem::exists(path) &&
           std::filesystem::last_write_time(path) != model.loaded.modified_at;
}

std::filesystem::path parse_args(int argc, char** argv, bool& print_only) {
    std::filesystem::path path = default_lsystem_path();

    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--print") {
            print_only = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            path = arg;
        }
    }

    return path;
}

void update_window_title(SDL_Window* window, const std::filesystem::path& path,
                         const RenderModel& model) {
    const std::string title = "lsystems - " + path.filename().string() + " - " +
                              std::to_string(model.generated.size()) + " symbols";
    SDL_SetWindowTitle(window, title.c_str());
}

} // namespace

int main(int argc, char** argv) {
    try {
        bool print_only = false;
        const std::filesystem::path path = parse_args(argc, argv, print_only);
        RenderModel model = load_render_model(path);

        if (print_only) {
            std::cout << model.generated << '\n';
            return 0;
        }

        const SdlContext sdl;
        WindowRenderer display;
        RenderScratch scratch;
        update_window_title(display.window(), path, model);

        bool running = true;
        auto next_reload_check = std::chrono::steady_clock::now() + reload_interval;

        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    running = false;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_reload_check) {
                next_reload_check = now + reload_interval;
                if (should_reload(path, model)) {
                    try {
                        model = load_render_model(path);
                        update_window_title(display.window(), path, model);
                        scratch.dirty = true;
                    } catch (const std::exception& error) {
                        std::cerr << "reload failed: " << error.what() << '\n';
                    }
                }
            }

            render(display.renderer(), scratch, model.geometry);
            SDL_Delay(16);
        }
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
