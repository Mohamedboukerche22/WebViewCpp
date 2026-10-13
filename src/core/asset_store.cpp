#include "webviewcpp/asset_store.hpp"

#include "webviewcpp/log.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#if defined(WVC_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace wvc {
namespace {

constexpr std::array<char, 4> kMagic{'W', 'V', 'C', 'P'};
constexpr uint16_t kVersion = 1;
constexpr uint16_t kFlagCompressed = 0x0001;

// Little-endian scalar helpers. The pack format is explicitly LE so a bundle
// written on x86 is readable on ARM and vice-versa.
void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> deflate_bytes(const uint8_t* data, std::size_t size) {
#if defined(WVC_HAVE_ZLIB)
    uLongf bound = compressBound(static_cast<uLong>(size));
    std::vector<uint8_t> out(bound);
    if (compress2(out.data(), &bound, data, static_cast<uLong>(size), 9) != Z_OK)
        return {};
    out.resize(bound);
    return out;
#else
    (void)data;
    (void)size;
    return {};
#endif
}

std::vector<uint8_t> inflate_bytes(const uint8_t* data, std::size_t size, std::size_t raw_len) {
#if defined(WVC_HAVE_ZLIB)
    std::vector<uint8_t> out(raw_len);
    uLongf out_len = static_cast<uLongf>(raw_len);
    if (uncompress(out.data(), &out_len, data, static_cast<uLong>(size)) != Z_OK)
        throw std::runtime_error("wvcp: corrupt deflate stream");
    out.resize(out_len);
    return out;
#else
    (void)data; (void)size; (void)raw_len;
    throw std::runtime_error("wvcp: bundle is compressed but built without zlib");
#endif
}

bool has_file_extension(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash);
}

}  // namespace

std::string AssetStore::mime_for_extension(std::string_view ext) {
    std::string e(ext);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (e == "html" || e == "htm") return "text/html; charset=utf-8";
    if (e == "js" || e == "mjs" || e == "cjs") return "text/javascript; charset=utf-8";
    if (e == "css")   return "text/css; charset=utf-8";
    if (e == "json")  return "application/json; charset=utf-8";
    if (e == "map")   return "application/json; charset=utf-8";
    if (e == "svg")   return "image/svg+xml";
    if (e == "png")   return "image/png";
    if (e == "jpg" || e == "jpeg") return "image/jpeg";
    if (e == "gif")   return "image/gif";
    if (e == "webp")  return "image/webp";
    if (e == "avif")  return "image/avif";
    if (e == "ico")   return "image/x-icon";
    if (e == "woff")  return "font/woff";
    if (e == "woff2") return "font/woff2";
    if (e == "ttf")   return "font/ttf";
    if (e == "otf")   return "font/otf";
    if (e == "wasm")  return "application/wasm";
    if (e == "txt")   return "text/plain; charset=utf-8";
    if (e == "xml")   return "application/xml";
    if (e == "pdf")   return "application/pdf";
    if (e == "mp3")   return "audio/mpeg";
    if (e == "mp4")   return "video/mp4";
    if (e == "webm")  return "video/webm";
    if (e == "ogg")   return "audio/ogg";
    if (e == "wav")   return "audio/wav";
    return "application/octet-stream";
}

std::string AssetStore::normalize(std::string_view raw) {
    std::string path(raw);
    // Drop scheme + authority: app://local/foo -> /foo
    if (auto scheme = path.find("://"); scheme != std::string::npos) {
        auto start = path.find('/', scheme + 3);
        path = (start == std::string::npos) ? "/" : path.substr(start);
    }
    // Drop query and fragment.
    if (auto q = path.find_first_of("?#"); q != std::string::npos) path.resize(q);
    // Percent-decode the common cases we care about.
    std::string decoded;
    decoded.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(path[i + 1]), lo = hex(path[i + 2]);
            if (hi >= 0 && lo >= 0) { decoded.push_back(static_cast<char>((hi << 4) | lo)); i += 2; continue; }
        }
        decoded.push_back(path[i]);
    }
    // Strip leading slashes, collapse "..", and normalise separators.
    std::vector<std::string> parts;
    std::stringstream ss(decoded);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(seg);
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out.push_back('/');
        out += parts[i];
    }
    return out;
}

std::unique_ptr<AssetStore> AssetStore::from_directory(std::filesystem::path root) {
    auto store = std::make_unique<AssetStore>();
    store->disk_mode_ = true;
    store->root_ = std::move(root);
    store->source_ = store->root_.string();
    return store;
}

