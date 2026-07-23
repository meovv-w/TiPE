#include "wine_caret_bridge.h"

#include <xcb/xcb.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <optional>
#include <spawn.h>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace tipe {

namespace {

struct ActiveX11Process {
    pid_t pid = -1;
    int rootWidth = 0;
    int rootHeight = 0;
};

std::vector<std::string_view> splitTabs(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    std::vector<std::string_view> fields;
    while (true) {
        const auto delimiter = line.find('\t');
        fields.push_back(line.substr(0, delimiter));
        if (delimiter == std::string_view::npos) {
            break;
        }
        line.remove_prefix(delimiter + 1);
    }
    return fields;
}

template <typename Integer>
std::optional<Integer> parseInteger(std::string_view value) {
    Integer parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<xcb_atom_t> internAtom(xcb_connection_t *connection, const char *name) {
    const auto cookie = xcb_intern_atom(connection, 0, static_cast<std::uint16_t>(std::char_traits<char>::length(name)),
                                        name);
    auto *reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    if (!reply) {
        return std::nullopt;
    }
    const auto atom = reply->atom;
    std::free(reply);
    return atom;
}

std::optional<std::uint32_t> cardinalProperty(xcb_connection_t *connection, xcb_window_t window,
                                              xcb_atom_t property, xcb_atom_t type) {
    const auto cookie = xcb_get_property(connection, 0, window, property, type, 0, 1);
    auto *reply = xcb_get_property_reply(connection, cookie, nullptr);
    if (!reply || xcb_get_property_value_length(reply) < static_cast<int>(sizeof(std::uint32_t))) {
        std::free(reply);
        return std::nullopt;
    }
    const auto value = *static_cast<const std::uint32_t *>(xcb_get_property_value(reply));
    std::free(reply);
    return value;
}

std::optional<ActiveX11Process> activeX11Process() {
    const char *display = std::getenv("DISPLAY");
    if (!display || !*display) {
        return std::nullopt;
    }
    int screenIndex = 0;
    xcb_connection_t *connection = xcb_connect(display, &screenIndex);
    if (!connection || xcb_connection_has_error(connection)) {
        if (connection) {
            xcb_disconnect(connection);
        }
        return std::nullopt;
    }
    auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int index = 0; screen.rem && index < screenIndex; ++index) {
        xcb_screen_next(&screen);
    }
    if (!screen.rem) {
        xcb_disconnect(connection);
        return std::nullopt;
    }

    const auto activeAtom = internAtom(connection, "_NET_ACTIVE_WINDOW");
    const auto pidAtom = internAtom(connection, "_NET_WM_PID");
    std::optional<std::uint32_t> activeWindow;
    if (activeAtom) {
        activeWindow = cardinalProperty(connection, screen.data->root, *activeAtom, XCB_ATOM_WINDOW);
    }
    if (!activeWindow || *activeWindow == XCB_WINDOW_NONE) {
        auto *focus = xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr);
        if (focus) {
            activeWindow = focus->focus;
            std::free(focus);
        }
    }

    std::optional<std::uint32_t> pid;
    if (activeWindow && pidAtom) {
        pid = cardinalProperty(connection, *activeWindow, *pidAtom, XCB_ATOM_CARDINAL);
    }
    const int rootWidth = screen.data->width_in_pixels;
    const int rootHeight = screen.data->height_in_pixels;
    xcb_disconnect(connection);
    if (!pid || *pid == 0 || *pid > static_cast<std::uint32_t>(std::numeric_limits<pid_t>::max())) {
        return std::nullopt;
    }
    return ActiveX11Process{static_cast<pid_t>(*pid), rootWidth, rootHeight};
}

