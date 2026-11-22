// media::MediaLibrary: folders, track index, file operations (media spec 3).

#include "wisp/media/library.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/media_data.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/os.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::core::IoLane;
using wisp::core::Medium;
using wisp::core::StorageWorker;
using wisp::media::MediaEntry;
using wisp::media::MediaLibrary;
using wisp::media::MediaLibraryState;
using wisp::media::Track;
using wisp::media::TrackPage;
using wisp::media::TrackQuery;

namespace fs = std::filesystem;

namespace {

constexpr const char* kTracks[] = {
    "sine_44k_stereo_cbr128.mp3", "sine_48k_mono_vbr.mp3", "sine_22k_noxing.mp3",
    "sine_44k_stereo.wav",        "sine_16k_mono.wav",
};

std::string read_raw(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_raw(const fs::path& path, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << contents;
}

bool on_disk(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

wisp::os::LargeBuffer buffer_of(const std::string& s) {
    auto b = wisp::os::LargeBuffer::allocate(s.size());
    std::copy(s.begin(), s.end(), b.data());
    return b;
}

std::span<const uint8_t> bytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// Waits for an asynchronous call's result.
template <class T>
struct Wait {
    std::optional<Result<T>> got;
    wisp::os::Semaphore sem{1, 0};
    MediaLibrary::Done<T> cb() {
        return [this](Result<T> r) {
            got.emplace(std::move(r));
            sem.give();
        };
    }
    Result<T> take() {
        EXPECT_TRUE(sem.take(5000));
        if (!got) return Result<T>::err(ErrorCode::Timeout, "no result");
        return std::move(*got);
    }
};

struct Fixture {
    explicit Fixture(bool sd_online = true, bool with_sd = true)
        : in(wisp::test::temp_dir().string()), sd(wisp::test::temp_dir().string()) {
        int_dev = std::make_shared<wisp::test::FakeStorage>("storage.int", in, false);
        sd_dev = std::make_shared<wisp::test::FakeStorage>("storage.sd", sd, true);
        sd_dev->online_flag = sd_online;
        registry.add(int_dev);
        if (with_sd) registry.add(sd_dev);
        EXPECT_TRUE(bus.start());
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        EXPECT_TRUE(flash.start());
        EXPECT_TRUE(sd_worker.start());
        router = std::make_unique<wisp::core::StorageRouter>(flash, &sd_worker);
    }

    ~Fixture() {
        lib.reset();
        router.reset();
        sd_worker.stop();
        flash.stop();
        storage.reset();
        bus.stop();
    }

    void make_library() {
        lib = std::make_unique<MediaLibrary>(wisp::media::MediaLibraryDeps{
            .storage = *storage, .router = router.get(), .bus = bus, .root = root()});
    }

    // Starts a library and waits for its first scan.
    void start() {
        make_library();
        lib->start();
        wait_ready();
    }

    void wait_ready() {
        for (int i = 0; i < 500; ++i) {
            MediaLibraryState s = lib->state().get();
            if (s.available && !s.scanning) break;
            wisp::os::sleep_ms(10);
        }
        ASSERT_TRUE(router->drain(5000));
        MediaLibraryState s = lib->state().get();
        ASSERT_TRUE(s.available);
        ASSERT_FALSE(s.scanning);
    }

    void rescan() {
        lib->rescan();
        ASSERT_TRUE(router->drain(5000));
        ASSERT_FALSE(lib->state().get().scanning);
    }

    // refresh() touches the card and internal flash: on wisp.fs.
    void set_sd(bool online) {
        sd_dev->online_flag = online;
        ASSERT_TRUE(flash.run_sync([this] { storage->refresh(); }, 5000));
    }

    std::string root() const { return sd + "/media"; }

    void copy_tracks(const std::string& dir = "music/x") {
        std::error_code ec;
        fs::create_directories(fs::path(root()) / dir, ec);
        for (const char* name : kTracks) {
            fs::copy_file(wisp::test::media_data(name), fs::path(root()) / dir / name,
                          fs::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(ec) << name;
        }
    }

    TrackPage all_tracks(TrackQuery q = {}) {
        Wait<TrackPage> w;
        lib->tracks(std::move(q), w.cb());
        auto r = w.take();
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
        return r ? std::move(r.value()) : TrackPage{};
    }

    std::vector<MediaEntry> list(const std::string& dir) {
        Wait<std::vector<MediaEntry>> w;
        lib->list(dir, w.cb());
        auto r = w.take();
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
        return r ? std::move(r.value()) : std::vector<MediaEntry>{};
    }

    Result<void> write(const std::string& path, const std::string& data) {
        Wait<void> w;
        lib->write(path, buffer_of(data), w.cb());
        return w.take();
    }

    Result<void> remove(const std::string& path) {
        Wait<void> w;
        lib->remove(path, w.cb());
        return w.take();
    }

    Result<void> mkdir(const std::string& path) {
        Wait<void> w;
        lib->mkdir(path, w.cb());
        return w.take();
    }

    // Parks wisp.sd.io in a job until the returned gate is released, once
    // the scan has read at least one file's tags (its folder is visited).
    // Returns the tag reads done when the worker stopped.
    uint64_t hold_sd_mid_scan(wisp::os::Semaphore& release) {
        for (int i = 0; i < 5000 && lib->tag_reads() == 0; ++i) wisp::os::sleep_ms(1);
        wisp::os::Semaphore started(1, 0);
        std::atomic<uint64_t> reads{0};
        EXPECT_TRUE(router->submit(Medium::Sd, [&, this] {
            reads = lib->tag_reads();
            started.give();
            release.take(wisp::os::kForever);
        }));
        EXPECT_TRUE(started.take(5000));
        return reads.load();
    }

    // fn() on wisp.sd.io.
    template <class Fn>
    void on_sd(Fn fn) {
        ASSERT_TRUE(router->run_sync(Medium::Sd, fn, 5000));
    }

    std::string in;
    std::string sd;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::shared_ptr<wisp::test::FakeStorage> int_dev;
    std::shared_ptr<wisp::test::FakeStorage> sd_dev;
    std::unique_ptr<wisp::core::StorageManager> storage;
    StorageWorker flash;
    StorageWorker sd_worker{StorageWorker::sd_io_config()};
    std::unique_ptr<wisp::core::StorageRouter> router;
    std::unique_ptr<MediaLibrary> lib;
};

std::vector<std::string> names(const std::vector<MediaEntry>& entries) {
    std::vector<std::string> out;
    for (const auto& e : entries) out.push_back(e.name);
    return out;
}

const Track* find(const TrackPage& page, const std::string& id) {
    for (const auto& t : page.items)
        if (t.id == id) return &t;
    return nullptr;
}

}  // namespace

TEST(MediaLibrary, CreatesFoldersOnMount) {
    Fixture f;
    f.start();
    for (const char* dir : {"music", "download", "document"})
        EXPECT_TRUE(fs::is_directory(fs::path(f.root()) / dir)) << dir;
    EXPECT_EQ(names(f.list("")), (std::vector<std::string>{"document", "download", "music"}));
    auto music = f.list("music");
    EXPECT_TRUE(music.empty());
    auto root = f.list("");
    ASSERT_EQ(root.size(), 3u);
    EXPECT_TRUE(root[0].dir);
}

TEST(MediaLibrary, NoCardIsUnavailable) {
    for (bool with_sd : {true, false}) {
        Fixture f(/*sd_online=*/false, with_sd);
        f.make_library();
        f.lib->start();
        wisp::os::sleep_ms(50);
        ASSERT_TRUE(f.router->drain(5000));
        EXPECT_FALSE(f.lib->state().get().available);

        Wait<std::vector<MediaEntry>> l;
        f.lib->list("", l.cb());
        EXPECT_EQ(l.take().error().code, ErrorCode::StorageUnavailable);
        Wait<TrackPage> t;
        f.lib->tracks({}, t.cb());
        EXPECT_EQ(t.take().error().code, ErrorCode::StorageUnavailable);
        EXPECT_EQ(f.write("music/a.mp3", "x").error().code, ErrorCode::StorageUnavailable);
        Wait<wisp::os::LargeBuffer> r;
        f.lib->read("music/a.mp3", 0, 10, r.cb());
        EXPECT_EQ(r.take().error().code, ErrorCode::StorageUnavailable);
        EXPECT_FALSE(f.lib->track("music/a.mp3"));
        EXPECT_FALSE(on_disk(fs::path(f.root())));
    }
}

TEST(MediaLibrary, ScanIndexesTracksWithTags) {
    Fixture f;
    f.copy_tracks();
    f.start();
    TrackPage page = f.all_tracks();
    ASSERT_EQ(page.total, 5u);
    ASSERT_EQ(page.items.size(), 5u);
    EXPECT_EQ(f.lib->state().get().track_count, 5u);
    EXPECT_TRUE(std::is_sorted(page.items.begin(), page.items.end(),
                               [](const Track& a, const Track& b) { return a.id < b.id; }));

    const Track* cbr = find(page, "music/x/sine_44k_stereo_cbr128.mp3");
    ASSERT_NE(cbr, nullptr);
    EXPECT_EQ(cbr->title, "Sine 440");
    EXPECT_EQ(cbr->artist, "Wisp");
    EXPECT_EQ(cbr->album, "Tests");
    EXPECT_EQ(cbr->duration_s, 3u);
    EXPECT_EQ(cbr->size, fs::file_size(wisp::test::media_data("sine_44k_stereo_cbr128.mp3")));
    EXPECT_GT(cbr->mtime, 0);

    const Track* vbr = find(page, "music/x/sine_48k_mono_vbr.mp3");
    ASSERT_NE(vbr, nullptr);
    EXPECT_EQ(vbr->title, "Sine 48k");
    EXPECT_EQ(vbr->duration_s, 2u);

    const Track* v1 = find(page, "music/x/sine_22k_noxing.mp3");
    ASSERT_NE(v1, nullptr);
    EXPECT_EQ(v1->title, "Sine 22k");
    EXPECT_EQ(v1->artist, "Wisp");
    EXPECT_EQ(v1->duration_s, 2u);

    const Track* wav = find(page, "music/x/sine_16k_mono.wav");
    ASSERT_NE(wav, nullptr);
    EXPECT_EQ(wav->title, "sine_16k_mono");
    EXPECT_EQ(wav->artist, "");
    EXPECT_EQ(wav->duration_s, 1u);

    auto one = f.lib->track("music/x/sine_48k_mono_vbr.mp3");
    ASSERT_TRUE(one);
    EXPECT_EQ(*one, *vbr);
    EXPECT_FALSE(f.lib->track("music/x/none.mp3"));

    // The index was saved, at version 1, with every track.
    auto doc = wisp::from_json(read_raw(fs::path(f.root()) / ".index.json"));
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc.value().find("version")->as_int(), 1);
    EXPECT_EQ(doc.value().find("tracks")->as_list().size(), 5u);
}

TEST(MediaLibrary, RescanReadsOnlyChangedFiles) {
    Fixture f;
    f.copy_tracks();
    f.start();
    EXPECT_EQ(f.lib->tag_reads(), 5u);
    uint32_t rev = f.lib->state().get().revision;

    f.rescan();
    EXPECT_EQ(f.lib->tag_reads(), 5u);
    EXPECT_EQ(f.lib->state().get().revision, rev);

    // A changed size is a changed file.
    fs::copy_file(wisp::test::media_data("sine_16k_mono.wav"),
                  fs::path(f.root()) / "music/x/sine_44k_stereo.wav",
                  fs::copy_options::overwrite_existing);
    f.rescan();
    EXPECT_EQ(f.lib->tag_reads(), 6u);
    EXPECT_NE(f.lib->state().get().revision, rev);
    EXPECT_EQ(f.lib->track("music/x/sine_44k_stereo.wav")->duration_s, 1u);

    // A reboot loads the saved index and reads nothing.
    f.lib.reset();
    f.start();
    EXPECT_EQ(f.lib->tag_reads(), 0u);
    EXPECT_EQ(f.all_tracks().total, 5u);
    EXPECT_EQ(f.lib->track("music/x/sine_44k_stereo_cbr128.mp3")->title, "Sine 440");
}

TEST(MediaLibrary, RemovedFileLeavesTheIndex) {
    Fixture f;
    f.copy_tracks();
    f.start();
    fs::remove(fs::path(f.root()) / "music/x/sine_48k_mono_vbr.mp3");
    f.rescan();
    EXPECT_EQ(f.all_tracks().total, 4u);
    EXPECT_EQ(f.lib->state().get().track_count, 4u);
    EXPECT_FALSE(f.lib->track("music/x/sine_48k_mono_vbr.mp3"));
}

TEST(MediaLibrary, CorruptIndexIsRebuilt) {
    Fixture f;
    f.copy_tracks();
    write_raw(fs::path(f.root()) / ".index.json", "{\"version\":1,\"tracks\":[{\"id\":");
    f.start();
    EXPECT_EQ(f.all_tracks().total, 5u);
    EXPECT_EQ(f.lib->tag_reads(), 5u);
    auto doc = wisp::from_json(read_raw(fs::path(f.root()) / ".index.json"));
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc.value().find("tracks")->as_list().size(), 5u);
}

TEST(MediaLibrary, OldVersionIndexIsRebuilt) {
    Fixture f;
    f.copy_tracks();
    auto size = fs::file_size(fs::path(f.root()) / "music/x/sine_16k_mono.wav");
    // Matches the file on size, but at version 0 it is not trusted.
    write_raw(fs::path(f.root()) / ".index.json",
              "{\"version\":0,\"tracks\":[{\"id\":\"music/x/sine_16k_mono.wav\",\"title\":"
              "\"Stale\",\"artist\":\"\",\"album\":\"\",\"duration_s\":9,\"size\":" +
                  std::to_string(size) + ",\"mtime\":0},{\"id\":\"music/ghost.mp3\"}]}");
    f.start();
    EXPECT_EQ(f.all_tracks().total, 5u);
    EXPECT_FALSE(f.lib->track("music/ghost.mp3"));
    EXPECT_EQ(f.lib->track("music/x/sine_16k_mono.wav")->title, "sine_16k_mono");
    auto doc = wisp::from_json(read_raw(fs::path(f.root()) / ".index.json"));
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc.value().find("version")->as_int(), 1);
}

