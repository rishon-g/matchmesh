#pragma once

// A tiny streaming JSON writer: enough for the snapshot document, with no
// dependency to compile to WebAssembly. Commas are inserted automatically.

#include <cmath>
#include <concepts>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace matchmesh::sim {

class Json {
public:
    Json& begin_object() { return open('{'); }
    Json& end_object() { return close('}'); }
    Json& begin_array() { return open('['); }
    Json& end_array() { return close(']'); }

    Json& key(std::string_view name) {
        separate();
        quote(name);
        out_ += ':';
        after_key_ = true;
        return *this;
    }

    Json& string(std::string_view value) {
        separate();
        quote(value);
        return *this;
    }

    Json& boolean(bool value) {
        separate();
        out_ += value ? "true" : "false";
        return *this;
    }

    template <std::integral Integer>
    Json& number(Integer value) {
        separate();
        out_ += std::to_string(value);
        return *this;
    }

    Json& number(double value) {
        separate();
        if (!std::isfinite(value)) {
            out_ += "null";
            return *this;
        }
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.6g", value);
        out_ += buffer;
        return *this;
    }

    [[nodiscard]] const std::string& str() const noexcept { return out_; }

private:
    Json& open(char bracket) {
        separate();
        out_ += bracket;
        first_.push_back(true);
        return *this;
    }

    Json& close(char bracket) {
        out_ += bracket;
        first_.pop_back();
        return *this;
    }

    // A comma before every value or key except the first in its container,
    // and none between a key and its value.
    void separate() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (!first_.empty()) {
            if (!first_.back()) {
                out_ += ',';
            }
            first_.back() = false;
        }
    }

    void quote(std::string_view text) {
        out_ += '"';
        for (const char c : text) {
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char escaped[8];
                        std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
                        out_ += escaped;
                    } else {
                        out_ += c;
                    }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> first_;
    bool after_key_ = false;
};

}  // namespace matchmesh::sim
