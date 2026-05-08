#include "turtle.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr int max_color_index = 2;

struct TurtleState {
    Vec2 position;
    double angle_degrees = 0.0;
    double step_scale = 1.0;
    int color_index = 0;
};

double to_radians(double degrees) {
    return degrees * pi / 180.0;
}

Vec2 step_forward(Vec2 position, double angle_degrees, double distance) {
    const double angle = to_radians(angle_degrees);
    return Vec2{
        .x = position.x + std::cos(angle) * distance,
        .y = position.y + std::sin(angle) * distance,
    };
}

bool same_point(Vec2 left, Vec2 right) {
    return left.x == right.x && left.y == right.y;
}

void add_polygon_point_if_active(Polygon& polygon, bool active, Vec2 point) {
    if (active && (polygon.points.empty() || !same_point(polygon.points.back(), point))) {
        polygon.points.push_back(point);
    }
}

} // namespace

TurtleGeometry build_turtle_geometry(const std::string& commands,
                                     const LSystemDefinition& definition) {
    TurtleState turtle{
        .position = Vec2{},
        .angle_degrees = definition.start_angle_degrees,
    };
    std::vector<TurtleState> stack;
    TurtleGeometry geometry;
    Polygon active_polygon;
    bool polygon_active = false;
    std::mt19937 rng(definition.seed ^ 0x9E3779B9U);

    for (const char command : commands) {
        switch (command) {
        case 'F':
        case 'G': {
            const Vec2 next = step_forward(turtle.position, turtle.angle_degrees,
                                           definition.step_length * turtle.step_scale);
            geometry.lines.push_back(LineSegment{
                .from = turtle.position,
                .to = next,
                .color_index = turtle.color_index,
            });
            turtle.position = next;
            add_polygon_point_if_active(active_polygon, polygon_active, turtle.position);
            break;
        }
        case 'f':
            turtle.position = step_forward(turtle.position, turtle.angle_degrees,
                                           definition.step_length * turtle.step_scale);
            add_polygon_point_if_active(active_polygon, polygon_active, turtle.position);
            break;
        case '+':
            turtle.angle_degrees += definition.angle_degrees;
            break;
        case '-':
            turtle.angle_degrees -= definition.angle_degrees;
            break;
        case '&':
            turtle.angle_degrees += definition.angle_degrees;
            break;
        case '^':
            turtle.angle_degrees -= definition.angle_degrees;
            break;
        case '/':
            turtle.angle_degrees += definition.angle_degrees;
            break;
        case '\\':
            turtle.angle_degrees -= definition.angle_degrees;
            break;
        case '|':
            turtle.angle_degrees += 180.0;
            break;
        case '~':
            if (definition.angle_jitter_degrees > 0.0) {
                std::uniform_real_distribution<double> distribution(
                    -definition.angle_jitter_degrees, definition.angle_jitter_degrees);
                turtle.angle_degrees += distribution(rng);
            }
            break;
        case ';':
            if (definition.step_jitter > 0.0) {
                std::uniform_real_distribution<double> distribution(1.0 - definition.step_jitter,
                                                                    1.0 + definition.step_jitter);
                turtle.step_scale *= distribution(rng);
            }
            break;
        case '\'':
            turtle.color_index = std::min(turtle.color_index + 1, max_color_index);
            break;
        case '`':
            turtle.color_index = std::max(turtle.color_index - 1, 0);
            break;
        case '[':
            stack.push_back(turtle);
            break;
        case ']':
            if (!stack.empty()) {
                turtle = stack.back();
                stack.pop_back();
            }
            break;
        case '{':
            active_polygon.points.clear();
            active_polygon.color_index = turtle.color_index;
            polygon_active = true;
            add_polygon_point_if_active(active_polygon, polygon_active, turtle.position);
            break;
        case '.':
            add_polygon_point_if_active(active_polygon, polygon_active, turtle.position);
            break;
        case '}':
            if (polygon_active) {
                add_polygon_point_if_active(active_polygon, polygon_active, turtle.position);
                if (active_polygon.points.size() >= 3U) {
                    geometry.polygons.push_back(active_polygon);
                }
                active_polygon.points.clear();
                polygon_active = false;
            }
            break;
        default:
            break;
        }
    }

    return geometry;
}