TEST(MediaLibrary, WriteAndRemoveUpdateOneEntry) {
    Fixture f;
    f.copy_tracks();
    f.start();
    std::vector<std::string> removed;
    f.lib->set_on_removed([&](const std::string& p) { removed.push_back(p); });
    const uint64_t reads = f.lib->tag_reads();
    const uint32_t rev = f.lib->state().get().revision;

    ASSERT_TRUE(
        f.write("music/new/a.mp3", read_raw(wisp::test::media_data("sine_44k_stereo_cbr128.mp3"))));
    EXPECT_EQ(f.lib->tag_reads(), reads + 1);
    auto t = f.lib->track("music/new/a.mp3");
    ASSERT_TRUE(t);
    EXPECT_EQ(t->title, "Sine 440");
    EXPECT_EQ(f.lib->state().get().track_count, 6u);
    EXPECT_NE(f.lib->state().get().revision, rev);

    // A file that is not a track changes no entry.
    const uint32_t rev2 = f.lib->state().get().revision;
    ASSERT_TRUE(f.write("document/notes.txt", "hello"));
    EXPECT_EQ(read_raw(fs::path(f.root()) / "document/notes.txt"), "hello");
    EXPECT_EQ(f.lib->state().get().revision, rev2);
    EXPECT_EQ(f.lib->tag_reads(), reads + 1);

    ASSERT_TRUE(f.remove("music/new/a.mp3"));
    EXPECT_FALSE(f.lib->track("music/new/a.mp3"));
    EXPECT_EQ(f.lib->state().get().track_count, 5u);
    EXPECT_FALSE(on_disk(fs::path(f.root()) / "music/new/a.mp3"));
    EXPECT_EQ(removed, (std::vector<std::string>{"music/new/a.mp3"}));

    EXPECT_EQ(f.remove("music/new/a.mp3").error().code, ErrorCode::NotFound);
    EXPECT_EQ(f.remove("music/x").error().code, ErrorCode::InvalidArgument);  // not empty
    ASSERT_TRUE(f.remove("music/new"));
    EXPECT_FALSE(on_disk(fs::path(f.root()) / "music/new"));

    ASSERT_TRUE(f.mkdir("download/a/b"));
    EXPECT_TRUE(fs::is_directory(fs::path(f.root()) / "download/a/b"));
    Wait<MediaEntry> st;
    f.lib->stat("download/a", st.cb());
    auto e = st.take();
    ASSERT_TRUE(e);
    EXPECT_EQ(e.value().name, "a");
    EXPECT_TRUE(e.value().dir);
    Wait<MediaEntry> st2;
    f.lib->stat("document/notes.txt", st2.cb());
    auto e2 = st2.take();
    ASSERT_TRUE(e2);
    EXPECT_FALSE(e2.value().dir);
    EXPECT_EQ(e2.value().size, 5u);
    Wait<MediaEntry> st3;
    f.lib->stat("document/none.txt", st3.cb());
    EXPECT_EQ(st3.take().error().code, ErrorCode::NotFound);

    // The index on the card follows the single-entry changes.
    ASSERT_TRUE(f.router->drain(5000));
    f.lib.reset();
    f.start();
    EXPECT_EQ(f.lib->tag_reads(), 0u);
    EXPECT_EQ(f.all_tracks().total, 5u);
}

