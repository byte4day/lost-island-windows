#include "json.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

static const Value kNull;
static const std::string kEmpty;

Value::Value(const std::wstring& s) : type_(Type::String), s_(ToUtf8(s)) {}

const std::string& Value::asString() const { return type_ == Type::String ? s_ : kEmpty; }
std::wstring Value::asWString(const std::wstring& def) const {
    return type_ == Type::String ? FromUtf8(s_) : def;
}

size_t Value::size() const {
    if (type_ == Type::Array) return a_.size();
    if (type_ == Type::Object) return o_.size();
    return 0;
}
const Value& Value::operator[](size_t i) const {
    return (type_ == Type::Array && i < a_.size()) ? a_[i] : kNull;
}
void Value::push(Value v) {
    if (type_ != Type::Array) { *this = Array(); }
    a_.push_back(std::move(v));
}
void Value::erase(size_t i) {
    if (type_ == Type::Array && i < a_.size()) a_.erase(a_.begin() + (ptrdiff_t)i);
}
bool Value::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (auto& kv : o_) if (kv.first == key) return true;
    return false;
}
const Value& Value::operator[](const std::string& key) const {
    if (type_ == Type::Object)
        for (auto& kv : o_) if (kv.first == key) return kv.second;
    return kNull;
}
Value& Value::set(const std::string& key, Value v) {
    if (type_ != Type::Object) *this = Object();
    for (auto& kv : o_) if (kv.first == key) { kv.second = std::move(v); return kv.second; }
    o_.emplace_back(key, std::move(v));
    return o_.back().second;
}
Value& Value::get(const std::string& key) {
    if (type_ != Type::Object) *this = Object();
    for (auto& kv : o_) if (kv.first == key) return kv.second;
    o_.emplace_back(key, Value());
    return o_.back().second;
}
void Value::remove(const std::string& key) {
    for (size_t i = 0; i < o_.size(); i++)
        if (o_[i].first == key) { o_.erase(o_.begin() + (ptrdiff_t)i); return; }
}

static void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
            else out += (char)c;
        }
    }
    out += '"';
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append((size_t)(indent * d), ' ');
    };
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: {
        char buf[40];
        if (std::isfinite(n_) && n_ == std::floor(n_) && std::fabs(n_) < 1e15)
            snprintf(buf, sizeof buf, "%lld", (long long)n_);
        else
            snprintf(buf, sizeof buf, "%.17g", std::isfinite(n_) ? n_ : 0.0);
        out += buf;
        break;
    }
    case Type::String: escape(out, s_); break;
    case Type::Array:
        if (a_.empty()) { out += "[]"; break; }
        out += '[';
        for (size_t i = 0; i < a_.size(); i++) {
            if (i) out += ',';
            nl(depth + 1);
            a_[i].dumpTo(out, indent, depth + 1);
        }
        nl(depth);
        out += ']';
        break;
    case Type::Object:
        if (o_.empty()) { out += "{}"; break; }
        out += '{';
        for (size_t i = 0; i < o_.size(); i++) {
            if (i) out += ',';
            nl(depth + 1);
            escape(out, o_[i].first);
            out += indent > 0 ? ": " : ":";
            o_[i].second.dumpTo(out, indent, depth + 1);
        }
        nl(depth);
        out += '}';
        break;
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    if (indent > 0) out += '\n';
    return out;
}

// ------------------------------------------------------------ parser
namespace {
struct Parser {
    const char* p;
    const char* end;
    int depth = 0;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool lit(const char* s) {
        size_t n = strlen(s);
        if ((size_t)(end - p) < n || memcmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { out += c; continue; }
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Value& out) {
        if (++depth > 200) return false;
        ws();
        if (p >= end) return false;
        bool ok = true;
        char c = *p;
        if (c == '{') {
            p++;
            out = Value::Object();
            ws();
            if (p < end && *p == '}') { p++; }
            else {
                for (;;) {
                    ws();
                    std::string key;
                    if (!str(key)) { ok = false; break; }
                    ws();
                    if (p >= end || *p != ':') { ok = false; break; }
                    p++;
                    Value v;
                    if (!value(v)) { ok = false; break; }
                    out.set(key, std::move(v));
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == '}') { p++; break; }
                    ok = false;
                    break;
                }
            }
        } else if (c == '[') {
            p++;
            out = Value::Array();
            ws();
            if (p < end && *p == ']') { p++; }
            else {
                for (;;) {
                    Value v;
                    if (!value(v)) { ok = false; break; }
                    out.push(std::move(v));
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == ']') { p++; break; }
                    ok = false;
                    break;
                }
            }
        } else if (c == '"') {
            std::string s;
            ok = str(s);
            out = Value(std::move(s));
        } else if (lit("true")) out = Value(true);
        else if (lit("false")) out = Value(false);
        else if (lit("null")) out = Value();
        else {
            char* stop = nullptr;
            std::string num;
            while (p < end && (strchr("+-0123456789.eE", *p) != nullptr)) num += *p++;
            if (num.empty()) return false;
            double d = strtod(num.c_str(), &stop);
            if (!stop || *stop) return false;
            out = Value(d);
        }
        depth--;
        return ok;
    }
};
}  // namespace

bool Parse(const std::string& text, Value& out) {
    Parser ps{text.data(), text.data() + text.size()};
    // tolerate a UTF-8 BOM (Notepad writes one)
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    Value v;
    if (!ps.value(v)) { out = Value(); return false; }
    ps.ws();
    if (ps.p != ps.end) { out = Value(); return false; }
    out = std::move(v);
    return true;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool ReadFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > 16 * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize((size_t)size.QuadPart);
    DWORD got = 0;
    BOOL ok = out.empty() || ::ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = ::WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size();
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace json
