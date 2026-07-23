#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

namespace tipe {

std::string persistentSupervisionSnapshot(std::string_view snapshot);
bool writeSupervisionSnapshotAtomically(const std::filesystem::path &path, std::string_view snapshot);
bool appendBoundedSupervisionHistory(const std::filesystem::path &path, std::string_view header,
                                     std::string_view snapshot, std::uintmax_t maxBytes);
bool sanitizePersistentSupervisionFile(const std::filesystem::path &path);

} // namespace tipe