TEST(MediaLibrary, TracksFilterAndPage) {
    Fixture f;
    f.copy_tracks();
    // 210 more tracks with no tags: their titles are their file names.
    for (int i = 0; i < 210; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "many/t%03d.wav", i);
        write_raw(fs::path(f.root()) / "music" / name, "not a wav");
    }
    f.start();
    EXPECT_EQ(f.all_tracks({.limit = 1000}).total, 215u);
    EXPECT_EQ(f.all_tracks({.limit = 1000}).items.size(), 200u);
    EXPECT_EQ(f.all_tracks({.offset = 210, .limit = 50}).items.size(), 5u);
    EXPECT_EQ(f.all_tracks({.offset = 300}).items.size(), 0u);
    EXPECT_EQ(f.all_tracks().items.size(), 50u);

    TrackPage q = f.all_tracks({.q = "SINE 4"});
    ASSERT_EQ(q.total, 2u);  // Sine 440, Sine 48k; not sine_44k_stereo
    EXPECT_TRUE(find(q, "music/x/sine_48k_mono_vbr.mp3"));
    EXPECT_EQ(f.all_tracks({.q = "STEREO"}).total, 1u);  // a title from a file name
    EXPECT_EQ(f.all_tracks({.q = "wisp"}).total, 3u);    // artist
    EXPECT_EQ(f.all_tracks({.q = "tests"}).total, 3u);   // album
    EXPECT_EQ(f.all_tracks({.artist = "Wisp"}).total, 3u);
    EXPECT_EQ(f.all_tracks({.artist = "wisp"}).total, 0u);
    EXPECT_EQ(f.all_tracks({.q = "48", .album = "Tests"}).total, 1u);
    EXPECT_EQ(f.all_tracks({.album = "Nope"}).total, 0u);
}

