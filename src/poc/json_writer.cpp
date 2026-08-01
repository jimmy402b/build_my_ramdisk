#include "json_writer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <iomanip>
#include <sstream>

namespace myrd::poc {

void JsonWriter::start_object() {
    emit_comma();
    raw("{");
    depth_++;
    need_comma_ = false;
}
void JsonWriter::end_object() {
    depth_--;
    raw("\n");
    emit_indent();
    raw("}");
    need_comma_ = true;
}
void JsonWriter::start_array() {
    emit_comma();
    raw("[");
    depth_++;
    need_comma_ = false;
}
void JsonWriter::end_array() {
    depth_--;
    raw("\n");
    emit_indent();
    raw("]");
    need_comma_ = true;
}
void JsonWriter::key(std::string_view k) {
    emit_comma();
    raw("\n");
    emit_indent();
    raw("\"");
    raw(escape(k));
    raw("\"");
    raw(": ");
    need_comma_ = false;
}
void JsonWriter::string_value(std::string_view v) {
    emit_comma();
    raw("\"");
    raw(escape(v));
    raw("\"");
    need_comma_ = true;
}
void JsonWriter::wstring_value(std::wstring_view v) {
    emit_comma();
    raw("\"");
    raw(escape_w(v));
    raw("\"");
    need_comma_ = true;
}
void JsonWriter::boolean_value(bool v) {
    emit_comma();
    raw(v ? "true" : "false");
    need_comma_ = true;
}
void JsonWriter::int_value(std::int64_t v) {
    emit_comma();
    buf_ += std::to_string(v);
    need_comma_ = true;
}
void JsonWriter::uint_value(std::uint64_t v) {
    emit_comma();
    buf_ += std::to_string(v);
    need_comma_ = true;
}
void JsonWriter::null_value() {
    emit_comma();
    raw("null");
    need_comma_ = true;
}
std::string JsonWriter::finish() {
    raw("\n");
    return std::move(buf_);
}

void JsonWriter::emit_indent() {
    for (int i = 0; i < depth_; ++i) raw("  ");
}
void JsonWriter::raw(std::string_view s) { buf_ += s; }
void JsonWriter::emit_comma() {
    if (need_comma_) raw(",\n");
}

std::string JsonWriter::escape(std::string_view s) const {
    std::ostringstream oss;
    for (auto ch : s) {
        switch (ch) {
        case '"': oss << "\\\""; break;
        case '\\': oss << "\\\\"; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(static_cast<unsigned char>(ch));
            else oss << ch;
        }
    }
    return oss.str();
}

std::string JsonWriter::escape_w(std::wstring_view ws) const {
    int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        ws.data(), static_cast<int>(ws.size()), utf8.data(), len,
        nullptr, nullptr);
    return escape(utf8);
}

} // namespace myrd::poc
