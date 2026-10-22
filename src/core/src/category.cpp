#include "storage/core/category.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace storage {

namespace {
const std::unordered_map<std::string, Category>& table() {
    static const std::unordered_map<std::string, Category> t = [] {
        std::unordered_map<std::string, Category> m;
        auto add = [&m](Category c, std::initializer_list<const char*> exts) {
            for (const char* e : exts) m.emplace(e, c);
        };
        add(Category::Image, {"jpg", "jpeg", "png", "gif", "bmp", "tif", "tiff", "webp", "heic", "heif", "raw",
                              "cr2", "cr3", "nef", "arw", "dng", "orf", "rw2", "svg", "ico", "psd", "avif"});
        add(Category::Video, {"mp4", "mov", "m4v", "avi", "mkv", "wmv", "flv", "webm", "mpg", "mpeg", "3gp",
                              "mts", "m2ts", "vob", "ts"});
        add(Category::Audio, {"mp3", "m4a", "aac", "flac", "wav", "aiff", "aif", "ogg", "opus", "wma", "alac",
                              "mid", "midi"});
        add(Category::Document, {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt", "ods", "odp", "rtf",
                                 "txt", "md", "csv", "pages", "numbers", "key", "epub", "mobi", "tex"});
        add(Category::Archive, {"zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "zst", "lz4", "iso",
                                "img", "cab"});
        add(Category::Code, {"c", "cc", "cpp", "cxx", "h", "hpp", "py", "js", "mjs", "ts", "tsx", "jsx", "java",
                             "kt", "go", "rs", "rb", "php", "cs", "swift", "m", "mm", "sh", "ps1", "json",
                             "yaml", "yml", "toml", "xml", "html", "css", "scss", "sql", "lua", "dart"});
        add(Category::Application, {"app", "dmg", "exe", "msi", "pkg", "deb", "rpm", "appimage", "apk", "ipa",
                                    "jar", "bat", "com"});
        add(Category::System, {"dll", "so", "dylib", "sys", "drv", "o", "obj", "a", "lib", "pdb", "log",
                               "tmp", "cache", "db", "sqlite", "plist", "ini", "cfg", "dat", "bak"});
        return m;
    }();
    return t;
}
}  // namespace

Category categorize(std::string_view fileName) {
    const size_t dot = fileName.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == fileName.size()) return Category::Other;
    std::string ext(fileName.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto& t = table();
    const auto it = t.find(ext);
    return it == t.end() ? Category::Other : it->second;
}

const char* categoryName(Category c) {
    switch (c) {
        case Category::Folder: return "Folder";
        case Category::Image: return "Images";
        case Category::Video: return "Videos";
        case Category::Audio: return "Audio";
        case Category::Document: return "Documents";
        case Category::Archive: return "Archives";
        case Category::Code: return "Code";
        case Category::Application: return "Applications";
        case Category::System: return "System";
        case Category::Other: return "Other";
    }
    return "Other";
}

}  // namespace storage