std::optional<std::string> winePrefixFor(pid_t pid) {
    struct stat processStatus {};
    const auto processPath = "/proc/" + std::to_string(pid);
    if (stat(processPath.c_str(), &processStatus) != 0 || processStatus.st_uid != geteuid()) {
        return std::nullopt;
    }

    std::array<char, PATH_MAX + 1> executable{};
    const auto executableSize = readlink((processPath + "/exe").c_str(), executable.data(), PATH_MAX);
    if (executableSize <= 0) {
        return std::nullopt;
    }
    const std::string_view executablePath(executable.data(), static_cast<std::size_t>(executableSize));
    if (executablePath.find("wine-preloader") == std::string_view::npos &&
        executablePath.find("/wine/") == std::string_view::npos &&
        !executablePath.ends_with("/wine") && !executablePath.ends_with("/wine64")) {
        return std::nullopt;
    }

    std::ifstream input(processPath + "/environ", std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    const std::string environment((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    constexpr std::string_view prefixKey = "WINEPREFIX=";
    std::string prefix;
    std::size_t offset = 0;
    while (offset < environment.size()) {
        const auto end = environment.find('\0', offset);
        const auto item = std::string_view(environment).substr(
            offset, (end == std::string::npos ? environment.size() : end) - offset);
        if (item.starts_with(prefixKey)) {
            prefix.assign(item.substr(prefixKey.size()));
            break;
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1;
    }
    if (prefix.empty()) {
        const char *home = std::getenv("HOME");
        if (!home || !*home) {
            return std::nullopt;
        }
        prefix = std::string(home) + "/.wine";
    }
    if (prefix.empty() || prefix.front() != '/' || prefix.find('\0') != std::string::npos ||
        !std::filesystem::is_directory(prefix)) {
        return std::nullopt;
    }
    return prefix;
}

std::vector<std::string> bridgeEnvironment(const std::string &prefix) {
    std::vector<std::string> result;
    for (char **item = environ; item && *item; ++item) {
        const std::string_view value(*item);
        if (!value.starts_with("WINEPREFIX=") && !value.starts_with("WINEDEBUG=")) {
            result.emplace_back(value);
        }
    }
    result.push_back("WINEPREFIX=" + prefix);
    result.emplace_back("WINEDEBUG=-all");
    return result;
}

void setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

} // namespace

std::optional<WineCaretBridgeReply> parseWineCaretBridgeReply(std::string_view line) {
    const auto fields = splitTabs(line);
    if ((fields.size() == 2 || fields.size() == 3) && fields[0] == "none") {
        const auto serial = parseInteger<std::uint64_t>(fields[1]);
        const auto hasImmContext = fields.size() == 3 ? parseInteger<int>(fields[2]) : std::optional<int>{0};
        if (serial && hasImmContext && (*hasImmContext == 0 || *hasImmContext == 1)) {
            return WineCaretBridgeReply{*serial, std::nullopt, *hasImmContext != 0};
        }
        return std::nullopt;
    }
    if ((fields.size() != 6 && fields.size() != 7) || fields[0] != "caret") {
        return std::nullopt;
    }
    const auto serial = parseInteger<std::uint64_t>(fields[1]);
    const auto x = parseInteger<int>(fields[2]);
    const auto y = parseInteger<int>(fields[3]);
    const auto width = parseInteger<int>(fields[4]);
    const auto height = parseInteger<int>(fields[5]);
    const auto hasImmContext = fields.size() == 7 ? parseInteger<int>(fields[6]) : std::optional<int>{0};
    if (!serial || !x || !y || !width || !height || !hasImmContext ||
        (*hasImmContext != 0 && *hasImmContext != 1) || (*x == 0 && *y == 0) || *width <= 0 || *height <= 0) {
        return std::nullopt;
    }
    return WineCaretBridgeReply{*serial, CandidateSnapshotRect{*x, *y, *width, *height}, *hasImmContext != 0};
}

WineCaretBridge::WineCaretBridge(std::string executablePath) : executablePath_(std::move(executablePath)) {}

WineCaretBridge::~WineCaretBridge() { stop(); }

bool WineCaretBridge::start(std::string prefix, pid_t targetPid) {
    stop();
    if (executablePath_.empty() || access(executablePath_.c_str(), R_OK) != 0) {
        return false;
    }

    int inputPipe[2]{-1, -1};
    int outputPipe[2]{-1, -1};
    if (pipe2(inputPipe, O_CLOEXEC) != 0 || pipe2(outputPipe, O_CLOEXEC) != 0) {
        if (inputPipe[0] >= 0) {
            close(inputPipe[0]);
            close(inputPipe[1]);
        }
        if (outputPipe[0] >= 0) {
            close(outputPipe[0]);
            close(outputPipe[1]);
        }
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, inputPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, inputPipe[1]);
    posix_spawn_file_actions_addclose(&actions, outputPipe[0]);

    auto environmentStrings = bridgeEnvironment(prefix);
    std::vector<char *> environment;
    environment.reserve(environmentStrings.size() + 1);
    for (auto &value : environmentStrings) {
        environment.push_back(value.data());
    }
    environment.push_back(nullptr);
    std::array<char *, 3> arguments{const_cast<char *>("wine"), executablePath_.data(), nullptr};
    pid_t child = -1;
    const int spawnResult =
        posix_spawnp(&child, "wine", &actions, nullptr, arguments.data(), environment.data());
    posix_spawn_file_actions_destroy(&actions);
    close(inputPipe[0]);
    close(outputPipe[1]);
    if (spawnResult != 0) {
        close(inputPipe[1]);
        close(outputPipe[0]);
        return false;
    }

    setNonBlocking(inputPipe[1]);
    setNonBlocking(outputPipe[0]);
    inputFd_ = inputPipe[1];
    outputFd_ = outputPipe[0];
    childPid_ = child;
    targetPid_ = targetPid;
    prefix_ = std::move(prefix);
    return true;
}

bool WineCaretBridge::writeRequest(std::uint64_t serial) {
    if (inputFd_ < 0) {
        return false;
    }
    const auto request = std::to_string(serial) + "\n";
    std::size_t offset = 0;
    while (offset < request.size()) {
        const auto written = write(inputFd_, request.data() + offset, request.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

WineCaretBridgeRequest WineCaretBridge::request(std::uint64_t serial) {
    const auto active = activeX11Process();
    if (!active) {
        stop();
        return {};
    }
    const auto prefix = active->pid == targetPid_ && !prefix_.empty()
                            ? std::optional<std::string>(prefix_)
                            : winePrefixFor(active->pid);
    if (!prefix) {
        stop();
        return {};
    }
    if (childPid_ <= 0 || inputFd_ < 0 || outputFd_ < 0 || prefix_ != *prefix || targetPid_ != active->pid) {
        if (!start(*prefix, active->pid)) {
            return {false, active->pid, active->rootWidth, active->rootHeight, -1};
        }
    }
    if (!writeRequest(serial)) {
        stop();
        return {false, active->pid, active->rootWidth, active->rootHeight, -1};
    }
    return {true, active->pid, active->rootWidth, active->rootHeight, outputFd_};
}

void WineCaretBridge::stop() {
    if (inputFd_ >= 0) {
        close(inputFd_);
        inputFd_ = -1;
    }
    if (outputFd_ >= 0) {
        close(outputFd_);
        outputFd_ = -1;
    }
    if (childPid_ > 0) {
        int status = 0;
        if (waitpid(childPid_, &status, WNOHANG) == 0) {
            kill(childPid_, SIGTERM);
            for (int attempt = 0; attempt < 10 && waitpid(childPid_, &status, WNOHANG) == 0; ++attempt) {
                usleep(1000);
            }
            if (waitpid(childPid_, &status, WNOHANG) == 0) {
                kill(childPid_, SIGKILL);
                while (waitpid(childPid_, &status, 0) < 0 && errno == EINTR) {
                }
            }
        }
    }
    childPid_ = -1;
    targetPid_ = -1;
    prefix_.clear();
}

} // namespace tipe
