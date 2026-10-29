#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace chuan::update {
inline constexpr char Repository[] = "DS-BigMountain/WLAN-chuan";
inline constexpr char ReleaseApi[] = "https://api.github.com/repos/DS-BigMountain/WLAN-chuan/releases/latest";
inline constexpr wchar_t ReleasePage[] = L"https://github.com/DS-BigMountain/WLAN-chuan/releases/latest";
inline std::array<unsigned, 3> ParseVersion(std::string value) {
    if (!value.empty() && value.front() == 'V') value.erase(0, 1);
    std::array<unsigned, 3> result{};
    for (size_t i = 0; i < 3; ++i) {
        const auto end = value.find('.');
        const auto part = value.substr(0, end);
        if (part.empty() || part.size() > 5 || part.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("版本号格式无效");
        auto parsed = std::from_chars(part.data(), part.data() + part.size(), result[i]);
        if (parsed.ec != std::errc{} || result[i] > 65535 || (i < 2) != (end != std::string::npos))
            throw std::runtime_error("版本号格式无效");
        if (end != std::string::npos) value.erase(0, end + 1);
    }
    return result;
}
struct JsonValue {
    enum Kind { Null, String, Number, Boolean, Object, Array } kind = Null;
    std::string text;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
    const JsonValue &At(const std::string &key) const {
        static const JsonValue empty;
        auto it = object.find(key); return it == object.end() ? empty : it->second;
    }
    std::string StringValue() const { return kind == String ? text : ""; }
    uint64_t Integer() const {
        uint64_t n = 0;
        auto r = std::from_chars(text.data(), text.data() + text.size(), n);
        if (kind != Number || r.ec != std::errc{} || r.ptr != text.data() + text.size())
            throw std::runtime_error("更新包大小无效");
        return n;
    }
    bool True() const { return kind == Boolean && text == "true"; }
};
class JsonReader {
    const std::string &source; size_t at = 0;
    [[noreturn]] void Bad() const { throw std::runtime_error("GitHub 更新信息格式无效"); }
    void Space() { while (at < source.size() && (source[at] == ' ' || source[at] == '\r' || source[at] == '\n' || source[at] == '\t')) ++at; }
    char Take() { if (at == source.size()) Bad(); return source[at++]; }
    unsigned Hex4() {
        unsigned n = 0;
        for (int i = 0; i < 4; ++i) { char c = Take(); unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16; if (d == 16) Bad(); n = n * 16 + d; }
        return n;
    }
    std::string Quoted() {
        std::string out;
        for (;;) {
            unsigned char c = static_cast<unsigned char>(Take());
            if (c == '"') return out;
            if (c < 32) Bad();
            if (c != '\\') { out += static_cast<char>(c); continue; }
            switch (Take()) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = Hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) { if (Take() != '\\' || Take() != 'u') Bad(); unsigned low = Hex4(); if (low < 0xDC00 || low > 0xDFFF) Bad(); cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00; }
                else if (cp >= 0xDC00 && cp <= 0xDFFF) Bad();
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63)); }
                else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
                else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
                break;
            }
            default: Bad();
            }
        }
    }
    JsonValue Value(unsigned depth) {
        if (depth > 32) Bad(); Space(); char c = Take(); JsonValue v;
        if (c == '"') { v.kind = JsonValue::String; v.text = Quoted(); return v; }
        if (c == '{') {
            v.kind = JsonValue::Object; Space(); if (at < source.size() && source[at] == '}') { ++at; return v; }
            for (;;) { Space(); if (Take() != '"') Bad(); auto key = Quoted(); Space(); if (Take() != ':') Bad(); if (!v.object.emplace(key, Value(depth + 1)).second) Bad(); Space(); c = Take(); if (c == '}') return v; if (c != ',') Bad(); }
        }
        if (c == '[') {
            v.kind = JsonValue::Array; Space(); if (at < source.size() && source[at] == ']') { ++at; return v; }
            for (;;) { v.array.push_back(Value(depth + 1)); Space(); c = Take(); if (c == ']') return v; if (c != ',') Bad(); }
        }
        --at;
        for (auto literal : {"true", "false", "null"}) { std::string s = literal; if (source.compare(at, s.size(), s) == 0) { at += s.size(); v.kind = s == "null" ? JsonValue::Null : JsonValue::Boolean; v.text = s; return v; } }
        const auto begin = at;
        if (at < source.size() && source[at] == '-') ++at;
        if (at == source.size() || source[at] < '0' || source[at] > '9') Bad();
        if (source[at] == '0') ++at; else while (at < source.size() && source[at] >= '0' && source[at] <= '9') ++at;
        if (at < source.size() && source[at] == '.') { ++at; auto first = at; while (at < source.size() && source[at] >= '0' && source[at] <= '9') ++at; if (first == at) Bad(); }
        if (at < source.size() && (source[at] == 'e' || source[at] == 'E')) { ++at; if (at < source.size() && (source[at] == '+' || source[at] == '-')) ++at; auto first = at; while (at < source.size() && source[at] >= '0' && source[at] <= '9') ++at; if (first == at) Bad(); }
        v.kind = JsonValue::Number; v.text = source.substr(begin, at - begin); return v;
    }
  public:
    explicit JsonReader(const std::string &s) : source(s) {}
    JsonValue Read() { if (source.size() > 1048576) Bad(); auto v = Value(0); Space(); if (at != source.size()) Bad(); return v; }
};
struct Release { std::string version, notes, url, digest, asset; uint64_t size = 0; };
inline Release ParseRelease(const std::string &body, const std::string &platform) {
    const auto root = JsonReader(body).Read();
    if (root.kind != JsonValue::Object || root.At("draft").True() || root.At("prerelease").True()) throw std::runtime_error("未找到正式发行版本");
    Release r; r.version = root.At("tag_name").StringValue();
    if (r.version.empty() || r.version.front() != 'V') throw std::runtime_error("发行版本须采用 Vx.x.x 格式");
    ParseVersion(r.version); r.notes = root.At("body").StringValue();
    r.asset = "Chuan-" + r.version.substr(1) + "-" + platform;
    for (const auto &asset : root.At("assets").array) {
        if (asset.At("name").StringValue() != r.asset) continue;
        if (!r.url.empty()) throw std::runtime_error("更新资源名称重复");
        r.url = asset.At("browser_download_url").StringValue(); r.digest = asset.At("digest").StringValue(); r.size = asset.At("size").Integer();
    }
    if (r.url != "https://github.com/" + std::string(Repository) + "/releases/download/" + r.version + "/" + r.asset) throw std::runtime_error("此版本缺少对应平台的更新包，或下载地址无效");
    if (r.size == 0 || r.size > 512ull * 1024 * 1024) throw std::runtime_error("更新包大小超出允许范围");
    if (!r.digest.starts_with("sha256:") || r.digest.size() != 71 || r.digest.substr(7).find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) throw std::runtime_error("更新包缺少有效的 SHA-256 摘要，请从发布页下载");
    r.digest.erase(0, 7);
    for (auto &c : r.digest) if (c >= 'A' && c <= 'F') c += 'a' - 'A';
    return r;
}
} // namespace chuan::update
