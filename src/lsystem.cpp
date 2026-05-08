#include "lsystem.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::size_t max_generated_symbols = 2'000'000;

std::string trim(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();

    while (begin != end && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
        ++begin;
    }

    while (begin != end) {
        const char previous = *(end - 1);
        if (std::isspace(static_cast<unsigned char>(previous)) == 0) {
            break;
        }
        --end;
    }

    return std::string(begin, end);
}

std::string strip_comment(std::string_view line) {
    const std::size_t comment = line.find('#');
    if (comment == std::string_view::npos) {
        return std::string(line);
    }
    return std::string(line.substr(0, comment));
}

int parse_int(std::string_view text, std::string_view field_name, int line_number) {
    const std::string value = trim(text);
    int parsed = 0;
    const char* const begin = value.data();
    const char* const end = value.data() + value.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);

    if (result.ec != std::errc() || result.ptr != end) {
        throw std::runtime_error("line " + std::to_string(line_number) + ": invalid integer for " +
                                 std::string(field_name));
    }

    return parsed;
}

double parse_double(std::string_view text, std::string_view field_name, int line_number) {
    const std::string value = trim(text);
    double parsed = 0.0;
    const char* const begin = value.data();
    const char* const end = value.data() + value.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);

    if (result.ec != std::errc() || result.ptr != end) {
        throw std::runtime_error("line " + std::to_string(line_number) + ": invalid number for " +
                                 std::string(field_name));
    }

    return parsed;
}

std::string remove_wrapping_parens(std::string value) {
    if (value.size() >= 2U && value.front() == '(' && value.back() == ')') {
        return value.substr(1U, value.size() - 2U);
    }

    return value;
}

ProductionRule parse_rule_head(std::string_view head, std::string_view replacement,
                               int line_number) {
    std::istringstream stream(trim(head));
    std::string symbol_text;
    std::string weight_text;

    stream >> symbol_text;
    stream >> weight_text;

    const std::size_t weight_start = symbol_text.find('(');
    if (weight_text.empty() && weight_start != std::string::npos && symbol_text.back() == ')') {
        weight_text = symbol_text.substr(weight_start);
        symbol_text = symbol_text.substr(0, weight_start);
    }

    if (symbol_text.size() != 1U) {
        throw std::runtime_error("line " + std::to_string(line_number) +
                                 ": rule symbol must be one character");
    }

    double weight = 1.0;
    if (!weight_text.empty()) {
        weight = parse_double(remove_wrapping_parens(weight_text), "rule weight", line_number);
    }

    if (weight <= 0.0) {
        throw std::runtime_error("line " + std::to_string(line_number) +
                                 ": rule weight must be greater than zero");
    }

    return ProductionRule{
        .weight = weight,
        .replacement = trim(replacement),
    };
}

void parse_rule_line(LSystemDefinition& definition, std::string_view body, int line_number) {
    std::size_t separator = body.find(':');
    std::size_t arrow_size = 1U;

    if (separator == std::string_view::npos) {
        separator = body.find("->");
        arrow_size = 2U;
    }

    if (separator == std::string_view::npos) {
        throw std::runtime_error("line " + std::to_string(line_number) +
                                 ": rule needs ':' or '->'");
    }

    const std::string symbol_text = trim(body.substr(0, separator));
    ProductionRule rule =
        parse_rule_head(symbol_text, body.substr(separator + arrow_size), line_number);

    definition.rules[symbol_text[0]].push_back(std::move(rule));
}

void parse_key_value(LSystemDefinition& definition, std::string_view line, int line_number) {
    const std::size_t separator = line.find(':');
    if (separator == std::string_view::npos) {
        throw std::runtime_error("line " + std::to_string(line_number) + ": expected key: value");
    }

    const std::string key = trim(line.substr(0, separator));
    const std::string value = trim(line.substr(separator + 1U));

    if (key == "axiom") {
        definition.axiom = value;
    } else if (key == "iterations") {
        definition.iterations = parse_int(value, key, line_number);
    } else if (key == "seed") {
        const int seed = parse_int(value, key, line_number);
        if (seed < 0) {
            throw std::runtime_error("line " + std::to_string(line_number) +
                                     ": seed must be zero or greater");
        }
        definition.seed = static_cast<std::uint32_t>(seed);
    } else if (key == "angle") {
        definition.angle_degrees = parse_double(value, key, line_number);
    } else if (key == "step") {
        definition.step_length = parse_double(value, key, line_number);
    } else if (key == "start_angle") {
        definition.start_angle_degrees = parse_double(value, key, line_number);
    } else if (key == "angle_jitter") {
        definition.angle_jitter_degrees = parse_double(value, key, line_number);
    } else if (key == "step_jitter") {
        definition.step_jitter = parse_double(value, key, line_number);
    } else if (key == "rule") {
        parse_rule_line(definition, value, line_number);
    } else {
        throw std::runtime_error("line " + std::to_string(line_number) + ": unknown key '" + key +
                                 "'");
    }
}

void validate_definition(const LSystemDefinition& definition) {
    if (definition.axiom.empty()) {
        throw std::runtime_error("lsystem file needs an axiom");
    }

    if (definition.iterations < 0) {
        throw std::runtime_error("iterations must be zero or greater");
    }

    if (definition.step_length <= 0.0) {
        throw std::runtime_error("step must be greater than zero");
    }

    if (definition.angle_jitter_degrees < 0.0) {
        throw std::runtime_error("angle_jitter must be zero or greater");
    }

    if (definition.step_jitter < 0.0 || definition.step_jitter >= 1.0) {
        throw std::runtime_error("step_jitter must be at least zero and less than one");
    }

    for (const auto& [symbol, rules] : definition.rules) {
        if (rules.empty()) {
            throw std::runtime_error(std::string("symbol '") + symbol + "' has no productions");
        }
    }
}

} // namespace

LoadedLSystem load_lsystem_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("could not open " + path.string());
    }

    LSystemDefinition definition;
    std::string line;
    int line_number = 0;

    while (std::getline(file, line)) {
        ++line_number;
        const std::string without_comment = strip_comment(line);
        const std::string cleaned = trim(without_comment);

        if (cleaned.empty()) {
            continue;
        }

        if (cleaned.starts_with("rule ")) {
            parse_rule_line(definition, std::string_view(cleaned).substr(5U), line_number);
        } else {
            parse_key_value(definition, cleaned, line_number);
        }
    }

    validate_definition(definition);

    return LoadedLSystem{
        .definition = definition,
        .modified_at = std::filesystem::last_write_time(path),
    };
}

std::string expand_lsystem(const LSystemDefinition& definition) {
    std::string current = definition.axiom;
    std::mt19937 rng(definition.seed);

    for (int iteration = 0; iteration < definition.iterations; ++iteration) {
        std::string next;
        next.reserve(std::min(current.size() * 2U, max_generated_symbols));

        for (const char symbol : current) {
            const auto rule = definition.rules.find(symbol);
            if (rule == definition.rules.end()) {
                next.push_back(symbol);
            } else if (rule->second.size() == 1U) {
                next += rule->second.front().replacement;
            } else {
                std::vector<double> weights;
                weights.reserve(rule->second.size());
                for (const ProductionRule& production : rule->second) {
                    weights.push_back(production.weight);
                }

                std::discrete_distribution<std::size_t> distribution(weights.begin(),
                                                                     weights.end());
                next += rule->second[distribution(rng)].replacement;
            }

            if (next.size() > max_generated_symbols) {
                throw std::runtime_error("generated string exceeded " +
                                         std::to_string(max_generated_symbols) + " symbols");
            }
        }

        current = std::move(next);
    }

    return current;
}