TEST(MediaLibrary, ReadIsCappedAt64KiB) {
    Fixture f;
    std::string big(100 * 1024, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 7);
    write_raw(fs::path(f.root()) / "download/big.bin", big);
    f.start();

    Wait<wisp::os::LargeBuffer> a;
    f.lib->read("download/big.bin", 0, 1 << 20, a.cb());
    auto ra = a.take();
    ASSERT_TRUE(ra);
    EXPECT_EQ(ra.value().size(), wisp::media::kMaxReadBytes);
    EXPECT_EQ(std::memcmp(ra.value().data(), big.data(), ra.value().size()), 0);

    Wait<wisp::os::LargeBuffer> b;
    f.lib->read("download/big.bin", 90000, 64 * 1024, b.cb());
    auto rb = b.take();
    ASSERT_TRUE(rb);
    EXPECT_EQ(rb.value().size(), big.size() - 90000);

    Wait<wisp::os::LargeBuffer> c;
    f.lib->read("download/none.bin", 0, 10, c.cb());
    EXPECT_EQ(c.take().error().code, ErrorCode::NotFound);

    f.on_sd([&] {
        auto r = f.lib->read_now("download/big.bin", 10, 1 << 20);
        ASSERT_TRUE(r);
        EXPECT_EQ(r.value().size(), wisp::media::kMaxReadBytes);
        EXPECT_EQ(f.lib->read_now("../x", 0, 1).error().code, ErrorCode::InvalidArgument);
    });
}

