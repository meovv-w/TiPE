#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace tipe {

inline constexpr std::uintmax_t diagnosticLogMaxBytes = 4 * 1024 * 1024;
inline constexpr std::uintmax_t diagnosticLogRetainBytes = 3 * 1024 * 1024;

inline bool trimOpenDiagnosticLog(int descriptor,
                                  std::uintmax_t maxBytes = diagnosticLogMaxBytes) {
    struct stat fileStatus {};
    if (::fstat(descriptor, &fileStatus) != 0 || !S_ISREG(fileStatus.st_mode) ||
        fileStatus.st_size <= static_cast<off_t>(maxBytes)) {
        return false;
    }
    return ::ftruncate(descriptor, 0) == 0;
}

inline int openPrivateAppendFile(const std::filesystem::path &path) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return -1;
    }
    if (::fchmod(descriptor, 0600) != 0) {
        ::close(descriptor);
        return -1;
    }
    return descriptor;
}

inline bool trimDiagnosticLogFile(const std::filesystem::path &path,
                                  std::uintmax_t maxBytes = diagnosticLogMaxBytes,
                                  std::uintmax_t retainBytes = diagnosticLogRetainBytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size <= maxBytes) {
        return false;
    }

    const auto keep = std::min({retainBytes, maxBytes, size});
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    const auto offset = size - keep;
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::string tail(static_cast<std::size_t>(keep), '\0');
    input.read(tail.data(), static_cast<std::streamsize>(tail.size()));
    tail.resize(static_cast<std::size_t>(input.gcount()));
    if (offset > 0) {
        const auto firstNewline = tail.find('\n');
        tail.erase(0, firstNewline == std::string::npos ? tail.size() : firstNewline + 1);
    }

    auto temporaryPath = path;
    temporaryPath += ".trim.tmp";
    const int temporaryDescriptor =
        ::open(temporaryPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (temporaryDescriptor < 0) {
        return false;
    }
    const bool temporaryPrivate = ::fchmod(temporaryDescriptor, 0600) == 0;
    const bool temporaryClosed = ::close(temporaryDescriptor) == 0;
    if (!temporaryPrivate || !temporaryClosed) {
        std::filesystem::remove(temporaryPath, error);
        return false;
    }
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
        output.write(tail.data(), static_cast<std::streamsize>(tail.size()));
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

inline void appendBoundedDiagnosticLog(const std::filesystem::path &path, std::string_view message) {
    static std::uint64_t appendCount = 0;
    if (appendCount++ % 128 == 0) {
        trimDiagnosticLogFile(path);
    }
    const int descriptor = openPrivateAppendFile(path);
    if (descriptor < 0) {
        return;
    }
    const auto writeAll = [descriptor](std::string_view value) {
        std::size_t written = 0;
        while (written < value.size()) {
            const auto result = ::write(descriptor, value.data() + written, value.size() - written);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            written += static_cast<std::size_t>(result);
        }
        return true;
    };
    if (!writeAll(message) || ((message.empty() || message.back() != '\n') && !writeAll("\n"))) {
        ::close(descriptor);
        return;
    }
    ::close(descriptor);
}

} // namespace tipe
