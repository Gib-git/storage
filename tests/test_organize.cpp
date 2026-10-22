#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "TempTree.hpp"
#include "check.hpp"
#include "storage/core/executor.hpp"
#include "storage/core/exif.hpp"
#include "storage/core/organize.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"

using namespace storage;
namespace fs = std::filesystem;

namespace {

// Minimal JPEG whose EXIF DateTimeOriginal is `date` ("YYYY:MM:DD HH:MM:SS").
std::string jpegWithDate(const std::string& date) {
    std::string tiff = "II";
    auto u16 = [&](uint16_t v) {
        tiff.push_back(static_cast<char>(v & 0xFF));
        tiff.push_back(static_cast<char>(v >> 8));
    };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) tiff.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    u16(42);
    u32(8);                          // IFD0 at 8
    u16(1);                          // one entry
    u16(0x8769), u16(4), u32(1), u32(26);  // ExifIFD pointer -> 26
    u32(0);                          // no next IFD
    u16(1);                          // Exif IFD: one entry
    u16(0x9003), u16(2), u32(20), u32(44);  // DateTimeOriginal, ASCII[20] at 44
    u32(0);
    tiff += date;
    tiff.push_back('\0');

    std::string app1 = "Exif";
    app1.append(2, '\0');
    app1 += tiff;
    const size_t len = app1.size() + 2;
    std::string jpeg = "\xFF\xD8\xFF\xE1";
    jpeg.push_back(static_cast<char>(len >> 8));
    jpeg.push_back(static_cast<char>(len & 0xFF));
    jpeg += app1;
    jpeg += std::string(1000, 'x');  // pretend image data
    jpeg += "\xFF\xD9";
    return jpeg;
}

void write(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

void setYear(const fs::path& p, int year) {
    // Approximate: seconds from a reference date are enough to land in the right year.
    const auto now = fs::file_time_type::clock::now();
    const auto ref = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(ref);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const int years = (tm.tm_year + 1900) - year;
    fs::last_write_time(p, now - std::chrono::hours(24 * 366 * years + 24 * 40));
}

std::unique_ptr<ScanTree> scan(const fs::path& root) {
    LocalFileSystem lfs;
    return Scanner(lfs, ScanOptions::defaultsFor(toUtf8(root))).run();
}

std::string rel(const fs::path& root, const std::string& p) {
    return fs::relative(fromUtf8(p), root).generic_string();
}

std::map<std::string, std::string> movesOf(const fs::path& root, const OrganizePlan& plan) {
    std::map<std::string, std::string> m;
    for (const auto& mv : plan.moves) m[rel(root, mv.src)] = rel(root, mv.dst);
    return m;
}

std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> out;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        out[fs::relative(e.path(), root).generic_string()] = ss.str();
    }
    return out;
}

}  // namespace

