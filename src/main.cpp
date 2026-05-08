#include "lsystem.h"
#include "turtle.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int initial_width = 1280;
constexpr int initial_height = 900;
constexpr int default_canvas_width = 240;
constexpr int default_canvas_height = 160;
constexpr double margin_ratio = 1.0 / 30.0;
constexpr double pi = 3.14159265358979323846;
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

struct RenderSettings {
    int canvas_width = default_canvas_width;
    int canvas_height = default_canvas_height;
    int paint_pulls_per_frame = 360;
    int max_pull_length = 5;
    bool pull_stems = true;
    bool pull_leaves = true;
    bool pull_flowers = true;
    bool blur_enabled = true;
    int blur_passes = 1;
    float blur_strength = 0.35F;
    bool wind_enabled = false;
    float wind_strength = 0.0F;
    float wind_direction_degrees = 0.0F;
    bool polygon_hue_variation = true;
    float polygon_hue_amount = 0.035F;
    bool leaf_two_tone = true;
    float leaf_two_tone_strength = 0.22F;
};

struct Hsv {
    double h = 0.0;
    double s = 0.0;
    double v = 0.0;
};

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

struct PaintLayer {
    std::vector<Uint8> pixels;
    std::vector<int> source_pixels;
};

struct RenderScratch {
    SDL_Texture* target = nullptr;
    SDL_Texture* post_processed = nullptr;
    std::vector<Uint8> clean_pixels;
    std::vector<Uint8> composed_pixels;
    std::array<PaintLayer, 3> layers;
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

class UiLayer {
  public:
    UiLayer(SDL_Window* window, SDL_Renderer* renderer) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        ImGui::StyleColorsDark();
        if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
            throw std::runtime_error("failed to initialize ImGui SDL3 backend");
        }
        if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
            throw std::runtime_error("failed to initialize ImGui SDL renderer backend");
        }
    }

    UiLayer(const UiLayer&) = delete;
    UiLayer& operator=(const UiLayer&) = delete;

    ~UiLayer() {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }

    void process_event(const SDL_Event& event) {
        (void)ImGui_ImplSDL3_ProcessEvent(&event);
    }

    void new_frame() {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void render(SDL_Renderer* renderer) {
        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    }
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

double squared_distance(Vec2 left, Vec2 right) {
    const double dx = left.x - right.x;
    const double dy = left.y - right.y;
    return dx * dx + dy * dy;
}

Vec2 root_point(const TurtleGeometry& geometry) {
    if (!geometry.lines.empty()) {
        return geometry.lines.front().from;
    }

    if (!geometry.polygons.empty() && !geometry.polygons.front().points.empty()) {
        return geometry.polygons.front().points.front();
    }

    return Vec2{};
}

double max_distance_from_root(const TurtleGeometry& geometry, Vec2 root) {
    double max_distance_squared = 1.0;

    for (const LineSegment& line : geometry.lines) {
        max_distance_squared = std::max(max_distance_squared, squared_distance(line.from, root));
        max_distance_squared = std::max(max_distance_squared, squared_distance(line.to, root));
    }

    for (const Polygon& polygon : geometry.polygons) {
        for (const Vec2 point : polygon.points) {
            max_distance_squared = std::max(max_distance_squared, squared_distance(point, root));
        }
    }

    return std::sqrt(max_distance_squared);
}

double degrees_to_radians(double degrees) {
    return degrees * pi / 180.0;
}

Vec2 transform_point(Vec2 point, const Bounds& bounds, Vec2 root, double max_root_distance,
                     const RenderSettings& settings, int width, int height) {
    const double render_margin =
        std::max(4.0, static_cast<double>(std::min(width, height)) * margin_ratio);
    const double usable_width = std::max(1.0, static_cast<double>(width) - render_margin * 2.0);
    const double usable_height = std::max(1.0, static_cast<double>(height) - render_margin * 2.0);
    const double bounds_width = std::max(1.0, bounds.max_x - bounds.min_x);
    const double bounds_height = std::max(1.0, bounds.max_y - bounds.min_y);
    const double scale = std::min(usable_width / bounds_width, usable_height / bounds_height);

    const double drawn_width = bounds_width * scale;
    const double drawn_height = bounds_height * scale;
    const double offset_x = (static_cast<double>(width) - drawn_width) * 0.5;
    const double offset_y = (static_cast<double>(height) - drawn_height) * 0.5;

    Vec2 transformed{
        .x = offset_x + (point.x - bounds.min_x) * scale,
        .y = offset_y + (point.y - bounds.min_y) * scale,
    };

    if (settings.wind_enabled && settings.wind_strength != 0.0F) {
        const double distance_from_root =
            std::sqrt(squared_distance(point, root)) / std::max(1.0, max_root_distance);
        const double bend =
            static_cast<double>(settings.wind_strength) * distance_from_root * distance_from_root;
        const double wind_radians = degrees_to_radians(settings.wind_direction_degrees);
        transformed.x += std::cos(wind_radians) * bend;
        transformed.y += std::sin(wind_radians) * bend;
    }

    return transformed;
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

std::size_t pixel_count(int width, int height) {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

std::size_t pixel_byte_count(int width, int height) {
    return pixel_count(width, height) * 4U;
}

void write_pixel(std::vector<Uint8>& pixels, int index, Rgba color) {
    const std::size_t offset = static_cast<std::size_t>(index) * 4U;
    pixels[offset] = color.r;
    pixels[offset + 1U] = color.g;
    pixels[offset + 2U] = color.b;
    pixels[offset + 3U] = color.a;
}

Rgba with_alpha(Rgba color, Uint8 alpha) {
    color.a = alpha;
    return color;
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

double wrap_unit(double value) {
    double wrapped = std::fmod(value, 1.0);
    if (wrapped < 0.0) {
        wrapped += 1.0;
    }
    return wrapped;
}

Hsv rgb_to_hsv(Rgba color) {
    const double red = static_cast<double>(color.r) / 255.0;
    const double green = static_cast<double>(color.g) / 255.0;
    const double blue = static_cast<double>(color.b) / 255.0;
    const double max_channel = std::max({red, green, blue});
    const double min_channel = std::min({red, green, blue});
    const double chroma = max_channel - min_channel;

    double hue = 0.0;
    if (chroma > 0.0) {
        if (max_channel == red) {
            hue = (green - blue) / chroma;
        } else if (max_channel == green) {
            hue = 2.0 + (blue - red) / chroma;
        } else {
            hue = 4.0 + (red - green) / chroma;
        }
        hue = wrap_unit(hue / 6.0);
    }

    return Hsv{
        .h = hue,
        .s = max_channel == 0.0 ? 0.0 : chroma / max_channel,
        .v = max_channel,
    };
}

Rgba hsv_to_rgb(Hsv color, Uint8 alpha) {
    const double hue = wrap_unit(color.h) * 6.0;
    const double saturation = std::clamp(color.s, 0.0, 1.0);
    const double value = std::clamp(color.v, 0.0, 1.0);
    const double chroma = value * saturation;
    const double x = chroma * (1.0 - std::abs(std::fmod(hue, 2.0) - 1.0));
    const double m = value - chroma;

    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;

    if (hue < 1.0) {
        red = chroma;
        green = x;
    } else if (hue < 2.0) {
        red = x;
        green = chroma;
    } else if (hue < 3.0) {
        green = chroma;
        blue = x;
    } else if (hue < 4.0) {
        green = x;
        blue = chroma;
    } else if (hue < 5.0) {
        red = x;
        blue = chroma;
    } else {
        red = chroma;
        blue = x;
    }

    const auto channel = [m](double value_channel) {
        return static_cast<Uint8>(std::clamp((value_channel + m) * 255.0, 0.0, 255.0));
    };

    return Rgba{
        .r = channel(red),
        .g = channel(green),
        .b = channel(blue),
        .a = alpha,
    };
}

Rgba shift_hsv(Rgba color, double hue_shift, double saturation_scale, double value_scale) {
    Hsv hsv = rgb_to_hsv(color);
    hsv.h = wrap_unit(hsv.h + hue_shift);
    hsv.s = std::clamp(hsv.s * saturation_scale, 0.0, 1.0);
    hsv.v = std::clamp(hsv.v * value_scale, 0.0, 1.0);
    return hsv_to_rgb(hsv, color.a);
}

Rgba alpha_blend(Rgba destination, Rgba source) {
    const int source_alpha = static_cast<int>(source.a);
    const int inverse_alpha = 255 - source_alpha;

    return Rgba{
        .r = static_cast<Uint8>((static_cast<int>(source.r) * source_alpha +
                                 static_cast<int>(destination.r) * inverse_alpha) /
                                255),
        .g = static_cast<Uint8>((static_cast<int>(source.g) * source_alpha +
                                 static_cast<int>(destination.g) * inverse_alpha) /
                                255),
        .b = static_cast<Uint8>((static_cast<int>(source.b) * source_alpha +
                                 static_cast<int>(destination.b) * inverse_alpha) /
                                255),
        .a = 255,
    };
}

Rgba layer_blend(Rgba destination, Rgba source, double opacity) {
    const int source_alpha =
        static_cast<int>(std::clamp(static_cast<double>(source.a) * opacity, 0.0, 255.0));
    const int inverse_alpha = 255 - source_alpha;
    const int destination_alpha = static_cast<int>(destination.a);
    const int output_alpha = source_alpha + (destination_alpha * inverse_alpha) / 255;

    if (output_alpha <= 0) {
        return Rgba{};
    }

    const auto channel = [source_alpha, inverse_alpha, destination_alpha,
                          output_alpha](Uint8 source_channel, Uint8 destination_channel) {
        const int destination_part =
            (static_cast<int>(destination_channel) * destination_alpha * inverse_alpha) / 255;
        const int value =
            (static_cast<int>(source_channel) * source_alpha + destination_part) / output_alpha;
        return static_cast<Uint8>(std::clamp(value, 0, 255));
    };

    return Rgba{
        .r = channel(source.r, destination.r),
        .g = channel(source.g, destination.g),
        .b = channel(source.b, destination.b),
        .a = static_cast<Uint8>(std::clamp(output_alpha, 0, 255)),
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

std::size_t layer_index_for_kind(PixelKind kind) {
    switch (kind) {
    case PixelKind::stem:
        return 0U;
    case PixelKind::leaf:
        return 1U;
    case PixelKind::flower:
        return 2U;
    case PixelKind::background:
        break;
    }

    return 0U;
}

Uint8 layer_seed_alpha(PixelKind kind) {
    switch (kind) {
    case PixelKind::stem:
        return 120;
    case PixelKind::leaf:
        return 145;
    case PixelKind::flower:
        return 170;
    case PixelKind::background:
        break;
    }

    return 0;
}

void reset_paint_simulation(RenderScratch& scratch) {
    scratch.dirty = true;
    scratch.rng.seed(0x5eed1234U);
}

int smear_length_for_kind(PixelKind kind, int max_pull_length, std::mt19937& rng) {
    const int clamped_max = std::clamp(max_pull_length, 1, 24);

    switch (kind) {
    case PixelKind::stem: {
        std::uniform_int_distribution<int> distribution(std::max(1, clamped_max / 3), clamped_max);
        return distribution(rng);
    }
    case PixelKind::leaf: {
        std::uniform_int_distribution<int> distribution(std::max(1, clamped_max / 2),
                                                        std::max(1, clamped_max + clamped_max / 2));
        return distribution(rng);
    }
    case PixelKind::flower: {
        std::uniform_int_distribution<int> distribution(1, std::max(1, clamped_max));
        return distribution(rng);
    }
    case PixelKind::background:
        break;
    }

    return 0;
}

void reset_paint_layers(RenderScratch& scratch, int width, int height) {
    for (PaintLayer& layer : scratch.layers) {
        layer.pixels.assign(pixel_byte_count(width, height), 0U);
        layer.source_pixels.clear();
        layer.source_pixels.reserve(pixel_count(width, height) / 24U);
    }

    for (int index = 0; index < width * height; ++index) {
        const Rgba color = read_pixel(scratch.clean_pixels, index);
        const PixelKind kind = classify_pixel(color);
        if (kind != PixelKind::background) {
            PaintLayer& layer = scratch.layers[layer_index_for_kind(kind)];
            write_pixel(layer.pixels, index, with_alpha(color, layer_seed_alpha(kind)));
            layer.source_pixels.push_back(index);
        }
    }
}

void apply_vertical_paint_pulls(PaintLayer& layer, PixelKind kind, int width, int height,
                                int strokes, int max_pull_length, std::mt19937& rng) {
    if (width <= 0 || height <= 0) {
        return;
    }

    if (layer.source_pixels.empty()) {
        return;
    }

    std::uniform_int_distribution<std::size_t> pixel_distribution(0U,
                                                                  layer.source_pixels.size() - 1U);
    std::uniform_int_distribution<int> horizontal_distribution(-1, 1);

    for (int stroke = 0; stroke < strokes; ++stroke) {
        const int source_index = layer.source_pixels[pixel_distribution(rng)];
        const int source_x = source_index % width;
        const int source_y = source_index / width;
        const int length = smear_length_for_kind(kind, max_pull_length, rng);
        const int target_y = std::min(height - 1, source_y + length);
        const int drift = horizontal_distribution(rng);
        const Rgba source = read_pixel(layer.pixels, source_index);
        const Rgba target = read_pixel(layer.pixels, target_y * width + source_x);

        for (int step = 1; step <= length && source_y + step < height; ++step) {
            const double amount = static_cast<double>(step) / static_cast<double>(length + 1);
            const int x = std::clamp(source_x + (drift * step) / std::max(1, length), 0, width - 1);
            const int y = source_y + step;
            const int destination_index = y * width + x;
            const Rgba existing = read_pixel(layer.pixels, destination_index);
            const Rgba pulled =
                target.a == 0 ? with_alpha(source, 75) : mix_color(source, target, amount);
            const double opacity = kind == PixelKind::flower ? 0.45 : 0.34;
            write_pixel(layer.pixels, destination_index, layer_blend(existing, pulled, opacity));
        }
    }
}

Uint8 mix_channel(Uint8 from, Uint8 to, double amount) {
    const double mixed =
        static_cast<double>(from) * (1.0 - amount) + static_cast<double>(to) * amount;
    return static_cast<Uint8>(std::clamp(mixed, 0.0, 255.0));
}

void blur_layer(PaintLayer& layer, int width, int height, float strength) {
    const double clamped_strength = std::clamp(static_cast<double>(strength), 0.0, 1.0);
    if (width <= 2 || height <= 2 || layer.pixels.empty() || clamped_strength <= 0.0) {
        return;
    }

    std::vector<Uint8> original = layer.pixels;
    constexpr std::array<int, 5> weights = {4, 1, 1, 1, 1};

    for (int y = 1; y + 1 < height; ++y) {
        for (int x = 1; x + 1 < width; ++x) {
            const int index = y * width + x;
            const std::array<Rgba, 5> samples = {
                read_pixel(original, index),         read_pixel(original, index - width),
                read_pixel(original, index + width), read_pixel(original, index - 1),
                read_pixel(original, index + 1),
            };

            int alpha_sum = 0;
            int red_sum = 0;
            int green_sum = 0;
            int blue_sum = 0;
            int weighted_alpha_sum = 0;
            int weight_sum = 0;

            for (std::size_t sample_index = 0; sample_index < samples.size(); ++sample_index) {
                const Rgba sample = samples[sample_index];
                const int weight = weights[sample_index];
                const int alpha = static_cast<int>(sample.a);

                alpha_sum += alpha * weight;
                red_sum += static_cast<int>(sample.r) * alpha * weight;
                green_sum += static_cast<int>(sample.g) * alpha * weight;
                blue_sum += static_cast<int>(sample.b) * alpha * weight;
                weighted_alpha_sum += alpha * weight;
                weight_sum += weight;
            }

            if (weighted_alpha_sum == 0 || weight_sum == 0) {
                continue;
            }

            const Rgba blurred{
                .r = static_cast<Uint8>(std::clamp(red_sum / weighted_alpha_sum, 0, 255)),
                .g = static_cast<Uint8>(std::clamp(green_sum / weighted_alpha_sum, 0, 255)),
                .b = static_cast<Uint8>(std::clamp(blue_sum / weighted_alpha_sum, 0, 255)),
                .a = static_cast<Uint8>(std::clamp(alpha_sum / weight_sum, 0, 255)),
            };
            const Rgba center = read_pixel(original, index);
            write_pixel(layer.pixels, index,
                        Rgba{
                            .r = mix_channel(center.r, blurred.r, clamped_strength),
                            .g = mix_channel(center.g, blurred.g, clamped_strength),
                            .b = mix_channel(center.b, blurred.b, clamped_strength),
                            .a = mix_channel(center.a, blurred.a, clamped_strength),
                        });
        }
    }
}

void compose_underpainting(RenderScratch& scratch, int width, int height) {
    scratch.composed_pixels.assign(pixel_byte_count(width, height), 0U);

    for (int index = 0; index < width * height; ++index) {
        write_pixel(scratch.composed_pixels, index, palette.background);
    }

    for (const PaintLayer& layer : scratch.layers) {
        for (int index = 0; index < width * height; ++index) {
            const Rgba color = read_pixel(layer.pixels, index);
            if (color.a == 0) {
                continue;
            }

            const Rgba destination = read_pixel(scratch.composed_pixels, index);
            write_pixel(scratch.composed_pixels, index, alpha_blend(destination, color));
        }
    }
}

std::uint32_t hash_u32(std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

std::uint32_t polygon_hash(const Polygon& polygon) {
    std::uint32_t hash = 0x811c9dc5U ^ static_cast<std::uint32_t>(polygon.color_index);
    for (const Vec2 point : polygon.points) {
        const auto quantized_x = static_cast<std::int32_t>(std::round(point.x * 16.0));
        const auto quantized_y = static_cast<std::int32_t>(std::round(point.y * 16.0));
        hash ^= hash_u32(static_cast<std::uint32_t>(quantized_x));
        hash *= 0x01000193U;
        hash ^= hash_u32(static_cast<std::uint32_t>(quantized_y));
        hash *= 0x01000193U;
    }
    return hash_u32(hash);
}

double unit_from_hash(std::uint32_t hash) {
    constexpr double denominator = 16777215.0;
    return static_cast<double>(hash & 0x00ffffffU) / denominator;
}

Vec2 polygon_centroid(const Polygon& polygon) {
    Vec2 centroid{};
    if (polygon.points.empty()) {
        return centroid;
    }

    for (const Vec2 point : polygon.points) {
        centroid.x += point.x;
        centroid.y += point.y;
    }

    const double count = static_cast<double>(polygon.points.size());
    centroid.x /= count;
    centroid.y /= count;
    return centroid;
}

Vec2 polygon_tip_from_base(const Polygon& polygon) {
    if (polygon.points.empty()) {
        return Vec2{};
    }

    const Vec2 base = polygon.points.front();
    Vec2 tip = base;
    double max_distance = 0.0;
    for (const Vec2 point : polygon.points) {
        const double distance = squared_distance(base, point);
        if (distance > max_distance) {
            max_distance = distance;
            tip = point;
        }
    }
    return tip;
}

double cross(Vec2 left, Vec2 right) {
    return left.x * right.y - left.y * right.x;
}

Rgba polygon_vertex_color(const Polygon& polygon, Vec2 point, Rgba base_color,
                          const RenderSettings& settings) {
    Rgba color = base_color;
    const std::uint32_t hash = polygon_hash(polygon);

    if (settings.polygon_hue_variation) {
        const double centered = unit_from_hash(hash) * 2.0 - 1.0;
        const double hue_shift = centered * static_cast<double>(settings.polygon_hue_amount);
        const double saturation_scale =
            polygon.color_index == 2 ? 1.0 + static_cast<double>(settings.polygon_hue_amount) * 4.0
                                     : 1.0;
        const double value_scale = 0.96 + unit_from_hash(hash >> 8U) * 0.09;
        color = shift_hsv(color, hue_shift, saturation_scale, value_scale);
    }

    if (settings.leaf_two_tone && polygon.color_index == 1 && polygon.points.size() >= 3U) {
        const Vec2 base = polygon.points.front();
        const Vec2 tip = polygon_tip_from_base(polygon);
        const Vec2 axis{
            .x = tip.x - base.x,
            .y = tip.y - base.y,
        };
        const Vec2 relative{
            .x = point.x - polygon_centroid(polygon).x,
            .y = point.y - polygon_centroid(polygon).y,
        };
        const double side = std::clamp(cross(axis, relative) / 18.0, -1.0, 1.0);
        const double strength = static_cast<double>(settings.leaf_two_tone_strength);
        const double hue_shift = side * 0.035 * strength;
        const double value_scale = 1.0 + side * 0.28 * strength;
        const double saturation_scale = 1.0 - side * 0.12 * strength;
        color = shift_hsv(color, hue_shift, saturation_scale, value_scale);
    }

    return color;
}

void render_polygon(SDL_Renderer* renderer, const Polygon& polygon, const Bounds& bounds, Vec2 root,
                    double max_root_distance, const RenderSettings& settings, int width,
                    int height) {
    if (polygon.points.size() < 3U) {
        return;
    }

    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(polygon.points.size());
    indices.reserve((polygon.points.size() - 2U) * 3U);

    for (const Vec2 point : polygon.points) {
        const Vec2 transformed =
            transform_point(point, bounds, root, max_root_distance, settings, width, height);
        const Rgba fill = polygon_vertex_color(
            polygon, point, color_at(palette.fills, polygon.color_index), settings);
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
                                       const RenderSettings& settings, int width, int height,
                                       bool clear_background = true) {
    if (clear_background) {
        (void)SDL_SetRenderDrawColor(renderer, palette.background.r, palette.background.g,
                                     palette.background.b, palette.background.a);
        (void)SDL_RenderClear(renderer);
    }

    const Bounds bounds = compute_bounds(geometry);
    const Vec2 root = root_point(geometry);
    const double max_root_distance = max_distance_from_root(geometry, root);

    for (const LineSegment& line : geometry.lines) {
        const Rgba color = color_at(palette.lines, line.color_index);
        (void)SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        const Vec2 from =
            transform_point(line.from, bounds, root, max_root_distance, settings, width, height);
        const Vec2 to =
            transform_point(line.to, bounds, root, max_root_distance, settings, width, height);
        (void)SDL_RenderLine(renderer, static_cast<float>(from.x), static_cast<float>(from.y),
                             static_cast<float>(to.x), static_cast<float>(to.y));
    }

    for (const Polygon& polygon : geometry.polygons) {
        render_polygon(renderer, polygon, bounds, root, max_root_distance, settings, width, height);
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
    scratch.clean_pixels.clear();
    scratch.composed_pixels.clear();
    for (PaintLayer& layer : scratch.layers) {
        layer.pixels.clear();
        layer.source_pixels.clear();
    }
    scratch.rng.seed(0x5eed1234U);
    scratch.target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                       width, height);
    scratch.post_processed = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                               SDL_TEXTUREACCESS_STREAMING, width, height);

    if (scratch.target == nullptr || scratch.post_processed == nullptr) {
        throw std::runtime_error(SDL_GetError());
    }

    (void)SDL_SetTextureScaleMode(scratch.target, SDL_SCALEMODE_NEAREST);
    (void)SDL_SetTextureScaleMode(scratch.post_processed, SDL_SCALEMODE_NEAREST);
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

    std::vector<Uint8> pixels(pixel_byte_count(width, height));
    const auto* source = static_cast<const Uint8*>(converted->pixels);

    for (int y = 0; y < height; ++y) {
        const std::size_t destination_offset = static_cast<std::size_t>(y * width * 4);
        const std::size_t source_offset = static_cast<std::size_t>(y * converted->pitch);
        std::copy(source + source_offset, source + source_offset + width * 4,
                  pixels.begin() + static_cast<std::ptrdiff_t>(destination_offset));
    }

    return pixels;
}

bool draw_settings_ui(RenderSettings& settings, const RenderModel& model) {
    bool changed = false;
    bool reset_requested = false;

    ImGui::SetNextWindowSize(ImVec2(320.0F, 0.0F), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("L-system")) {
        ImGui::Text("symbols: %zu", model.generated.size());
        ImGui::Text("segments: %zu", model.geometry.lines.size());
        ImGui::Text("polygons: %zu", model.geometry.polygons.size());
        ImGui::Separator();

        changed |= ImGui::SliderInt("canvas width", &settings.canvas_width, 120, 480);
        changed |= ImGui::SliderInt("canvas height", &settings.canvas_height, 80, 320);
        if (ImGui::Button("GBA 240x160")) {
            settings.canvas_width = default_canvas_width;
            settings.canvas_height = default_canvas_height;
            changed = true;
        }

        ImGui::Separator();
        changed |= ImGui::SliderInt("pulls/frame", &settings.paint_pulls_per_frame, 0, 2000);
        changed |= ImGui::SliderInt("pull distance", &settings.max_pull_length, 1, 24);
        changed |= ImGui::Checkbox("pull stems", &settings.pull_stems);
        changed |= ImGui::Checkbox("pull leaves", &settings.pull_leaves);
        changed |= ImGui::Checkbox("pull flowers", &settings.pull_flowers);
        changed |= ImGui::Checkbox("blur", &settings.blur_enabled);
        if (settings.blur_enabled) {
            changed |= ImGui::SliderInt("blur passes", &settings.blur_passes, 1, 4);
            changed |= ImGui::SliderFloat("blur strength", &settings.blur_strength, 0.0F, 1.0F);
        }

        ImGui::Separator();
        changed |= ImGui::Checkbox("wind", &settings.wind_enabled);
        if (settings.wind_enabled) {
            changed |= ImGui::SliderFloat("wind strength", &settings.wind_strength, 0.0F, 80.0F);
            changed |= ImGui::SliderFloat("wind direction", &settings.wind_direction_degrees,
                                          -180.0F, 180.0F, "%.0f deg");
        }

        ImGui::Separator();
        changed |= ImGui::Checkbox("polygon hue", &settings.polygon_hue_variation);
        if (settings.polygon_hue_variation) {
            changed |= ImGui::SliderFloat("hue amount", &settings.polygon_hue_amount, 0.0F, 0.12F);
        }
        changed |= ImGui::Checkbox("leaf two-tone", &settings.leaf_two_tone);
        if (settings.leaf_two_tone) {
            changed |=
                ImGui::SliderFloat("leaf contrast", &settings.leaf_two_tone_strength, 0.0F, 1.0F);
        }

        ImGui::Separator();
        reset_requested = ImGui::Button("restart paint");
    }
    ImGui::End();

    settings.canvas_width = std::clamp(settings.canvas_width, 1, 4096);
    settings.canvas_height = std::clamp(settings.canvas_height, 1, 4096);
    settings.paint_pulls_per_frame = std::clamp(settings.paint_pulls_per_frame, 0, 100000);
    settings.max_pull_length = std::clamp(settings.max_pull_length, 1, 512);
    settings.blur_passes = std::clamp(settings.blur_passes, 1, 16);
    settings.blur_strength = std::clamp(settings.blur_strength, 0.0F, 1.0F);
    settings.wind_strength = std::clamp(settings.wind_strength, 0.0F, 1000.0F);
    settings.wind_direction_degrees = std::clamp(settings.wind_direction_degrees, -360.0F, 360.0F);
    settings.polygon_hue_amount = std::clamp(settings.polygon_hue_amount, 0.0F, 1.0F);
    settings.leaf_two_tone_strength = std::clamp(settings.leaf_two_tone_strength, 0.0F, 1.0F);

    return changed || reset_requested;
}

void render_scene(SDL_Renderer* renderer, RenderScratch& scratch, const TurtleGeometry& geometry,
                  const RenderSettings& settings) {
    int output_width = initial_width;
    int output_height = initial_height;
    (void)SDL_GetRenderOutputSize(renderer, &output_width, &output_height);

    const int width = settings.canvas_width;
    const int height = settings.canvas_height;

    ensure_scratch_textures(renderer, scratch, width, height);

    if (scratch.dirty) {
        if (!SDL_SetRenderTarget(renderer, scratch.target)) {
            throw std::runtime_error(SDL_GetError());
        }

        render_geometry_to_current_target(renderer, geometry, settings, width, height);
        scratch.clean_pixels = read_rgba_pixels(renderer, width, height);
        reset_paint_layers(scratch, width, height);
        scratch.dirty = false;
    }

    const int stem_pulls = settings.paint_pulls_per_frame * 3 / 4;
    const int leaf_pulls = settings.paint_pulls_per_frame;
    const int flower_pulls = settings.paint_pulls_per_frame * 6 / 7;
    if (settings.pull_stems) {
        apply_vertical_paint_pulls(scratch.layers[0], PixelKind::stem, width, height, stem_pulls,
                                   settings.max_pull_length, scratch.rng);
    }
    if (settings.pull_leaves) {
        apply_vertical_paint_pulls(scratch.layers[1], PixelKind::leaf, width, height, leaf_pulls,
                                   settings.max_pull_length, scratch.rng);
    }
    if (settings.pull_flowers) {
        apply_vertical_paint_pulls(scratch.layers[2], PixelKind::flower, width, height,
                                   flower_pulls, settings.max_pull_length, scratch.rng);
    }

    if (settings.blur_enabled) {
        for (int pass = 0; pass < settings.blur_passes; ++pass) {
            blur_layer(scratch.layers[0], width, height, settings.blur_strength);
            blur_layer(scratch.layers[1], width, height, settings.blur_strength);
            blur_layer(scratch.layers[2], width, height, settings.blur_strength);
        }
    }
    compose_underpainting(scratch, width, height);

    if (!SDL_UpdateTexture(scratch.post_processed, nullptr, scratch.composed_pixels.data(),
                           width * 4)) {
        throw std::runtime_error(SDL_GetError());
    }

    if (!SDL_SetRenderTarget(renderer, scratch.target)) {
        throw std::runtime_error(SDL_GetError());
    }

    (void)SDL_RenderTexture(renderer, scratch.post_processed, nullptr, nullptr);
    render_geometry_to_current_target(renderer, geometry, settings, width, height, false);

    if (!SDL_SetRenderTarget(renderer, nullptr)) {
        throw std::runtime_error(SDL_GetError());
    }

    (void)SDL_SetRenderDrawColor(renderer, palette.background.r, palette.background.g,
                                 palette.background.b, palette.background.a);
    (void)SDL_RenderClear(renderer);

    const int output_scale = std::max(1, std::min(output_width / width, output_height / height));
    const float destination_width = static_cast<float>(width * output_scale);
    const float destination_height = static_cast<float>(height * output_scale);
    const SDL_FRect destination{
        .x = (static_cast<float>(output_width) - destination_width) * 0.5F,
        .y = (static_cast<float>(output_height) - destination_height) * 0.5F,
        .w = destination_width,
        .h = destination_height,
    };

    (void)SDL_RenderTexture(renderer, scratch.target, nullptr, &destination);
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
        UiLayer ui(display.window(), display.renderer());
        RenderSettings settings;
        RenderScratch scratch;
        update_window_title(display.window(), path, model);

        bool running = true;
        auto next_reload_check = std::chrono::steady_clock::now() + reload_interval;

        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ui.process_event(event);
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
                        reset_paint_simulation(scratch);
                    } catch (const std::exception& error) {
                        std::cerr << "reload failed: " << error.what() << '\n';
                    }
                }
            }

            ui.new_frame();
            if (draw_settings_ui(settings, model)) {
                reset_paint_simulation(scratch);
            }

            render_scene(display.renderer(), scratch, model.geometry, settings);
            ui.render(display.renderer());
            SDL_RenderPresent(display.renderer());
            SDL_Delay(16);
        }
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
