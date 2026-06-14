#include "json_util.h"

#include <cctype>
#include <cstdlib>

static std::optional<std::size_t> find_key_value(const std::string& json, const std::string& key) {
    const std::string quoted = "\"" + key + "\"";
    std::size_t pos = json.find(quoted);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + quoted.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    return pos;
}

std::optional<std::string> json_string(const std::string& json, const std::string& key) {
    auto start_opt = find_key_value(json, key);
    if (!start_opt) return std::nullopt;
    std::size_t pos = *start_opt;
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;
    std::string out;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return out;
        if (c == '\\' && pos < json.size()) {
            char e = json[pos++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(e); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

std::optional<double> json_number(const std::string& json, const std::string& key) {
    auto start_opt = find_key_value(json, key);
    if (!start_opt) return std::nullopt;
    std::size_t pos = *start_opt;
    char* end = nullptr;
    double value = std::strtod(json.c_str() + pos, &end);
    if (end == json.c_str() + pos) return std::nullopt;
    return value;
}

std::string json_escape(const std::string& value) {
    std::string out;
    for (char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}
