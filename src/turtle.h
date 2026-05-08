#pragma once

#include "lsystem.h"

#include <string>
#include <vector>

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct LineSegment {
    Vec2 from;
    Vec2 to;
    int color_index = 0;
};

struct Polygon {
    std::vector<Vec2> points;
    int color_index = 0;
};

struct TurtleGeometry {
    std::vector<LineSegment> lines;
    std::vector<Polygon> polygons;
};

TurtleGeometry build_turtle_geometry(const std::string& commands,
                                     const LSystemDefinition& definition);
