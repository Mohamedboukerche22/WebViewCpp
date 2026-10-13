#include "test.hpp"
#include "webviewcpp/asset_store.hpp"

#include <string>

using namespace wvc;

namespace {
std::vector<AssetStore::Entry> sample_entries() {
    return {
        {"index.html", "text/html", {'<', 'h', '1', '>', 'h', 'i', '<', '/', 'h', '1', '>'}},
        {"assets/app.js", "text/javascript", {'c', 'o', 'n', 's', 'o', 'l', 'e'}},
        {"assets/style.css", "text/css", {'b', 'o', 'd', 'y', '{', '}'}},
    };
}
}  // namespace

TEST(asset_pack_roundtrip_uncompressed) {
    auto entries = sample_entries();
    auto blob = AssetStore::pack(entries, /*compress=*/false);
    CHECK(blob.size() > 12);

    auto store = AssetStore::from_memory(blob.data(), blob.size());
    CHECK_EQ(store->count(), std::size_t(3));
    auto idx = store->get("index.html");
    CHECK(idx.has_value());
    CHECK_EQ(idx->mime, "text/html");
    CHECK_EQ(idx->bytes.size(), std::size_t(11));
}

TEST(asset_pack_roundtrip_compressed) {
    // Highly repetitive payload so deflate actually wins.
    std::string big(4096, 'A');
    std::vector<AssetStore::Entry> entries = {
        {"index.html", "text/html", {'o', 'k'}},
        {"assets/big.js", "text/javascript", std::vector<uint8_t>(big.begin(), big.end())},
    };
    auto blob = AssetStore::pack(entries, /*compress=*/true);
    CHECK(blob.size() < big.size() / 2);

    auto store = AssetStore::from_memory(blob.data(), blob.size());
    auto asset = store->get("assets/big.js");
    CHECK(asset.has_value());
    CHECK_EQ(asset->bytes.size(), big.size());
    CHECK_EQ(asset->bytes[0], uint8_t('A'));
}

TEST(asset_normalize_paths) {
    CHECK_EQ(AssetStore::normalize("app://local/index.html"), "index.html");
    CHECK_EQ(AssetStore::normalize("/assets/app.js"), "assets/app.js");
    CHECK_EQ(AssetStore::normalize("assets/../index.html"), "index.html");
    CHECK_EQ(AssetStore::normalize("a/b/../../c"), "c");
    CHECK_EQ(AssetStore::normalize("app://local/index.html?v=2"), "index.html");
}

TEST(asset_mime_types) {
    CHECK_EQ(AssetStore::mime_for_extension("js"), "text/javascript; charset=utf-8");
    CHECK_EQ(AssetStore::mime_for_extension("PNG"), "image/png");
    CHECK_EQ(AssetStore::mime_for_extension("wasm"), "application/wasm");
    CHECK_EQ(AssetStore::mime_for_extension("unknown"), "application/octet-stream");
}

TEST(asset_spa_fallback) {
    auto entries = sample_entries();
    auto blob = AssetStore::pack(entries, false);
    auto store = AssetStore::from_memory(blob.data(), blob.size());

    // A deep client-side route should fall back to the SPA shell.
    auto route = store->resolve_url("app://local/settings/profile");
    CHECK(route.has_value());
    CHECK_EQ(std::string(route->bytes.begin(), route->bytes.end()), "<h1>hi</h1>");

    // A missing file with an extension must NOT fall back.
    auto missing = store->resolve_url("app://local/missing.png");
    CHECK(!missing.has_value());
}
