#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wvc {

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

enum class Theme { System, Light, Dark };

enum class Cursor {
    Default, Pointer, Text, Crosshair, Move, Wait, NotAllowed,
};

// Result of a native user-facing dialog that the host cancelled.
struct FileFilter {
    std::string name;               // "Images"
    std::vector<std::string> globs; // {"png", "jpg"}
};

}  // namespace wvc
