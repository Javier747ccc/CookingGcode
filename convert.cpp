#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Ingredient {
    std::string name;
    std::string amount;
};

struct Config {
    std::vector<std::string> start;
    std::map<std::string, std::vector<std::string>> ingredients;
    std::vector<std::string> finish;
};

std::string trim(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string lower_copy(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

bool ends_with(const std::string& text, char c) {
    return !text.empty() && text.back() == c;
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string expand_command(std::string command, const Ingredient& ingredient) {
    replace_all(command, "{ingrediente}", ingredient.name);
    replace_all(command, "{cantidad}", ingredient.amount);
    return command;
}

bool load_config(const fs::path& path, Config& config) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "No se pudo abrir la configuracion: " << path.string() << '\n';
        return false;
    }

    enum class BlockKind { None, Start, Ingredient, Finish };
    BlockKind block = BlockKind::None;
    std::string current_ingredient;

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        const auto command = trim(line);
        if (command.empty() || command.rfind("#", 0) == 0) {
            continue;
        }

        if (ends_with(command, ':')) {
            const auto header = trim(command.substr(0, command.size() - 1));
            if (header == "inicio") {
                block = BlockKind::Start;
                current_ingredient.clear();
                continue;
            }
            if (header == "fin") {
                block = BlockKind::Finish;
                current_ingredient.clear();
                continue;
            }
            if (header.rfind("ingrediente ", 0) == 0) {
                current_ingredient = trim(header.substr(12));
                if (current_ingredient.empty()) {
                    std::cerr << path.string() << ':' << line_number << ": falta nombre de ingrediente\n";
                    return false;
                }
                block = BlockKind::Ingredient;
                config.ingredients[lower_copy(current_ingredient)] = {};
                continue;
            }
        }

        switch (block) {
        case BlockKind::Start:
            config.start.push_back(command);
            break;
        case BlockKind::Ingredient:
            config.ingredients[lower_copy(current_ingredient)].push_back(command);
            break;
        case BlockKind::Finish:
            config.finish.push_back(command);
            break;
        case BlockKind::None:
            std::cerr << path.string() << ':' << line_number << ": comando fuera de bloque: " << command << '\n';
            return false;
        }
    }

    return true;
}

bool load_jcu(const fs::path& path, std::vector<Ingredient>& ingredients) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "No se pudo abrir el fichero: " << path.string() << '\n';
        return false;
    }

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.rfind("#", 0) == 0) {
            continue;
        }

        const auto comma = line.find(',');
        if (comma == std::string::npos) {
            std::cerr << path.string() << ':' << line_number << ": uso: Ingrediente, cantidad\n";
            return false;
        }

        auto name = trim(line.substr(0, comma));
        auto amount = trim(line.substr(comma + 1));
        if (name.empty() || amount.empty()) {
            std::cerr << path.string() << ':' << line_number << ": ingrediente o cantidad vacios\n";
            return false;
        }

        ingredients.push_back({name, amount});
    }

    return true;
}

void emit_commands(const std::vector<std::string>& commands, const Ingredient& ingredient) {
    for (const auto& command : commands) {
        std::cout << expand_command(command, ingredient) << '\n';
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Uso: " << argv[0] << " fichero.jcu [configuracion]\n";
        return 2;
    }

    const fs::path jcu_path = argv[1];
    if (jcu_path.extension() != ".jcu") {
        std::cerr << "El fichero debe tener extension .jcu: " << jcu_path.string() << '\n';
        return 2;
    }

    const fs::path config_path = argc == 3 ? fs::path(argv[2]) : fs::path("cooking-actions.cfg");

    Config config;
    std::vector<Ingredient> ingredients;
    if (!load_config(config_path, config) || !load_jcu(jcu_path, ingredients)) {
        return 1;
    }

    std::cout << "# Generado desde " << jcu_path.string() << " usando " << config_path.string() << '\n';
    const Ingredient empty_ingredient{"", ""};
    emit_commands(config.start, empty_ingredient);

    for (const auto& ingredient : ingredients) {
        const auto action = config.ingredients.find(lower_copy(ingredient.name));
        if (action == config.ingredients.end()) {
            std::cerr << "No hay accion configurada para el ingrediente: " << ingredient.name << '\n';
            return 1;
        }

        std::cout << "# " << ingredient.name << ", " << ingredient.amount << '\n';
        emit_commands(action->second, ingredient);
    }

    emit_commands(config.finish, empty_ingredient);
    return 0;
}
