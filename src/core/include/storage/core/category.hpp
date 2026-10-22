#pragma once

#include <string_view>

namespace storage {

enum class Category {
    Folder,
    Image,
    Video,
    Audio,
    Document,
    Archive,
    Code,
    Application,
    System,
    Other,
};

Category categorize(std::string_view fileName);
const char* categoryName(Category c);

}  // namespace storage
