#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::poc {

class JsonWriter {
public:
    void start_object();
    void end_object();
    void start_array();
    void end_array();
    void key(std::string_view k);
    void string_value(std::string_view v);
    void wstring_value(std::wstring_view v);
    void boolean_value(bool v);
    void int_value(std::int64_t v);
    void uint_value(std::uint64_t v);
    void null_value();
    std::string finish();

private:
    void emit_indent();
    void raw(std::string_view s);
    void emit_comma();
    std::string escape(std::string_view s) const;
    std::string escape_w(std::wstring_view ws) const;

    std::string buf_;
    int depth_ = 0;
    bool need_comma_ = false;
};

} // namespace myrd::poc