TEST_CASE("organize: sanitizeSegment makes names safe everywhere") {
    CHECK(sanitizeSegment("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
    CHECK(sanitizeSegment("..") == "_");
    CHECK(sanitizeSegment("") == "_");
    CHECK(sanitizeSegment("name. ") == "name");
    CHECK(sanitizeSegment("CON") == "_CON");
    CHECK(sanitizeSegment("con.txt") == "_con.txt");
    CHECK(sanitizeSegment("Photos 2024") == "Photos 2024");
    CHECK(sanitizeSegment(std::string(300, 'a')).size() == 200);
}

TEST_CASE("organize: EXIF capture date is read from JPEG") {
    const CaptureDate d = readCaptureDate(jpegWithDate("2019:07:04 10:11:12"));
    CHECK(d.valid());
    CHECK(d.year == 2019);
    CHECK(d.month == 7);
    CHECK(d.day == 4);
    CHECK(d.hour == 10);
    CHECK(!readCaptureDate("not a jpeg at all").valid());
    CHECK(!readCaptureDate(jpegWithDate("0000:00:00 00:00:00")).valid());
}

TEST_CASE("organize: presets round-trip through JSON") {
    for (const auto& p : builtinPresets()) {
        const RuleSet back = RuleSet::fromJson(json::Value::parse(p.toJson().dump()));
        CHECK(back.toJson().dump() == p.toJson().dump());
    }
    bool threw = false;
    try {
        RuleSet::fromJson(json::Value::parse(R"({"rules":[{"name":"bad","match":{"nameRegex":"("},"folder":"x"}]})"));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("organize: by type plans moves, skips packages, renames clashes") {
    TempTree t;
    t.file("a.pdf", 10);
    t.file("sub/b.pdf", 20);
    t.file("sub/deep/c.jpg", 30);
    t.file("Documents/a.pdf", 40);       // already where "a.pdf" wants to go -> clash
    t.file("Documents/kept.pdf", 50);    // already in place
    t.file("Thing.app/Contents/x.pdf", 60);  // inside an app bundle: never touched
    t.file(".git/config", 70);
    t.file(".hidden.pdf", 80);
    t.file("sub/.DS_Store", 1);

    LocalFileSystem lfs;
    RuleSet rs = builtinPresets()[0];  // By file type
    auto tree = scan(t.root());
    const OrganizePlan plan = Organizer(lfs, rs).plan(*tree->root);
    const auto m = movesOf(t.root(), plan);

    CHECK(m.size() == 3);
    CHECK(m.at("a.pdf") == "Documents/a (2).pdf");
    CHECK(m.at("sub/b.pdf") == "Documents/b.pdf");
    CHECK(m.at("sub/deep/c.jpg") == "Images/c.jpg");
    CHECK(plan.alreadyInPlace == 2);
    CHECK(plan.protectedSkipped >= 2);  // Thing.app and .git contents
    CHECK(std::count_if(plan.moves.begin(), plan.moves.end(), [](const PlannedMove& mv) { return mv.renamed; }) == 1);

    std::set<std::string> removed;
    for (const auto& d : plan.removeDirs) removed.insert(rel(t.root(), d));
    CHECK(removed == std::set<std::string>{"sub", "sub/deep"});
    CHECK(rel(t.root(), plan.removeDirs.front()) == "sub/deep");  // deepest first
}

TEST_CASE("organize: date templates use EXIF for photos, mtime otherwise") {
    TempTree t;
    write(t.root() / "img.jpg", jpegWithDate("2015:03:09 08:00:00"));
    t.file("notes.txt", 5);
    setYear(t.root() / "notes.txt", 2020);
    setYear(t.root() / "img.jpg", 2023);  // mtime differs from capture date on purpose

    LocalFileSystem lfs;
    RuleSet rs;
    rs.fallbackFolder = "{year}/{month} - {monthname}";
    auto tree = scan(t.root());
    auto m = movesOf(t.root(), Organizer(lfs, rs).plan(*tree->root));
    CHECK(m.at("img.jpg") == "2015/03 - March/img.jpg");
    CHECK(m.at("notes.txt").rfind("2020/", 0) == 0);

    rs.dateSource = DateSource::Modified;
    m = movesOf(t.root(), Organizer(lfs, rs).plan(*tree->root));
    CHECK(m.at("img.jpg").rfind("2023/", 0) == 0);
}

TEST_CASE("organize: rules, rename templates, skip-on-conflict and destination") {
    TempTree t, out;
    t.file("Invoice-001.pdf", 10);
    t.file("holiday.mov", 2000);
    t.file("song.mp3", 30);
    t.file("random.xyz", 1);
    out.file("Music/song.mp3", 99);  // already present at the destination

    RuleSet rs;
    OrganizeRule invoices;
    invoices.name = "Invoices";
    invoices.match.nameContains = "invoice";
    invoices.folder = "Finance/Invoices";
    invoices.rename = "{year} {name}";
    OrganizeRule bigVideos;
    bigVideos.name = "Big videos";
    bigVideos.match.categories = {Category::Video};
    bigVideos.match.minSize = 1000;
    bigVideos.folder = "Videos/{size}";
    OrganizeRule music;
    music.name = "Music";
    music.match.extensions = {"mp3"};
    music.folder = "Music";
    rs.rules = {invoices, bigVideos, music};
    rs.conflict = ConflictPolicy::Skip;

    LocalFileSystem lfs;
    auto tree = scan(t.root());
    const OrganizePlan plan = Organizer(lfs, rs).plan(*tree->root, toUtf8(out.root()));
    std::map<std::string, std::string> m;
    for (const auto& mv : plan.moves) m[rel(t.root(), mv.src)] = rel(out.root(), mv.dst);

    CHECK(m.size() == 2);
    CHECK(m.at("Invoice-001.pdf").rfind("Finance/Invoices/", 0) == 0);
    CHECK(m.at("Invoice-001.pdf").size() > 4);
    CHECK(m.at("Invoice-001.pdf").substr(m.at("Invoice-001.pdf").size() - 4) == ".pdf");  // extension kept
    CHECK(m.at("holiday.mov") == "Videos/Small (under 1 MB)/holiday.mov");
    CHECK(plan.conflictsSkipped == 1);  // song.mp3 exists at the destination
    CHECK(plan.unmatched == 1);         // random.xyz: no rule, no fallback
}

TEST_CASE("organize: executing a plan and undoing it restores everything") {
    TempTree t, j;
    t.file("inbox/report.pdf", 100, 'r');
    t.file("inbox/photo.jpg", 200, 'p');
    t.file("inbox/nested/movie.mp4", 300, 'm');
    t.file("inbox/nested/.DS_Store", 3, 'd');
    t.file("keep.txt", 5, 'k');
    const auto before = snapshot(t.root());

    LocalFileSystem lfs;
    RuleSet rs = builtinPresets()[0];
    auto tree = scan(t.root());
    const OrganizePlan plan = Organizer(lfs, rs).plan(*tree->root);
    Journal journal(j.root() / "journal.db");
    const int64_t batch = journal.createBatch("organize", "test", "local", plan.toOps());
    Executor ex(lfs, journal);
    const ExecResult r = ex.run(batch);
    CHECK(r.failed == 0);
    CHECK(fs::exists(t.root() / "Documents" / "report.pdf"));
    CHECK(fs::exists(t.root() / "Images" / "photo.jpg"));
    CHECK(fs::exists(t.root() / "Videos" / "movie.mp4"));
    CHECK(fs::exists(t.root() / "Documents" / "keep.txt"));
    CHECK(!fs::exists(t.root() / "inbox"));  // emptied folders (incl. .DS_Store-only) removed

    const ExecResult u = ex.undo(batch);
    CHECK(u.failed == 0);
    auto after = snapshot(t.root());
    after.erase("inbox/nested/.DS_Store");  // OS junk is not restored
    auto expected = before;
    expected.erase("inbox/nested/.DS_Store");
    CHECK(after == expected);
    CHECK(!fs::exists(t.root() / "Documents"));
}
