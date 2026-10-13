#pragma once

#include "util.hpp"
#include "webviewcpp/json.hpp"

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace wvc::cli {

struct WindowConfig {
    int width = 1024;
    int height = 720;
    int min_width = 320;
    int min_height = 240;
    std::string title;
    bool resizable = true;
    bool frameless = false;
    bool devtools = false;
};

struct FrontendConfig {
    std::string root = ".";
    std::string dist = "dist";
    std::string install = "npm install";
    std::string build = "npm run build";
    std::string dev = "npm run dev";
    std::string dev_url = "http://localhost:5173";
};

struct PackageConfig {
    std::string category = "Utility";
    std::string icon;
    std::vector<std::string> extra_files;
};

struct Config {
    std::string name = "WebViewCppApp";
    std::string identifier = "com.example.webviewcppapp";
    std::string version = "0.1.0";
    std::string binary_name;
    WindowConfig window;
    FrontendConfig frontend;
    PackageConfig package;
    std::string framework_root;      // path to the WebViewCpp checkout / install
    bool embed = true;               // compile assets into the executable
    bool compress = true;
    std::string embed_mode = "array";  // "array" or "cxx_embed"

    static Config from_json(const Json& j) {
        Config c;
        c.name = j["name"].string_or(c.name);
        c.identifier = j["identifier"].string_or(c.identifier);
        c.version = j["version"].string_or(c.version);
        c.binary_name = j["binaryName"].string_or(c.binary_name);
        c.framework_root = j["frameworkRoot"].string_or(c.framework_root);
        c.embed = j["embed"].bool_or(c.embed);
        c.compress = j["compress"].bool_or(c.compress);
        c.embed_mode = j["embedMode"].string_or(c.embed_mode);

        const Json& w = j["window"];
        c.window.width = static_cast<int>(w["width"].int_or(c.window.width));
        c.window.height = static_cast<int>(w["height"].int_or(c.window.height));
        c.window.min_width = static_cast<int>(w["minWidth"].int_or(c.window.min_width));
        c.window.min_height = static_cast<int>(w["minHeight"].int_or(c.window.min_height));
        c.window.title = w["title"].string_or(c.window.title);
        c.window.resizable = w["resizable"].bool_or(c.window.resizable);
        c.window.frameless = w["frameless"].bool_or(c.window.frameless);
        c.window.devtools = w["devtools"].bool_or(c.window.devtools);

        const Json& f = j["frontend"];
        c.frontend.root = f["root"].string_or(c.frontend.root);
        c.frontend.dist = f["dist"].string_or(c.frontend.dist);
        c.frontend.install = f["install"].string_or(c.frontend.install);
        c.frontend.build = f["build"].string_or(c.frontend.build);
        c.frontend.dev = f["dev"].string_or(c.frontend.dev);
        c.frontend.dev_url = f["devUrl"].string_or(c.frontend.dev_url);

        const Json& p = j["package"];
        c.package.category = p["category"].string_or(c.package.category);
        c.package.icon = p["icon"].string_or(c.package.icon);
        for (const auto& extra : p["extraFiles"].as_array())
            c.package.extra_files.push_back(extra.string_or(""));

        if (c.window.title.empty()) c.window.title = c.name;
        if (c.binary_name.empty()) c.binary_name = sanitize(c.name);
        return c;
    }

    Json to_json() const {
        Json j = Json::object{};
        j["name"] = name;
        j["identifier"] = identifier;
        j["version"] = version;
        j["binaryName"] = binary_name;
        j["embed"] = embed;
        j["compress"] = compress;
        j["embedMode"] = embed_mode;
        if (!framework_root.empty()) j["frameworkRoot"] = framework_root;

        Json w = Json::object{};
        w["width"] = window.width;
        w["height"] = window.height;
        w["minWidth"] = window.min_width;
        w["minHeight"] = window.min_height;
        w["title"] = window.title;
        w["resizable"] = window.resizable;
        w["frameless"] = window.frameless;
        w["devtools"] = window.devtools;
        j["window"] = w;

        Json f = Json::object{};
        f["root"] = frontend.root;
        f["dist"] = frontend.dist;
        f["install"] = frontend.install;
        f["build"] = frontend.build;
        f["dev"] = frontend.dev;
        f["devUrl"] = frontend.dev_url;
        j["frontend"] = f;

        Json p = Json::object{};
        p["category"] = package.category;
        p["icon"] = package.icon;
        Json extras = Json::array{};
        for (const auto& e : package.extra_files) extras.push_back(e);
        p["extraFiles"] = extras;
        j["package"] = p;
        return j;
    }

    static std::string sanitize(const std::string& input) {
        std::string out;
        for (char c : input) {
            if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(std::tolower(c)));
            else if (c == ' ' || c == '-' || c == '_') out.push_back('-');
        }
        if (out.empty()) out = "app";
        return out;
    }

    static Config load(const fs::path& path) {
        std::string text = read_file(path);
        if (text.empty()) throw std::runtime_error("cannot read config: " + path.string());
        return from_json(Json::parse(text));
    }
};

}  // namespace wvc::cli
