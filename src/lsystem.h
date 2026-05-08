#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct ProductionRule {
    double weight = 1.0;
    std::string replacement;
};

struct LSystemDefinition {
    std::string axiom;
    std::unordered_map<char, std::vector<ProductionRule>> rules;
    int iterations = 4;
    std::uint32_t seed = 1;
    double angle_degrees = 25.0;
    double step_length = 8.0;
    double start_angle_degrees = -90.0;
    double angle_jitter_degrees = 0.0;
    double step_jitter = 0.0;
};

struct LoadedLSystem {
    LSystemDefinition definition;
    std::filesystem::file_time_type modified_at;
};

LoadedLSystem load_lsystem_file(const std::filesystem::path& path);
std::string expand_lsystem(const LSystemDefinition& definition);
