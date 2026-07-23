#include "supervision_snapshot.h"

#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace tipe {

namespace {

bool appendPrivateFile(const std::filesystem::path &path, std::string_view content) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    if (::fchmod(descriptor, 0600) != 0) {
        ::close(descriptor);
        return false;
    }
    std::size_t written = 0;
    while (written < content.size()) {
        const auto result = ::write(descriptor, content.data() + written, content.size() - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            ::close(descriptor);
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return ::close(descriptor) == 0;
}

bool createPrivateFile(const std::filesystem::path &path) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    const bool privateMode = ::fchmod(descriptor, 0600) == 0;
    const bool closed = ::close(descriptor) == 0;
    return privateMode && closed;
}

} // namespace

std::string persistentSupervisionSnapshot(std::string_view snapshot) {
    std::string result;
    std::size_t begin = 0;
    while (begin < snapshot.size()) {
        const auto newline = snapshot.find('\n', begin);
        const auto end = newline == std::string_view::npos ? snapshot.size() : newline;
        const auto line = snapshot.substr(begin, end - begin);
        const bool ephemeral = line.starts_with("surrounding_before\t") ||
                               line.starts_with("surrounding_after\t") || line == "context" ||
                               line.starts_with("context\t");
        if (!ephemeral) {
            result.append(line);
            result.push_back('\n');
        }
        if (newline == std::string_view::npos) {
            break;
        }
        begin = newline + 1;
    }
    return result;
}

bool writeSupervisionSnapshotAtomically(const std::filesystem::path &path, std::string_view snapshot) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto temporaryPath = path;
    temporaryPath += ".tmp." + std::to_string(::getpid());
    if (!createPrivateFile(temporaryPath)) {
        std::filesystem::remove(temporaryPath, error);
        return false;
    }
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
        output.write(snapshot.data(), static_cast<std::streamsize>(snapshot.size()));
        if (!output) {
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
    }
    std::filesystem::permissions(temporaryPath,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
        std::filesystem::remove(temporaryPath, error);
        return false;
    }
    error.clear();
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::filesystem::remove(temporaryPath, error);
        return false;
    }
    return true;
}

bool appendBoundedSupervisionHistory(const std::filesystem::path &path, std::string_view header,
                                     std::string_view snapshot, std::uintmax_t maxBytes) {
    if (path.empty() || maxBytes == 0 || !header.starts_with("---\t") ||
        header.find_first_of("\r\n") != std::string_view::npos || snapshot.empty()) {
        return false;
    }

    std::string record;
    record.reserve(header.size() + snapshot.size() + 2);
    record.append(header);
    record.push_back('\n');
    record.append(snapshot);
    if (!snapshot.ends_with('\n')) {
        record.push_back('\n');
    }
    if (record.size() > maxBytes) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (!appendPrivateFile(path, record)) {
        return false;
    }

    const auto size = std::filesystem::file_size(path, error);
    if (error || size <= maxBytes) {
        return !error;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::vector<std::size_t> recordStarts;
    if (content.starts_with("---\t")) {
        recordStarts.push_back(0);
    }
    std::size_t search = 0;
    while ((search = content.find("\n---\t", search)) != std::string::npos) {
        recordStarts.push_back(search + 1);
        search += 5;
    }
    if (recordStarts.empty()) {
        return false;
    }

    const auto retainTarget = static_cast<std::size_t>(maxBytes * 3 / 4);
    std::size_t firstRetained = recordStarts.size() - 1;
    while (firstRetained > 0 && content.size() - recordStarts[firstRetained - 1] <= retainTarget) {
        --firstRetained;
    }
    const auto retained = std::string_view(content).substr(recordStarts[firstRetained]);
    return writeSupervisionSnapshotAtomically(path, retained);
}

bool sanitizePersistentSupervisionFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto sanitized = persistentSupervisionSnapshot(content);
    if (sanitized == content) {
        return true;
    }
    return writeSupervisionSnapshotAtomically(path, sanitized);
}

} // namespace tipe