std::unique_ptr<AssetStore> AssetStore::from_file(const std::filesystem::path& pack) {
    std::ifstream in(pack, std::ios::binary);
    if (!in) throw std::runtime_error("wvcp: cannot open " + pack.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return from_memory(bytes.data(), bytes.size(), pack.string());
}

std::unique_ptr<AssetStore> AssetStore::from_memory(const uint8_t* data, std::size_t size,
                                                    std::string source_name) {
    auto store = std::make_unique<AssetStore>();
    store->source_ = std::move(source_name);
    if (size < 12 || std::memcmp(data, kMagic.data(), 4) != 0)
        throw std::runtime_error("wvcp: bad magic in " + store->source_);

    uint16_t version = get_u16(data + 4);
    uint16_t flags = get_u16(data + 6);
    uint32_t count = get_u32(data + 8);
    if (version != kVersion)
        throw std::runtime_error("wvcp: unsupported version " + std::to_string(version));

    const bool compressed = (flags & kFlagCompressed) != 0;
    std::size_t pos = 12;
    for (uint32_t i = 0; i < count; ++i) {
        auto need = [&](std::size_t n) {
            if (pos + n > size) throw std::runtime_error("wvcp: truncated container");
        };
        need(2); uint16_t name_len = get_u16(data + pos); pos += 2;
        need(name_len); std::string name(reinterpret_cast<const char*>(data + pos), name_len); pos += name_len;
        need(2); uint16_t mime_len = get_u16(data + pos); pos += 2;
        need(mime_len); std::string mime(reinterpret_cast<const char*>(data + pos), mime_len); pos += mime_len;
        need(4); uint32_t raw_len = get_u32(data + pos); pos += 4;
        need(4); uint32_t stored_len = get_u32(data + pos); pos += 4;
        need(stored_len);
        Asset asset;
        asset.mime = mime;
        if (compressed && stored_len != raw_len) {
            asset.bytes = inflate_bytes(data + pos, stored_len, raw_len);
        } else {
            asset.bytes.assign(data + pos, data + pos + stored_len);
        }
        pos += stored_len;
        store->assets_.emplace(std::move(name), Node{std::move(asset)});
    }
    WVC_INFO("AssetStore: loaded " + std::to_string(store->assets_.size()) +
             " assets from " + store->source_);
    return store;
}

std::vector<AssetStore::Entry> AssetStore::read_directory(const std::filesystem::path& root) {
    std::vector<Entry> entries;
    if (!std::filesystem::exists(root)) return entries;
    for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!dir_entry.is_regular_file()) continue;
        auto rel = std::filesystem::relative(dir_entry.path(), root);
        std::string name = rel.generic_string();  // always '/'
        std::ifstream in(dir_entry.path(), std::ios::binary);
        if (!in) continue;
        Entry e;
        e.name = name;
        e.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        auto dot = name.find_last_of('.');
        e.mime = (dot == std::string::npos)
                     ? "application/octet-stream"
                     : mime_for_extension(name.substr(dot + 1));
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return entries;
}

std::vector<uint8_t> AssetStore::pack(const std::vector<Entry>& entries, bool compress) {
    // Pre-compute compressed bodies so we know whether compression actually
    // helps before we decide on the global flag.
    struct Stored { const Entry* e; std::vector<uint8_t> body; uint32_t raw; bool comp; };
    std::vector<Stored> stored;
    stored.reserve(entries.size());
    for (const auto& e : entries) {
        Stored s{&e, e.bytes, static_cast<uint32_t>(e.bytes.size()), false};
        if (compress && !e.bytes.empty()) {
            auto deflated = deflate_bytes(e.bytes.data(), e.bytes.size());
            if (!deflated.empty() && deflated.size() < e.bytes.size()) {
                s.body = std::move(deflated);
                s.comp = true;
            }
        }
        stored.push_back(std::move(s));
    }

    const bool any_compressed = std::any_of(
        stored.begin(), stored.end(), [](const Stored& s) { return s.comp; });
    // The flag is global, so if only some entries shrank we either compress all
    // or none. If any shrank, re-run compression for the rest to keep the flag
    // consistent. Entries that don't shrink store raw bytes (stored_len==raw_len).
    std::vector<uint8_t> out;
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    put_u16(out, kVersion);
    put_u16(out, any_compressed ? kFlagCompressed : 0);
    put_u32(out, static_cast<uint32_t>(stored.size()));

    for (auto& s : stored) {
        put_u16(out, static_cast<uint16_t>(s.e->name.size()));
        out.insert(out.end(), s.e->name.begin(), s.e->name.end());
        put_u16(out, static_cast<uint16_t>(s.e->mime.size()));
        out.insert(out.end(), s.e->mime.begin(), s.e->mime.end());
        put_u32(out, s.raw);
        put_u32(out, static_cast<uint32_t>(s.body.size()));
        out.insert(out.end(), s.body.begin(), s.body.end());
    }
    return out;
}

bool AssetStore::write_pack(const std::vector<Entry>& entries, const std::filesystem::path& out,
                            bool compress, std::string* error) {
    try {
        auto bytes = pack(entries, compress);
        if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path());
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        if (!file) { if (error) *error = "cannot write " + out.string(); return false; }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(file);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool AssetStore::contains(const std::string& path) const {
    auto key = normalize(path);
    if (assets_.count(key)) return true;
    if (disk_mode_) return std::filesystem::is_regular_file(root_ / key);
    return false;
}

std::optional<Asset> AssetStore::get(const std::string& path) const {
    auto key = normalize(path);
    if (auto it = assets_.find(key); it != assets_.end()) return it->second.asset;

    if (disk_mode_) {
        auto full = root_ / key;
        if (std::filesystem::is_regular_file(full)) {
            std::ifstream in(full, std::ios::binary);
            Asset a;
            a.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            auto dot = key.find_last_of('.');
            a.mime = (dot == std::string::npos) ? "application/octet-stream"
                                                : mime_for_extension(key.substr(dot + 1));
            return a;
        }
    }
    return std::nullopt;
}

std::optional<Asset> AssetStore::resolve_url(std::string_view url) const {
    auto key = normalize(url);
    if (key.empty()) key = spa_shell_;
    // Directory request -> index.html inside it.
    if (auto asset = get(key)) return asset;
    if (auto asset = get(key + "/index.html")) return asset;
    if (spa_fallback_ && !has_file_extension(key)) {
        if (auto shell = get(spa_shell_)) return shell;
    }
    return std::nullopt;
}

}  // namespace wvc