TEST(MediaLibrary, WriteOutsideTheFoldersIsRejected) {
    Fixture f;
    f.start();
    for (const char* bad : {"x.bin", "other/x.bin", "music", "/music/a.mp3", "music/../x",
                            "music/.hidden", ".index.json"}) {
        EXPECT_EQ(f.write(bad, "x").error().code, ErrorCode::InvalidArgument) << bad;
        EXPECT_EQ(f.remove(bad).error().code, ErrorCode::InvalidArgument) << bad;
        EXPECT_EQ(f.mkdir(bad).error().code, ErrorCode::InvalidArgument) << bad;
    }
    f.on_sd([&] {
        EXPECT_EQ(f.lib->begin_write("x.bin").error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(f.lib->begin_write("music").error().code, ErrorCode::InvalidArgument);
    });
    EXPECT_FALSE(on_disk(fs::path(f.root()) / "x.bin"));
    EXPECT_FALSE(on_disk(fs::path(f.root()) / "other"));
    EXPECT_TRUE(fs::is_directory(fs::path(f.root()) / "music"));
}

TEST(MediaLibrary, HiddenFilesAreNotListed) {
    Fixture f;
    f.copy_tracks();
    write_raw(fs::path(f.root()) / "music/.hidden.mp3", "x");
    write_raw(fs::path(f.root()) / "music/.dot/in.mp3", "x");
    write_raw(fs::path(f.root()) / "music/y.mp3.tmp", "x");
    write_raw(fs::path(f.root()) / "music/y.mp3.new", "x");
    write_raw(fs::path(f.root()) / "music/cover.jpg", "x");
    f.start();
    EXPECT_EQ(names(f.list("")), (std::vector<std::string>{"document", "download", "music"}));
    EXPECT_EQ(names(f.list("music")), (std::vector<std::string>{"cover.jpg", "x"}));
    EXPECT_EQ(f.all_tracks().total, 5u);
    Wait<std::vector<MediaEntry>> w;
    f.lib->list("music/.dot", w.cb());
    EXPECT_EQ(w.take().error().code, ErrorCode::InvalidArgument);
}

TEST(MediaLibrary, WriterCommitsAtomically) {
    Fixture f;
    f.start();
    const fs::path target = fs::path(f.root()) / "download/f.bin";

    f.on_sd([&] {
        auto w = f.lib->begin_write("download/f.bin");
        ASSERT_TRUE(w);
        ASSERT_TRUE(w.value()->append(bytes("abc")));
        w.value()->abort();
    });
    EXPECT_FALSE(on_disk(target));
    EXPECT_TRUE(fs::is_empty(fs::path(f.root()) / "download"));

    write_raw(target, "old");
    f.on_sd([&] {
        auto w = f.lib->begin_write("download/f.bin");
        ASSERT_TRUE(w);
        std::string big(100 * 1024, 'n');
        ASSERT_TRUE(w.value()->append(bytes(big)));
        ASSERT_TRUE(w.value()->append(bytes("end")));
        EXPECT_EQ(read_raw(target), "old");
        ASSERT_TRUE(w.value()->commit());
        EXPECT_EQ(w.value()->written(), big.size() + 3);
    });
    EXPECT_EQ(read_raw(target).size(), 100u * 1024 + 3);
    EXPECT_EQ(names(f.list("download")), (std::vector<std::string>{"f.bin"}));
    {
        std::set<std::string> raw;
        for (const auto& e : fs::directory_iterator(fs::path(f.root()) / "download"))
            raw.insert(e.path().filename().string());
        EXPECT_EQ(raw, (std::set<std::string>{"f.bin"}));
    }

    // Destroyed without commit: aborted.
    f.on_sd([&] {
        auto w = f.lib->begin_write("download/g.bin");
        ASSERT_TRUE(w);
        ASSERT_TRUE(w.value()->append(bytes("abc")));
    });
    EXPECT_FALSE(on_disk(fs::path(f.root()) / "download/g.bin"));
    EXPECT_FALSE(on_disk(fs::path(f.root()) / "download/.g.bin.part"));

    // A track written through a writer is indexed on commit.
    const std::string mp3 = read_raw(wisp::test::media_data("sine_48k_mono_vbr.mp3"));
    f.on_sd([&] {
        auto w = f.lib->begin_write("music/w.mp3");
        ASSERT_TRUE(w);
        ASSERT_TRUE(w.value()->append(bytes(mp3)));
        ASSERT_TRUE(w.value()->commit());
    });
    ASSERT_TRUE(f.lib->track("music/w.mp3"));
    EXPECT_EQ(f.lib->track("music/w.mp3")->title, "Sine 48k");

    // A ".done" whose final rename was interrupted is promoted when its
    // folder is listed; a ".part" nobody writes is removed.
    write_raw(fs::path(f.root()) / "download/.h.bin.done", "complete");
    write_raw(fs::path(f.root()) / "download/.i.bin.part", "torn");
    EXPECT_EQ(names(f.list("download")), (std::vector<std::string>{"f.bin", "h.bin"}));
    EXPECT_EQ(read_raw(fs::path(f.root()) / "download/h.bin"), "complete");
    EXPECT_FALSE(on_disk(fs::path(f.root()) / "download/.i.bin.part"));
}

TEST(MediaLibrary, CardRemovedClearsTheIndex) {
    Fixture f;
    f.copy_tracks();
    f.start();
    ASSERT_EQ(f.lib->state().get().track_count, 5u);

    f.set_sd(false);
    for (int i = 0; i < 500 && f.lib->state().get().available; ++i) wisp::os::sleep_ms(10);
    MediaLibraryState s = f.lib->state().get();
    EXPECT_FALSE(s.available);
    EXPECT_EQ(s.track_count, 0u);
    EXPECT_FALSE(f.lib->track("music/x/sine_16k_mono.wav"));
    Wait<TrackPage> t;
    f.lib->tracks({}, t.cb());
    EXPECT_EQ(t.take().error().code, ErrorCode::StorageUnavailable);
    f.on_sd([&] {
        EXPECT_EQ(f.lib->begin_write("download/a").error().code, ErrorCode::StorageUnavailable);
    });

    f.set_sd(true);
    f.wait_ready();
    EXPECT_EQ(f.lib->state().get().track_count, 5u);
}

// Releases a gate when the test body exits, including by a failed ASSERT,
// so the workers can join. Declare it after the Fixture.
struct ReleaseOnExit {
    wisp::os::Semaphore& sem;
    ~ReleaseOnExit() { sem.give(); }
};

TEST(MediaLibrary, CardRemovedMidScanStopsTheScan) {
    Fixture f;
    constexpr uint64_t kFiles = 300;
    for (uint64_t i = 0; i < kFiles; ++i)
        write_raw(fs::path(f.root()) / "music" / ("t" + std::to_string(i) + ".wav"), "x");
    f.make_library();
    f.lib->start();
    wisp::os::Semaphore release(1, 0);
    ReleaseOnExit guard{release};
    const uint64_t reads = f.hold_sd_mid_scan(release);
    ASSERT_LT(reads, kFiles);
    ASSERT_TRUE(f.lib->state().get().scanning);
    // The removal is memory work on the bus; the scan's next job sees it.
    f.sd_dev->online_flag = false;
    ASSERT_TRUE(f.flash.submit([&f] { f.storage->refresh(); }));
    for (int i = 0; i < 500 && f.lib->state().get().available; ++i) wisp::os::sleep_ms(10);
    EXPECT_FALSE(f.lib->state().get().available);
    release.give();
    ASSERT_TRUE(f.router->drain(5000));
    EXPECT_LE(f.lib->tag_reads(), reads + 1);  // the step that was queued saw the removal
    MediaLibraryState s = f.lib->state().get();
    EXPECT_FALSE(s.available);
    EXPECT_FALSE(s.scanning);
    EXPECT_EQ(s.track_count, 0u);
}

TEST(MediaLibrary, StopWaitsForQueuedCalls) {
    Fixture f;
    f.copy_tracks();
    f.start();
    std::atomic<int> done{0};
    for (int i = 0; i < 10; ++i) f.lib->list("music/x", [&](auto) { done.fetch_add(1); });
    f.lib->rescan();
    f.lib->stop();
    const int after_stop = done.load();
    ASSERT_TRUE(f.router->drain(5000));
    EXPECT_EQ(done.load(), after_stop);
    EXPECT_EQ(after_stop, 10);  // queued before stop(): run, or refused at once
    Wait<std::vector<MediaEntry>> w;
    f.lib->list("music", w.cb());
    EXPECT_EQ(w.take().error().code, ErrorCode::StorageUnavailable);
    f.lib->stop();  // idempotent
}

TEST(MediaLibrary, WritesAndRemovesDuringAScanSurviveIt) {
    Fixture f;
    constexpr int kFiles = 600;
    for (int i = 0; i < kFiles; ++i)
        write_raw(fs::path(f.root()) / "music" / ("t" + std::to_string(i) + ".wav"), "x");
    f.make_library();
    f.lib->start();
    wisp::os::Semaphore release(1, 0);
    ReleaseOnExit guard{release};
    // The scan reads the folder from one end, in the order the file system
    // lists it: the middle of the folder is still waiting for its tag read.
    ASSERT_LT(f.hold_sd_mid_scan(release), static_cast<uint64_t>(kFiles / 2 - 30));
    auto name = [](int i) { return "music/t" + std::to_string(i) + ".wav"; };
    constexpr int kMid = kFiles / 2;

    // Queued behind the gate, so they run while the scan is in progress.
    std::vector<std::unique_ptr<Wait<void>>> waits;
    for (int i = kMid; i < kMid + 12; ++i) {
        waits.push_back(std::make_unique<Wait<void>>());
        f.lib->remove(name(i), waits.back()->cb());
    }
    Wait<void> wrote;
    f.lib->write("music/new.wav", buffer_of(read_raw(wisp::test::media_data("sine_16k_mono.wav"))),
                 wrote.cb());
    // Removed behind the library's back: the scan's tag read finds nothing.
    for (int i = kMid + 12; i < kMid + 24; ++i) fs::remove(fs::path(f.root()) / name(i));
    release.give();
    for (auto& w : waits) EXPECT_TRUE(w->take());
    EXPECT_TRUE(wrote.take());
    f.wait_ready();

    const uint32_t expected = kFiles - 24 + 1;
    EXPECT_EQ(f.all_tracks().total, expected);
    for (int i = kMid; i < kMid + 24; ++i) EXPECT_FALSE(f.lib->track(name(i))) << i;
    ASSERT_TRUE(f.lib->track("music/new.wav"));
    EXPECT_EQ(f.lib->track("music/new.wav")->duration_s, 1u);

    // What was saved matches the card: a reboot reads no tags.
    f.lib.reset();
    f.start();
    EXPECT_EQ(f.lib->tag_reads(), 0u);
    EXPECT_EQ(f.all_tracks().total, expected);
}

TEST(MediaLibrary, StaleDoneNeverResurrectsAFile) {
    Fixture f;
    f.start();
    const fs::path dl = fs::path(f.root()) / "download";

    // A commit cut before the old file was removed: both are there. A
    // remove() without a listing first must not leave the ".done" to come
    // back later.
    write_raw(dl / "f.bin", "old");
    write_raw(dl / ".f.bin.done", "stale");
    ASSERT_TRUE(f.remove("download/f.bin"));
    EXPECT_FALSE(on_disk(dl / ".f.bin.done"));
    EXPECT_TRUE(f.list("download").empty());
    EXPECT_FALSE(on_disk(dl / "f.bin"));

    // The same leftover is dropped by a listing: the target is kept.
    write_raw(dl / "h.bin", "old");
    write_raw(dl / ".h.bin.done", "stale");
    EXPECT_EQ(names(f.list("download")), (std::vector<std::string>{"h.bin"}));
    EXPECT_EQ(read_raw(dl / "h.bin"), "old");
    EXPECT_FALSE(on_disk(dl / ".h.bin.done"));

    // Cut after the old file was removed: stat() and read() recover it.
    write_raw(dl / ".g.bin.done", "complete");
    Wait<MediaEntry> st;
    f.lib->stat("download/g.bin", st.cb());
    auto e = st.take();
    ASSERT_TRUE(e) << e.error().message;
    EXPECT_EQ(e.value().size, 8u);
    write_raw(dl / ".r.bin.done", "recovered");
    Wait<wisp::os::LargeBuffer> rd;
    f.lib->read("download/r.bin", 0, 100, rd.cb());
    auto r = rd.take();
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(r.value().data()), r.value().size()),
              "recovered");
    EXPECT_FALSE(on_disk(dl / ".r.bin.done"));

    // remove() also takes a torn ".part" nobody writes.
    write_raw(dl / ".p.bin.part", "torn");
    EXPECT_EQ(f.remove("download/p.bin").error().code, ErrorCode::NotFound);
    EXPECT_FALSE(on_disk(dl / ".p.bin.part"));
}
