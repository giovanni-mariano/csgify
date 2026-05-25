#pragma once

// Minimal recursive-descent JSON parser, scoped to what cad2csg's
// decision-log writer and recipe compactor emit (objects, arrays,
// strings, numbers, bools, nulls). Header-only, no dependency.

#include <cctype>
#include <cstddef>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cad2csg {

struct JsonValue;
using JsonObject = std::map<std::string, std::shared_ptr<JsonValue>>;
using JsonArray = std::vector<std::shared_ptr<JsonValue>>;

enum class JsonKind { null, boolean, number, string, array, object };

struct JsonValue {
    JsonKind kind = JsonKind::null;
    bool b = false;
    double num = 0.0;
    std::string str;
    JsonArray arr;
    JsonObject obj;

    const JsonValue* get(const std::string& key) const {
        if (kind != JsonKind::object) return nullptr;
        auto it = obj.find(key);
        if (it == obj.end()) return nullptr;
        return it->second.get();
    }
    std::string as_string(const std::string& fallback = {}) const {
        return kind == JsonKind::string ? str : fallback;
    }
    double as_number(double fallback = 0.0) const {
        return kind == JsonKind::number ? num : fallback;
    }
    bool as_bool(bool fallback = false) const {
        return kind == JsonKind::boolean ? b : fallback;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src_(src) {}

    std::shared_ptr<JsonValue> parse() {
        skip_ws();
        auto v = parse_value();
        skip_ws();
        if (pos_ != src_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) {
        std::ostringstream out;
        out << "json parse error at " << pos_ << ": " << msg;
        throw std::runtime_error(out.str());
    }
    void skip_ws() {
        while (pos_ < src_.size() &&
               (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                src_[pos_] == '\n' || src_[pos_] == '\r')) {
            ++pos_;
        }
    }
    char peek() {
        if (pos_ >= src_.size()) fail("unexpected end of input");
        return src_[pos_];
    }
    void expect(char ch) {
        if (peek() != ch) fail(std::string("expected '") + ch + "'");
        ++pos_;
    }

    std::shared_ptr<JsonValue> parse_value() {
        skip_ws();
        const char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string_value();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        fail("unexpected character");
    }
    std::shared_ptr<JsonValue> parse_object() {
        auto v = std::make_shared<JsonValue>();
        v->kind = JsonKind::object;
        expect('{');
        skip_ws();
        if (peek() == '}') { ++pos_; return v; }
        while (true) {
            skip_ws();
            const std::string key = parse_string_raw();
            skip_ws();
            expect(':');
            v->obj[key] = parse_value();
            skip_ws();
            const char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; return v; }
            fail("expected ',' or '}'");
        }
    }
    std::shared_ptr<JsonValue> parse_array() {
        auto v = std::make_shared<JsonValue>();
        v->kind = JsonKind::array;
        expect('[');
        skip_ws();
        if (peek() == ']') { ++pos_; return v; }
        while (true) {
            v->arr.push_back(parse_value());
            skip_ws();
            const char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == ']') { ++pos_; return v; }
            fail("expected ',' or ']'");
        }
    }
    std::string parse_string_raw() {
        expect('"');
        std::string out;
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= src_.size()) fail("unterminated escape");
                const char e = src_[pos_++];
                switch (e) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u':
                    if (pos_ + 4 > src_.size()) fail("unterminated \\u escape");
                    out += '?';
                    pos_ += 4;
                    break;
                default: fail("unknown escape");
                }
            } else {
                out += c;
            }
        }
        fail("unterminated string");
    }
    std::shared_ptr<JsonValue> parse_string_value() {
        auto v = std::make_shared<JsonValue>();
        v->kind = JsonKind::string;
        v->str = parse_string_raw();
        return v;
    }
    std::shared_ptr<JsonValue> parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < src_.size() &&
               (std::isdigit(static_cast<unsigned char>(src_[pos_])) ||
                src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E' ||
                src_[pos_] == '+' || src_[pos_] == '-')) {
            ++pos_;
        }
        auto v = std::make_shared<JsonValue>();
        v->kind = JsonKind::number;
        try {
            v->num = std::stod(src_.substr(start, pos_ - start));
        } catch (const std::exception&) {
            fail("invalid number");
        }
        return v;
    }
    std::shared_ptr<JsonValue> parse_bool() {
        if (src_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            auto v = std::make_shared<JsonValue>();
            v->kind = JsonKind::boolean;
            v->b = true;
            return v;
        }
        if (src_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            auto v = std::make_shared<JsonValue>();
            v->kind = JsonKind::boolean;
            v->b = false;
            return v;
        }
        fail("invalid literal");
    }
    std::shared_ptr<JsonValue> parse_null() {
        if (src_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return std::make_shared<JsonValue>();
        }
        fail("invalid literal");
    }

    const std::string& src_;
    std::size_t pos_ = 0;
};

inline std::shared_ptr<JsonValue> load_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("could not open " + path);
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return JsonParser(buf.str()).parse();
}

} // namespace cad2csg
