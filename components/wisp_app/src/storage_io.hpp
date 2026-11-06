#pragma once

// App's side of the single flash thread (spec 3.5, 3.7 and 3.8 of
// docs/superpowers/specs/2026-11-06-flash-thread-design.md). The services
// reach the storage worker through App::Impl::submit_io()/run_io(); this
// header holds what else needs App itself.

namespace wisp::app {

class App;

// Queues StorageManager::refresh() (the SD card hot-plug probe) as a Normal
// job on the storage worker; without one, runs it here. False when skipped:
// the previous refresh has not finished yet, or the queue is full. The 2 s
// "wisp.storage" timer calls it. Any thread.
bool queue_storage_refresh(App& app);

}  // namespace wisp::app
