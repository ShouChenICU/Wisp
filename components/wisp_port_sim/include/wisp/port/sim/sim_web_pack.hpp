#pragma once

// Builds a WispPack in memory from a directory, for the simulator's
// --web-dir option: edit a file, restart wisp_sim, and the change is
// served without re-running tools/pack_assets.py. Follows that tool's
// rules (same MIME table, same set of gzipped suffixes, paths sorted,
// 4-byte aligned data, sha256 over the stored bytes), so net::AssetPack
// serves it exactly like the device's partition. The gzip bytes
// themselves may differ from Python's, which only changes ETags. The
// import-resolution, partition-size and budget checks are left to the
// real packer.

#include <cstdint>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"

namespace wisp::port::sim {

// NotFound when `dir` is not a directory; IoError when a file cannot be
// read or compressed. An empty directory gives a valid, empty pack.
Result<std::vector<uint8_t>> pack_web_dir(const std::string& dir);

}  // namespace wisp::port::sim
