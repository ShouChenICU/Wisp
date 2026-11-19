#pragma once
// ItemReader: the media::Source the player decodes from (media spec 4.4).
// It resolves one queue item to the storage that holds it and keeps a
// prefetch ring of that file's bytes filled:
//   "music/..." (any library path)  MediaLibrary::stat() to open, then
//                                   MediaLibrary::read_now() jobs on the
//                                   SD I/O worker (StorageRouter,
//                                   Medium::Sd);
//   "scene:<id>/<name>"             SceneFiles::stat() and read() jobs
//                                   through SceneFiles::submit(), which
//                                   routes them by the scene's medium.
// The thread that reads (wisp.media) never touches a file itself
// (AGENTS.md section 5): every file access is a job on a storage worker,
// and the reader waits for its result.
//
// The ring (`ring_bytes`, Mem::Large) is refilled `read_bytes` at a time on
// the Interactive lane whenever at most half of it is left unread; one
// refill is in flight at a time. A seek inside the bytes the ring holds
// costs nothing (decoders seek back to the start after probing); any other
// seek drops the ring and refills from the new offset.
//
// The open's size lookup goes through MediaLibrary::stat() (the library's
// Normal lane) or a SceneFiles job; only the refills use the Interactive
// lane, which on wisp.sd.io holds one job, so a refill is never queued
// behind a scan's backlog.
//
// Failure never hangs the reader:
//   - a job that fails ends the read with its error (StorageUnavailable
//     once the card is gone);
//   - a job refused (a full lane, the SD I/O worker draining for shutdown)
//     is retried for up to kRefusedGiveUpMs, then the read fails Busy;
//   - a job destroyed without running is queued again at the next poll;
//   - `abort` (polled every kPollMs while waiting) ends a wait with
//     ErrorCode::Timeout ("aborted"), so the player's shutdown joins
//     promptly whatever the storage workers do.
// A job that runs after its reader is gone does no file work: the reader
// marks the shared state closed and waits only for a job already in the
// middle of its read, so neither the library nor SceneFiles is touched
// once the destructor has returned.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/media/decoder.hpp"

namespace wisp::media {

class MediaLibrary;

// The item reference prefix of a scene's private file (media spec 4.2).
inline constexpr std::string_view kSceneItemPrefix = "scene:";

// "scene:<id>/<name>" split into its id and name; false for anything else
// (a library path) and for a malformed reference.
bool split_scene_item(std::string_view item, std::string_view& scene, std::string_view& name);

struct ItemReaderDeps {
    MediaLibrary* library = nullptr;          // null: library items fail
    core::SceneFiles* scene_files = nullptr;  // null: scene items fail
    core::StorageRouter* router = nullptr;    // library jobs; null: they fail
    size_t ring_bytes = 64 * 1024;
    size_t read_bytes = 16 * 1024;
    // Polled while waiting: true gives up (see the file header).
    std::function<bool()> abort;
};

class ItemReader : public Source {
public:
    // How long refused submissions are retried before a read fails.
    static constexpr uint32_t kRefusedGiveUpMs = 1000;
    static constexpr uint32_t kPollMs = 10;

    // Looks the item's file up (its size) and makes the ring. NotFound for
    // a missing file, InvalidArgument for a malformed item, the storage's
    // error otherwise; Timeout when `abort` ended the wait.
    static Result<std::unique_ptr<ItemReader>> open(const std::string& item,
                                                    const ItemReaderDeps& deps);

    ~ItemReader() override;
    ItemReader(const ItemReader&) = delete;
    ItemReader& operator=(const ItemReader&) = delete;

    Result<size_t> read(uint8_t* dst, size_t len) override;
    Result<void> seek(uint64_t offset) override;
    uint64_t size() const override { return size_; }

    // Shared with the jobs in flight (item_reader.cpp).
    struct Shared;

private:
    ItemReader(const ItemReaderDeps& deps, std::shared_ptr<Shared> shared, uint64_t size);
    // Queues the next refill when one is due (none in flight, at most half
    // the ring unread, not at the end). Outside the shared lock.
    void maybe_refill();

    ItemReaderDeps deps_;
    std::shared_ptr<Shared> shared_;
    uint64_t size_;
};

}  // namespace wisp::media
