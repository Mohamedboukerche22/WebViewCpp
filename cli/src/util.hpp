#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace wvc::cli {

// ANSI colours (disabled when not a TTY).
inline bool& color_enabled() {
    static bool enabled = true;
    return enabled;
}
inline std::string paint(const char* code, const std::string& text) {
    if (!color_enabled()) return text;
    return std::string("\033[") + code + "m" + text + "\033[0m";
}
inline std::string bold(const std::string& s) { return paint("1", s); }
inline std::string green(const std::string& s) { return paint("32", s); }
inline std::string red(const std::string& s) { return paint("31", s); }
inline std::string yellow(const std::string& s) { return paint("33", s); }
inline std::string dim(const std::string& s) { return paint("2", s); }

inline void log_step(const std::string& msg) { std::printf("%s %s\n", green("-->").c_str(), msg.c_str()); }
inline void log_info(const std::string& msg) { std::printf("    %s\n", msg.c_str()); }
inline void log_warn(const std::string& msg) { std::printf("%s %s\n", yellow("warn:").c_str(), msg.c_str()); }
inline void log_error(const std::string& msg) { std::fprintf(stderr, "%s %s\n", red("error:").c_str(), msg.c_str()); }

struct RunResult {
    int code = -1;
    std::string output;
};

// Runs a shell command, optionally capturing combined stdout/stderr.
inline RunResult run(const std::string& command, bool capture = false) {
    RunResult result;
    std::string full = command + (capture ? " 2>&1" : "");
    if (capture) {
        std::array<char, 4096> buffer{};
        FILE* pipe = popen(full.c_str(), "r");
        if (!pipe) { result.code = -1; return result; }
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
            result.output += buffer.data();
        result.code = pclose(pipe);
    } else {
        result.code = std::system(full.c_str());
    }
    return result;
}

inline bool command_exists(const std::string& exe) {
#if defined(_WIN32)
    std::string probe = "where " + exe + " >NUL 2>NUL";
#else
    std::string probe = "command -v " + exe + " >/dev/null 2>&1";
#endif
    return std::system(probe.c_str()) == 0;
}

inline std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline bool write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

inline std::string host_platform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

inline std::string quote_arg(const std::string& arg) {
#if defined(_WIN32)
    return "\"" + arg + "\"";
#else
    return "'" + arg + "'";
#endif
}

// Escapes a byte string into a C string literal (each byte as \xNN, with a
// concatenation boundary so hex escapes cannot run into each other).
inline std::string to_c_string_literal(const std::vector<unsigned char>& data, int per_line = 32) {
    std::string out;
    out.reserve(data.size() * 5);
    out += '"';
    int count = 0;
    for (unsigned char byte : data) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\x%02x", byte);
        out += buf;
        if (++count % per_line == 0) {
            out += "\"\n    \"";
        }
    }
    if (data.empty()) out += '0';
    out += '"';
    return out;
}

}  // namespace wvc::cli
