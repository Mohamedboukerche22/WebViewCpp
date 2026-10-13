#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wvc {

// One file inside an app bundle.
struct Asset {
    std::string mime;
    std::vector<uint8_t> bytes;
};

// The "WVCP" container. A single file — or a single byte array compiled into
// the executable — holds the whole frontend. Layout (little-endian):
//
//   char     magic[4]      = 'W','V','C','P'
//   uint16   version       = 1
//   uint16   flags         bit0: entries are zlib/deflate compressed
//   uint32   entry_count
//   repeated entry_count times:
//     uint16 name_len;  char name[name_len]
//     uint16 mime_len;  char mime[mime_len]
//     uint32 raw_len            (uncompressed size)
//     uint32 stored_len         (== raw_len when flags==0)
//     uint8  data[stored_len]
class AssetStore {
public:
    AssetStore() = default;

    // Dev mode: read straight off disk. `root` is the directory holding the
    // built frontend (e.g. `dist/`).
    static std::unique_ptr<AssetStore> from_directory(std::filesystem::path root);

    // Packaged mode: parse an in-memory WVCP container. The caller keeps the
    // buffer alive for the lifetime of the store; most often it is a static
    // array in the generated embed TU.
    static std::unique_ptr<AssetStore> from_memory(const uint8_t* data, std::size_t size,
                                                   std::string source_name = "<embedded>");

    // Packaged mode: load a .wvcp file from disk.
    static std::unique_ptr<AssetStore> from_file(const std::filesystem::path& pack);

    // ---- authoring (used by tools/wvc) ---------------------------------
    struct Entry {
        std::string name;
        std::string mime;
        std::vector<uint8_t> bytes;
    };
    static std::vector<Entry> read_directory(const std::filesystem::path& root);
    static std::vector<uint8_t> pack(const std::vector<Entry>& entries, bool compress);
    static bool write_pack(const std::vector<Entry>& entries,
                           const std::filesystem::path& out, bool compress,
                           std::string* error = nullptr);

    // ---- runtime lookup -------------------------------------------------
    bool contains(const std::string& path) const;
    std::optional<Asset> get(const std::string& path) const;
    std::size_t count() const { return assets_.size(); }
    const std::string& source_name() const { return source_; }

    // A request for a route with no file extension that misses the store is
    // served the SPA shell (index.html) so client-side routing works.
    void set_spa_fallback(bool enabled, std::string shell = "index.html") {
        spa_fallback_ = enabled;
        spa_shell_ = std::move(shell);
    }

    // Resolve an app:// URL (or a bare path) to a stored asset.
    std::optional<Asset> resolve_url(std::string_view url) const;

    static std::string mime_for_extension(std::string_view ext);
    static std::string normalize(std::string_view path);

private:
    struct Node { Asset asset; };
    std::map<std::string, Node> assets_;
    std::filesystem::path root_;
    bool disk_mode_ = false;
    bool spa_fallback_ = true;
    std::string spa_shell_ = "index.html";
    std::string source_ = "<memory>";
};

}  // namespace wvc
