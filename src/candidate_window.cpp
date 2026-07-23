#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <xcb/xcb.h>

#include "candidate_layout.h"
#include "candidate_render.h"
#include "bounded_log.h"
#include "wine_caret_bridge.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

struct WindowCursorRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct X11PointerSnapshot {
    WindowCursorRect rect;
    int rootWidth = 0;
    int rootHeight = 0;
};

struct CandidateWindowData {
    std::string preedit = "nihao";
    std::vector<std::string> candidates{"你好", "你号", "拟好", "倪浩", "泥豪"};
    bool expanded = false;
    bool stdinMode = false;
    bool selfTest = false;
    bool argumentError = false;
    bool parseSnapshot = false;
    bool statusMode = false;
    bool fixedStatus = false;
    bool supervision = false;
    bool continuous = false;
    bool staticCursorRect = false;
    bool pointerFallback = false;
    bool cursorRectResolved = false;
    int eventsFd = -1;
    int ttlMs = -1;
    int supervisedKeys = 0;
    int selections = 0;
    int reranks = 0;
    int snapshotSerial = 0;
    std::string snapshotLine;
    std::string statusText;
    std::optional<GdkRectangle> layoutGeometry;
    std::size_t selectedIndex = 0;
    std::size_t preeditCursor = 5;
    int cursorX = -1;
    int cursorY = -1;
    int cursorWidth = 0;
    int cursorHeight = 0;
    std::string lastFallbackPositionKey;
    std::optional<X11PointerSnapshot> pointerFallbackAnchor;
    std::optional<X11PointerSnapshot> wineCaretAnchor;
    bool wineHasImmContext = false;
    std::shared_ptr<tipe::WineCaretBridge> wineCaretBridge;
    guint wineCaretWatchId = 0;
    int wineCaretOutputFd = -1;
    pid_t wineCaretTargetPid = -1;
    std::uint64_t wineCaretRequestSerial = 0;
    std::uint64_t latestWineCaretRequest = 0;
    std::uint64_t pendingWineCaretRender = 0;
    guint wineCaretRenderTimeoutId = 0;
    int wineCaretRootWidth = 0;
    int wineCaretRootHeight = 0;
    std::vector<tipe::CandidateHitRegion> candidateHitRegions;
    std::optional<std::size_t> hoveredCandidateIndex;
    GtkWidget *window = nullptr;
    GtkWidget *panel = nullptr;
    GtkWidget *inlinePreeditWindow = nullptr;
    GtkWidget *inlinePreeditPanel = nullptr;
    int inlinePreeditWidth = 1;
    int inlinePreeditHeight = 1;
    int inlinePreeditFontSize = 15;
};

std::vector<std::string> split(std::string_view input, char delimiter) {
    std::vector<std::string> result;
    std::string current;
    for (const char ch : input) {
        if (ch == delimiter) {
            result.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    result.push_back(current);
    return result;
}

bool parseCursorRect(std::string_view input, int &x, int &y, int &width, int &height) {
    const auto fields = split(input, ',');
    if (fields.size() != 4) {
        return false;
    }

    int values[4]{};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        char *end = nullptr;
        const long parsed = std::strtol(fields[index].c_str(), &end, 10);
        if (end == fields[index].c_str() || *end != '\0') {
            return false;
        }
        values[index] = static_cast<int>(parsed);
    }
    x = values[0];
    y = values[1];
    width = values[2];
    height = values[3];
    return true;
}

std::optional<int> parseInt(std::string_view input) {
    std::string value(input);
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::vector<std::string> splitCandidates(std::string_view input) {
    auto values = split(input, ',');
    values.erase(std::remove(values.begin(), values.end(), ""), values.end());
    return values;
}

std::vector<std::string> splitEscapedCandidates(std::string_view input) {
    std::vector<std::string> result;
    std::string current;
    bool escaped = false;
    for (const char ch : input) {
        if (escaped) {
            switch (ch) {
            case 't':
                current.push_back('\t');
                break;
            case 'n':
                current.push_back('\n');
                break;
            case 'r':
                current.push_back('\r');
                break;
            case '|':
            case '\\':
                current.push_back(ch);
                break;
            default:
                current.push_back(ch);
                break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '|') {
            result.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (escaped) {
        current.push_back('\\');
    }
    result.push_back(current);
    result.erase(std::remove(result.begin(), result.end(), ""), result.end());
    return result;
}

bool applySnapshotLine(CandidateWindowData &data, std::string_view line) {
    if (!line.empty() && line.back() == '\n') {
        line.remove_suffix(1);
    }
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }

    const auto fields = split(line, '\t');
    if (fields.size() != 3 && fields.size() != 7 && fields.size() != 8 && fields.size() != 9) {
        return false;
    }

    const auto parsedPreedit = fields[0];
    const bool parsedExpanded = fields[1] == "1";
    std::size_t parsedSelectedIndex = 0;
    int parsedCursorX = -1;
    int parsedCursorY = -1;
    int parsedCursorWidth = 0;
    int parsedCursorHeight = 0;
    std::size_t candidateField = 2;
    if (fields.size() == 8 || fields.size() == 9) {
        const auto selectedIndex = parseInt(fields[2]);
        const auto cursorX = parseInt(fields[3]);
        const auto cursorY = parseInt(fields[4]);
        const auto cursorWidth = parseInt(fields[5]);
        const auto cursorHeight = parseInt(fields[6]);
        if (!selectedIndex || !cursorX || !cursorY || !cursorWidth || !cursorHeight) {
            return false;
        }
        parsedSelectedIndex = static_cast<std::size_t>(std::max(0, *selectedIndex));
        parsedCursorX = *cursorX;
        parsedCursorY = *cursorY;
        parsedCursorWidth = *cursorWidth;
        parsedCursorHeight = *cursorHeight;
        candidateField = 7;
    } else if (fields.size() == 7) {
        const auto cursorX = parseInt(fields[2]);
        const auto cursorY = parseInt(fields[3]);
        const auto cursorWidth = parseInt(fields[4]);
        const auto cursorHeight = parseInt(fields[5]);
        if (!cursorX || !cursorY || !cursorWidth || !cursorHeight) {
            return false;
        }
        parsedCursorX = *cursorX;
        parsedCursorY = *cursorY;
        parsedCursorWidth = *cursorWidth;
        parsedCursorHeight = *cursorHeight;
        candidateField = 6;
    }

    auto parsedCandidates = splitEscapedCandidates(fields[candidateField]);
    if (parsedCandidates.empty() || parsedSelectedIndex >= parsedCandidates.size()) {
        parsedSelectedIndex = 0;
    }
    bool parsedSupervision = false;
    bool parsedContinuous = false;
    bool parsedStaticCursorRect = false;
    bool parsedPointerFallback = false;
    int parsedSupervisedKeys = 0;
    int parsedSelections = 0;
    int parsedReranks = 0;
    int parsedSnapshotSerial = 0;
    bool parsedMetadata = false;
    std::size_t parsedPreeditCursor = parsedPreedit.size();
    if (fields.size() == 9) {
        for (const auto &token : split(fields[8], ',')) {
            const auto delimiter = token.find('=');
            if (delimiter == std::string::npos) {
                continue;
            }
            const auto key = std::string_view(token).substr(0, delimiter);
            const auto value = parseInt(std::string_view(token).substr(delimiter + 1));
            if (!value) {
                continue;
            }
            if (key == "supervision") {
                parsedSupervision = *value != 0;
                parsedMetadata = true;
            } else if (key == "continuous") {
                parsedContinuous = *value != 0;
                parsedMetadata = true;
            } else if (key == "keys") {
                parsedSupervisedKeys = std::max(0, *value);
                parsedMetadata = true;
            } else if (key == "selects") {
                parsedSelections = std::max(0, *value);
                parsedMetadata = true;
            } else if (key == "reranks") {
                parsedReranks = std::max(0, *value);
                parsedMetadata = true;
            } else if (key == "preedit_cursor") {
                parsedPreeditCursor = static_cast<std::size_t>(
                    std::clamp(*value, 0, static_cast<int>(parsedPreedit.size())));
                parsedMetadata = true;
            } else if (key == "cursor_static") {
                parsedStaticCursorRect = *value != 0;
                parsedMetadata = true;
            } else if (key == "snapshot") {
                parsedSnapshotSerial = std::max(0, *value);
                parsedMetadata = true;
            } else if (key == "pointer_fallback") {
                parsedPointerFallback = *value != 0;
                parsedMetadata = true;
            }
        }
        if (!parsedMetadata) {
            return false;
        }
    }
    data.preedit = parsedPreedit;
    data.expanded = parsedExpanded;
    data.selectedIndex = parsedSelectedIndex;
    data.preeditCursor = parsedPreeditCursor;
    data.cursorX = parsedCursorX;
    data.cursorY = parsedCursorY;
    data.cursorWidth = parsedCursorWidth;
    data.cursorHeight = parsedCursorHeight;
    data.candidates = std::move(parsedCandidates);
    data.supervision = parsedSupervision;
    data.continuous = parsedContinuous;
    data.staticCursorRect = parsedStaticCursorRect;
    data.pointerFallback = parsedPointerFallback;
    if (parsedPreedit.empty() || !parsedPointerFallback) {
        data.pointerFallbackAnchor.reset();
        data.wineCaretAnchor.reset();
        data.wineHasImmContext = false;
    }
    data.cursorRectResolved = false;
    data.supervisedKeys = parsedSupervisedKeys;
    data.selections = parsedSelections;
    data.reranks = parsedReranks;
    data.snapshotSerial = parsedSnapshotSerial;
    return true;
}

CandidateWindowData parseArgs(int argc, char **argv) {
    CandidateWindowData data;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--stdin") {
            data.stdinMode = true;
            data.preedit.clear();
            data.preeditCursor = 0;
            data.candidates.clear();
        } else if (arg == "--self-test") {
            data.selfTest = true;
        } else if (arg == "--parse-snapshot") {
            if (index + 1 >= argc) {
                std::cerr << "--parse-snapshot requires SNAPSHOT or -\n";
                data.argumentError = true;
                continue;
            }
            data.parseSnapshot = true;
            data.snapshotLine = argv[++index];
        } else if (arg == "--snapshot") {
            if (index + 1 >= argc) {
                std::cerr << "--snapshot requires SNAPSHOT\n";
                data.argumentError = true;
                continue;
            }
            if (!applySnapshotLine(data, argv[++index])) {
                std::cerr << "invalid --snapshot value\n";
                data.argumentError = true;
            }
        } else if (arg == "--ttl-ms") {
            if (index + 1 >= argc) {
                std::cerr << "--ttl-ms requires MILLISECONDS\n";
                data.argumentError = true;
                continue;
            }
            const auto ttl = parseInt(argv[++index]);
            if (!ttl || *ttl < 100 || *ttl > 10000) {
                std::cerr << "invalid --ttl-ms value, expected 100..10000\n";
                data.argumentError = true;
            } else {
                data.ttlMs = *ttl;
            }
        } else if (arg == "--layout-geometry") {
            if (index + 1 >= argc) {
                std::cerr << "--layout-geometry requires x,y,width,height\n";
                data.argumentError = true;
                continue;
            }
            GdkRectangle geometry{};
            if (!parseCursorRect(argv[++index], geometry.x, geometry.y, geometry.width, geometry.height) ||
                geometry.width <= 0 || geometry.height <= 0) {
                std::cerr << "invalid --layout-geometry value, expected x,y,width,height\n";
                data.argumentError = true;
            } else {
                data.layoutGeometry = geometry;
            }
        } else if (arg == "--expanded") {
            data.expanded = true;
        } else if (arg == "--status") {
            if (index + 1 >= argc) {
                std::cerr << "--status requires TEXT\n";
                data.argumentError = true;
                continue;
            }
            data.statusMode = true;
            data.statusText = argv[++index];
            data.preedit.clear();
            data.preeditCursor = 0;
            data.candidates.clear();
        } else if (arg == "--fixed-status") {
            data.fixedStatus = true;
        } else if (arg == "--selected") {
            if (index + 1 >= argc) {
                std::cerr << "--selected requires INDEX\n";
                data.argumentError = true;
                continue;
            }
            const auto selected = parseInt(argv[++index]);
            if (!selected) {
                std::cerr << "invalid --selected value, expected index\n";
                data.argumentError = true;
            } else {
                data.selectedIndex = static_cast<std::size_t>(std::max(0, *selected));
            }
        } else if (arg == "--preedit") {
            if (index + 1 >= argc) {
                std::cerr << "--preedit requires TEXT\n";
                data.argumentError = true;
                continue;
            }
            data.preedit = argv[++index];
            data.preeditCursor = data.preedit.size();
        } else if (arg == "--candidates") {
            if (index + 1 >= argc) {
                std::cerr << "--candidates requires comma-separated candidates\n";
                data.argumentError = true;
                continue;
            }
            auto candidates = splitCandidates(argv[++index]);
            if (!candidates.empty()) {
                data.candidates = std::move(candidates);
            }
        } else if (arg == "--cursor") {
            if (index + 1 >= argc) {
                std::cerr << "--cursor requires x,y,width,height\n";
                data.argumentError = true;
                continue;
            }
            const std::string_view rect = argv[++index];
            if (!parseCursorRect(rect, data.cursorX, data.cursorY, data.cursorWidth, data.cursorHeight)) {
                std::cerr << "invalid --cursor value, expected x,y,width,height\n";
                data.argumentError = true;
            }
        } else if (arg == "--events-fd") {
            if (index + 1 >= argc) {
                std::cerr << "--events-fd requires FD\n";
                data.argumentError = true;
                continue;
            }
            const auto fd = parseInt(argv[++index]);
            if (!fd || *fd < 0) {
                std::cerr << "invalid --events-fd value\n";
                data.argumentError = true;
            } else {
                data.eventsFd = *fd;
            }
        }
    }
    return data;
}

std::vector<char *> gtkArgs(int argc, char **argv) {
    std::vector<char *> args;
    args.push_back(argv[0]);
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--stdin" || arg == "--expanded" || arg == "--self-test" || arg == "--fixed-status") {
            continue;
        }
        if ((arg == "--preedit" || arg == "--candidates" || arg == "--selected" || arg == "--cursor" ||
             arg == "--parse-snapshot" || arg == "--snapshot" || arg == "--ttl-ms" ||
             arg == "--layout-geometry" || arg == "--status" || arg == "--events-fd") &&
            index + 1 < argc) {
            ++index;
            continue;
        }
        args.push_back(argv[index]);
    }
    return args;
}

constexpr std::size_t maxExpandedRows = 5;
constexpr int expandedMinWindowWidth = tipe::tipeUIPanelMaxWidth;
constexpr int expandedMaxWindowWidth = 1180;
constexpr int collapsedMinWindowWidth = tipe::tipeUIPanelMinWidth;
constexpr int collapsedMaxWindowWidth = 1040;
constexpr int layerWindowHorizontalGuard = 24;
constexpr int layerWindowTopGuard = 24;
constexpr int layerWindowBottomGuard = 72;
constexpr int layerWindowFallbackBottom = 156;

struct WindowPosition {
    int left = 0;
    int top = 0;
};

struct MonitorLayout {
    GdkRectangle geometry{};
    double scale = 1.0;
};

bool hasUsableCursorRect(const CandidateWindowData &data);
WindowPosition computeWindowPosition(const CandidateWindowData &data, const GdkRectangle &geometry);
WindowPosition computeCursorAnchoredWindowPosition(int windowWidth, int windowHeight, int cursorX, int cursorY,
                                                   int cursorWidth, int cursorHeight, const GdkRectangle &geometry);
WindowPosition computeFixedWindowPosition(int windowWidth, int windowHeight, const GdkRectangle &geometry,
                                          int defaultLeft, int defaultTop, const char *leftEnv,
                                          const char *topEnv);
std::vector<tipe::VisualCandidateCell> expandedCellsFor(const CandidateWindowData &data);
std::vector<tipe::VisualCandidateCell> collapsedCellsFor(const CandidateWindowData &data);
std::vector<tipe::VisualCandidateCell> visibleCellsFor(const CandidateWindowData &data);
void logFallbackPosition(const char *mode, CandidateWindowData &data, const GdkRectangle &geometry,
                         const WindowPosition &position, int windowWidth, int windowHeight);
WindowCursorRect normalizedCursorRect(WindowCursorRect rect, const std::vector<MonitorLayout> &monitors);
void resolveCursorRect(CandidateWindowData &data, const std::vector<MonitorLayout> &monitors);

std::optional<X11PointerSnapshot> queryX11PointerCursorRect() {
    const char *displayName = std::getenv("DISPLAY");
    if (!displayName || !*displayName) {
        return std::nullopt;
    }
    int screenIndex = 0;
    xcb_connection_t *connection = xcb_connect(displayName, &screenIndex);
    if (!connection || xcb_connection_has_error(connection)) {
        if (connection) {
            xcb_disconnect(connection);
        }
        return std::nullopt;
    }
    const auto *setup = xcb_get_setup(connection);
    auto screen = xcb_setup_roots_iterator(setup);
    for (int index = 0; screen.rem && index < screenIndex; ++index) {
        xcb_screen_next(&screen);
    }
    if (!screen.rem) {
        xcb_disconnect(connection);
        return std::nullopt;
    }
    const auto cookie = xcb_query_pointer(connection, screen.data->root);
    auto *reply = xcb_query_pointer_reply(connection, cookie, nullptr);
    if (!reply || !reply->same_screen) {
        std::free(reply);
        xcb_disconnect(connection);
        return std::nullopt;
    }
    X11PointerSnapshot result{{reply->root_x, reply->root_y, 1, 22}, screen.data->width_in_pixels,
                              screen.data->height_in_pixels};
    std::free(reply);
    xcb_disconnect(connection);
    return result;
}

tipe::TipeUIPanelMetrics panelMetricsFor(const CandidateWindowData &data) {
    return tipe::tipeUIPanelMetricsFor(visibleCellsFor(data), data.candidates, data.expanded, !data.preedit.empty());
}

int estimatedWindowWidth(const CandidateWindowData &data) {
    return panelMetricsFor(data).width;
}

int effectiveWindowWidth(const CandidateWindowData &data, const GdkRectangle &geometry) {
    return std::min(estimatedWindowWidth(data), std::max(96, geometry.width - 2 * layerWindowHorizontalGuard));
}

int estimatedWindowHeight(const CandidateWindowData &data) {
    return panelMetricsFor(data).height;
}

int positioningWindowHeight(const CandidateWindowData &data) {
    return estimatedWindowHeight(data);
}

int positioningStatusHeight() {
    return tipe::tipeUIStatusPopupHeight;
}

int requestedWindowHeight(const CandidateWindowData &data) {
    return estimatedWindowHeight(data);
}

int requestedStatusHeight() {
    return tipe::tipeUIStatusPopupHeight;
}

void setLayerWindowSize(CandidateWindowData &data, int width, int height) {
    gtk_window_set_default_size(GTK_WINDOW(data.window), width, height);
    gtk_widget_set_size_request(data.window, width, height);
    if (data.panel) {
        gtk_widget_set_size_request(data.panel, width, height);
    }
}

std::vector<tipe::VisualCandidateCell> expandedCellsFor(const CandidateWindowData &data) {
    if (!data.expanded || data.candidates.empty()) {
        return {};
    }
    return tipe::visibleVisualCellsFor(data.candidates, data.selectedIndex, true, maxExpandedRows);
}

std::vector<tipe::VisualCandidateCell> collapsedCellsFor(const CandidateWindowData &data) {
    if (data.expanded || data.candidates.empty()) {
        return {};
    }
    return tipe::visibleVisualCellsFor(data.candidates, data.selectedIndex, false);
}

std::vector<tipe::VisualCandidateCell> visibleCellsFor(const CandidateWindowData &data) {
    return data.expanded ? expandedCellsFor(data) : collapsedCellsFor(data);
}

bool printParsedSnapshot(std::string_view snapshotLine, std::optional<GdkRectangle> layoutGeometry) {
    CandidateWindowData data;
    if (!applySnapshotLine(data, snapshotLine)) {
        std::cerr << "invalid snapshot\n";
        return false;
    }

    std::cout << "preedit\t" << data.preedit << '\n';
    std::cout << "preedit-cursor\t" << data.preeditCursor << '\n';
    std::cout << "expanded\t" << (data.expanded ? 1 : 0) << '\n';
    std::cout << "selected\t" << data.selectedIndex << '\n';
    std::cout << "cursor\t" << data.cursorX << '\t' << data.cursorY << '\t' << data.cursorWidth << '\t'
              << data.cursorHeight << '\n';
    const auto width = estimatedWindowWidth(data);
    const auto height = estimatedWindowHeight(data);
    std::cout << "layout\t" << width << '\t' << height << '\t' << (hasUsableCursorRect(data) ? 1 : 0) << '\n';
    std::cout << "edge-fallback\t"
              << (tipe::tipeUIPopupEdgeFallbackNeededForRect(data.cursorX, data.cursorY, data.cursorWidth,
                                                             data.cursorHeight,
                                                             tipe::tipeUIPopupEdgeFallbackLeftThreshold,
                                                             tipe::tipeUIPopupEdgeFallbackTopThreshold, width, height)
                      ? 1
                      : 0)
              << '\n';
    if (data.supervision) {
        std::cout << "supervision\t" << data.supervisedKeys << '\t' << data.selections << '\t' << data.reranks
                  << '\n';
    }
    if (data.continuous) {
        std::cout << "continuous\t1\n";
    }
    if (layoutGeometry) {
        const auto position = computeWindowPosition(data, *layoutGeometry);
        const auto globalLeft = layoutGeometry->x + position.left;
        const auto globalTop = layoutGeometry->y + position.top;
        const auto effectiveWidth = effectiveWindowWidth(data, *layoutGeometry);
        const auto effectiveHeight = positioningWindowHeight(data);
        std::cout << "position\t" << globalLeft << '\t' << globalTop << '\t' << (globalLeft + effectiveWidth) << '\t'
                  << (globalTop + effectiveHeight) << '\n';
    }
    const auto cells = data.expanded ? expandedCellsFor(data) : collapsedCellsFor(data);
    const auto selectedRow = data.expanded ? tipe::selectedVisualRow(cells, data.selectedIndex) : std::size_t{0};
    const auto diagnosticWidth = layoutGeometry ? effectiveWindowWidth(data, *layoutGeometry) : width;
    const auto drawCells = tipe::tipeUIDrawCellsFor(cells, data.candidates, diagnosticWidth, data.expanded);
    for (std::size_t index = 0; index < data.candidates.size(); ++index) {
        const auto cell = std::find_if(cells.begin(), cells.end(),
                                       [index](const auto &candidateCell) {
                                           return candidateCell.index == index;
                                       });
        const bool visible = cell != cells.end();
        const auto shortcut =
            cell == cells.end() ? std::string{} : tipe::shortcutForVisualCell(cells, *cell, selectedRow, data.expanded);
        std::cout << "candidate\t" << index << '\t' << (index == data.selectedIndex ? 1 : 0) << '\t'
                  << (visible ? 1 : 0) << '\t' << shortcut << '\t' << data.candidates[index] << '\n';
        if (cell != cells.end()) {
            std::cout << "cell\t" << index << '\t' << cell->row << '\t' << cell->column << '\t' << cell->span << '\n';
            const auto drawCell = std::find_if(drawCells.begin(), drawCells.end(), [index](const auto &candidateCell) {
                return candidateCell.cell.index == index;
            });
            const int drawLeft = drawCell == drawCells.end() ? tipe::tipeUIPanelHorizontalPadding : drawCell->x;
            const int drawWidth = drawCell == drawCells.end() ? 0 : drawCell->width;
            const auto drawTop = tipe::tipeUIPanelTopPadding +
                                 (!data.preedit.empty()
                                      ? tipe::tipeUIPanelPreeditHeight + tipe::tipeUIPanelPreeditDividerHeight
                                      : 0) +
                                 static_cast<int>(cell->row) * tipe::tipeUIPanelRowHeight;
            std::cout << "draw-cell\t" << index << '\t' << drawLeft << '\t' << drawTop << '\t'
                      << (drawLeft + drawWidth) << '\t' << (drawTop + tipe::tipeUIPanelRowHeight) << '\t'
                      << (drawLeft >= tipe::tipeUIPanelHorizontalPadding &&
                                  drawLeft + drawWidth <= diagnosticWidth - tipe::tipeUIPanelHorizontalPadding
                              ? 1
                              : 0)
                      << '\n';
        }
    }
    return true;
}

void installCss() {
    auto *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, R"CSS(
        window {
            background: transparent;
        }
    )CSS");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

std::optional<int> envInt(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return std::nullopt;
    }
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

bool debugEnabled() {
    const char *value = std::getenv("TIPE_CANDIDATE_DEBUG");
    return value && std::string_view(value) == "1";
}

std::string fallbackPidPath(const CandidateWindowData &data) {
    if (const char *xdgCacheHome = std::getenv("XDG_CACHE_HOME"); xdgCacheHome && *xdgCacheHome) {
        return std::string(xdgCacheHome) + "/tipe/" + (data.statusMode ? "status-window.pid" : "candidate-window.pid");
    }
    const char *home = std::getenv("HOME");
    if (!home || !*home) {
        return {};
    }
    return std::string(home) + "/.cache/tipe/" + (data.statusMode ? "status-window.pid" : "candidate-window.pid");
}

bool processCommandContains(pid_t pid, std::string_view marker) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!input) {
        return false;
    }
    std::string command((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::replace(command.begin(), command.end(), '\0', ' ');
    return command.find("tipe-candidate-window") != std::string::npos &&
           command.find(marker) != std::string::npos;
}

void replaceExistingFallbackInstance(const CandidateWindowData &data) {
    if (data.stdinMode || data.parseSnapshot || data.selfTest) {
        return;
    }
    if (!data.statusMode && data.ttlMs <= 0) {
        return;
    }

    const auto path = fallbackPidPath(data);
    if (path.empty()) {
        return;
    }

    std::ifstream previousFile(path);
    long previousPid = -1;
    previousFile >> previousPid;
    const auto currentPid = static_cast<long>(getpid());
    const std::string_view marker = data.statusMode ? "--status" : "--snapshot";
    if (previousPid > 0 && previousPid != currentPid &&
        processCommandContains(static_cast<pid_t>(previousPid), marker)) {
        if (debugEnabled()) {
            std::cerr << "fallback\treplace\tmode=" << (data.statusMode ? "status" : "candidate")
                      << "\tpreviousPid=" << previousPid << "\tcurrentPid=" << currentPid << '\n';
        }
        kill(static_cast<pid_t>(previousPid), SIGTERM);
    }

    std::ofstream output(path, std::ios::trunc);
    if (output) {
        output << currentPid << '\n';
    }
}

void clearFallbackPid(const CandidateWindowData &data) {
    if (data.stdinMode || data.parseSnapshot || data.selfTest) {
        return;
    }
    if (!data.statusMode && data.ttlMs <= 0) {
        return;
    }

    const auto path = fallbackPidPath(data);
    if (path.empty()) {
        return;
    }

    std::ifstream currentFile(path);
    long storedPid = -1;
    currentFile >> storedPid;
    if (storedPid == static_cast<long>(getpid())) {
        std::remove(path.c_str());
    }
}

bool hasUsableCursorRect(const CandidateWindowData &data) {
    return data.cursorHeight > 0;
}

bool cursorInsideGeometry(const WindowCursorRect &rect, const GdkRectangle &geometry) {
    return rect.x >= geometry.x && rect.x < geometry.x + geometry.width && rect.y >= geometry.y &&
           rect.y < geometry.y + geometry.height;
}

WindowCursorRect scaledCursorRect(WindowCursorRect rect, double scale) {
    scale = std::max(1.0, scale);
    const auto logical = [scale](int value) { return static_cast<int>(std::lround(value / scale)); };
    const auto logicalExtent = [scale](int value) {
        return value > 0 ? std::max(1, static_cast<int>(std::lround(value / scale))) : value;
    };
    return {logical(rect.x), logical(rect.y), logicalExtent(rect.width), logicalExtent(rect.height)};
}

WindowCursorRect logicalX11PointerRect(const X11PointerSnapshot &pointer,
                                       const std::vector<MonitorLayout> &monitors) {
    auto result = pointer.rect;
    if (monitors.empty() || pointer.rootWidth <= 0 || pointer.rootHeight <= 0) {
        return result;
    }

    int logicalLeft = monitors.front().geometry.x;
    int logicalTop = monitors.front().geometry.y;
    int logicalRight = monitors.front().geometry.x + monitors.front().geometry.width;
    int logicalBottom = monitors.front().geometry.y + monitors.front().geometry.height;
    for (const auto &monitor : monitors) {
        logicalLeft = std::min(logicalLeft, monitor.geometry.x);
        logicalTop = std::min(logicalTop, monitor.geometry.y);
        logicalRight = std::max(logicalRight, monitor.geometry.x + monitor.geometry.width);
        logicalBottom = std::max(logicalBottom, monitor.geometry.y + monitor.geometry.height);
    }
    const int logicalWidth = logicalRight - logicalLeft;
    const int logicalHeight = logicalBottom - logicalTop;
    if (logicalWidth <= 0 || logicalHeight <= 0) {
        return result;
    }

    const double scaleX = static_cast<double>(pointer.rootWidth) / logicalWidth;
    const double scaleY = static_cast<double>(pointer.rootHeight) / logicalHeight;
    constexpr double minimumPhysicalScale = 1.1;
    constexpr double maximumPhysicalScale = 4.0;
    constexpr double maximumAxisDifference = 0.12;
    if (scaleX < minimumPhysicalScale || scaleX > maximumPhysicalScale ||
        scaleY < minimumPhysicalScale || scaleY > maximumPhysicalScale ||
        std::abs(scaleX - scaleY) > maximumAxisDifference) {
        return result;
    }

    result.x = logicalLeft + static_cast<int>(std::lround(pointer.rect.x / scaleX));
    result.y = logicalTop + static_cast<int>(std::lround(pointer.rect.y / scaleY));
    // The X11 query supplies only a point. Keep a logical synthetic caret
    // instead of shrinking its extent with the physical desktop scale.
    result.width = 1;
    result.height = 22;
    return result;
}

int winePreeditPrefixWidth(const CandidateWindowData &data, int fontSize = 15) {
    const auto cursor = std::min(data.preeditCursor, data.preedit.size());
    return tipe::candidateTextWidth(data.preedit.substr(0, cursor), fontSize, true);
}

void resetWineCaretTracking(CandidateWindowData &data) {
    data.wineCaretAnchor.reset();
    data.wineHasImmContext = false;
}

bool wineCaretNeedsSyntheticOffset(const CandidateWindowData &data) {
    // A Wine control without an IMM context cannot render the XIM callback
    // preedit. Its accessibility caret remains the insertion point before the
    // composition, so both the drawn preedit and the candidate panel must use
    // a deterministic synthetic advance. Do not infer this from small caret
    // movements: accessibility rectangles can jitter with fractional scaling.
    return !data.wineCaretAnchor || !data.wineHasImmContext;
}

WindowCursorRect wineInlinePreeditRect(const CandidateWindowData &data,
                                       const std::vector<MonitorLayout> &monitors) {
    if (!data.wineCaretAnchor) {
        return {};
    }
    return logicalX11PointerRect(*data.wineCaretAnchor, monitors);
}

bool wineNeedsInlinePreedit(const CandidateWindowData &data) {
    return !data.preedit.empty() && data.pointerFallback && data.wineCaretAnchor &&
           !data.wineHasImmContext;
}

WindowCursorRect normalizedCursorRect(WindowCursorRect rect, const std::vector<MonitorLayout> &monitors) {
    if (rect.height <= 0 || monitors.empty()) {
        return rect;
    }
    if (std::any_of(monitors.begin(), monitors.end(), [&](const auto &monitor) {
            return cursorInsideGeometry(rect, monitor.geometry);
        })) {
        return rect;
    }

    WindowCursorRect best = rect;
    int bestScore = std::numeric_limits<int>::max();
    const auto consider = [&](double scale, bool displayReportedScale) {
        if (scale <= 1.01) {
            return;
        }
        const auto candidate = scaledCursorRect(rect, scale);
        for (const auto &monitor : monitors) {
            if (!cursorInsideGeometry(candidate, monitor.geometry)) {
                continue;
            }
            const int bottomClearance = monitor.geometry.y + monitor.geometry.height -
                                        (candidate.y + std::max(1, candidate.height));
            const int edgePenalty = std::max(0, 48 - bottomClearance);
            const int heightPenalty = std::abs(candidate.height - 22) * 8;
            const int scalePenalty = static_cast<int>(std::lround(std::abs(scale - 2.0) * 4.0));
            const int score = (displayReportedScale ? 0 : 1000) + heightPenalty + edgePenalty + scalePenalty;
            if (score < bestScore) {
                best = candidate;
                bestScore = score;
            }
        }
    };

    for (const auto &monitor : monitors) {
        consider(monitor.scale, true);
    }
    constexpr std::array fallbackScales{1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 3.0, 4.0};
    for (const double scale : fallbackScales) {
        consider(scale, false);
    }
    return best;
}

void resolveCursorRect(CandidateWindowData &data, const std::vector<MonitorLayout> &monitors) {
    if (data.cursorRectResolved) {
        return;
    }
    const WindowCursorRect raw{data.cursorX, data.cursorY, data.cursorWidth, data.cursorHeight};
    WindowCursorRect source = raw;
    if (source.height <= 0 && data.pointerFallback) {
        if (data.wineCaretAnchor) {
            source = logicalX11PointerRect(*data.wineCaretAnchor, monitors);
        } else if (!data.pointerFallbackAnchor) {
            if (const auto pointer = queryX11PointerCursorRect()) {
                data.pointerFallbackAnchor = *pointer;
            }
        }
        if (source.height <= 0 && data.pointerFallbackAnchor) {
            source = logicalX11PointerRect(*data.pointerFallbackAnchor, monitors);
        }
    }
    auto resolved = normalizedCursorRect(source, monitors);
    if (data.staticCursorRect && !data.preedit.empty() && resolved.height > 0 &&
        wineCaretNeedsSyntheticOffset(data)) {
        resolved.x += winePreeditPrefixWidth(data);
    }
    data.cursorX = resolved.x;
    data.cursorY = resolved.y;
    data.cursorWidth = resolved.width;
    data.cursorHeight = resolved.height;
    data.cursorRectResolved = true;
    if (debugEnabled() && (raw.x != resolved.x || raw.y != resolved.y || raw.width != resolved.width ||
                           raw.height != resolved.height)) {
        std::cerr << "fallback\tcursor-normalized\traw=" << raw.x << ',' << raw.y << ',' << raw.width << ','
                  << raw.height << "\tresolved=" << resolved.x << ',' << resolved.y << ',' << resolved.width
                  << ',' << resolved.height << "\tstatic=" << (data.staticCursorRect ? 1 : 0)
                  << "\tpointerFallback=" << (data.pointerFallback ? 1 : 0) << '\n';
    }
}

std::vector<MonitorLayout> monitorLayouts(GListModel *monitors) {
    std::vector<MonitorLayout> result;
    if (!monitors) {
        return result;
    }
    const auto count = g_list_model_get_n_items(monitors);
    result.reserve(count);
    for (guint index = 0; index < count; ++index) {
        auto *monitorObject = static_cast<GObject *>(g_list_model_get_item(monitors, index));
        if (!monitorObject) {
            continue;
        }
        MonitorLayout layout;
        gdk_monitor_get_geometry(GDK_MONITOR(monitorObject), &layout.geometry);
        layout.scale = std::max(1.0, gdk_monitor_get_scale(GDK_MONITOR(monitorObject)));
        result.push_back(layout);
        g_object_unref(monitorObject);
    }
    return result;
}

void resolveCursorRectForDisplay(CandidateWindowData &data, GListModel *monitors) {
    resolveCursorRect(data, monitorLayouts(monitors));
}

void logFallbackPosition(const char *mode, CandidateWindowData &data, const GdkRectangle &geometry,
                         const WindowPosition &position, int windowWidth, int windowHeight) {
    if (!debugEnabled()) {
        return;
    }
    const int right = position.left + windowWidth;
    const int bottom = position.top + windowHeight;
    const bool boundsOk = position.left >= layerWindowHorizontalGuard && position.top >= layerWindowTopGuard &&
                          right <= geometry.width - layerWindowHorizontalGuard &&
                          bottom <= geometry.height - layerWindowBottomGuard;
    std::ostringstream key;
    key << mode << '\t' << data.cursorX << ',' << data.cursorY << ',' << data.cursorWidth << ','
        << data.cursorHeight << '\t' << geometry.x << ',' << geometry.y << ',' << geometry.width << ','
        << geometry.height << '\t' << position.left << ',' << position.top << '\t' << windowWidth << ','
        << windowHeight << '\t' << (boundsOk ? 1 : 0);
    if (data.lastFallbackPositionKey == key.str()) {
        return;
    }
    data.lastFallbackPositionKey = key.str();
    static std::size_t positionLogCount = 0;
    if (positionLogCount++ % 64 == 0) {
        tipe::trimOpenDiagnosticLog(STDERR_FILENO);
    }
    std::cerr << "fallback\tposition\tmode=" << mode << "\tpreedit=" << data.preedit
              << "\texpanded=" << (data.expanded ? 1 : 0) << "\tcursor=" << data.cursorX << ',' << data.cursorY
              << ',' << data.cursorWidth << ',' << data.cursorHeight << "\tmonitor=" << geometry.x << ','
              << geometry.y << ',' << geometry.width << ',' << geometry.height << "\tleft=" << position.left
              << "\ttop=" << position.top << "\twidth=" << windowWidth << "\theight=" << windowHeight
              << "\tright=" << right << "\tbottom=" << bottom << "\tboundsOk=" << (boundsOk ? 1 : 0) << '\n';
}

WindowPosition computeWindowPosition(const CandidateWindowData &data, const GdkRectangle &geometry) {
    const int windowWidth = effectiveWindowWidth(data, geometry);
    const int windowHeight = positioningWindowHeight(data);

    const int fallbackLeft = envInt("TIPE_CANDIDATE_LEFT").value_or((geometry.width - windowWidth) / 2);
    int left = std::clamp(fallbackLeft, layerWindowHorizontalGuard,
                          std::max(layerWindowHorizontalGuard, geometry.width - windowWidth - layerWindowHorizontalGuard));
    const int fallbackBottom = std::clamp(envInt("TIPE_CANDIDATE_BOTTOM").value_or(layerWindowFallbackBottom), 40, 320);
    const int maxTop = std::max(layerWindowTopGuard, geometry.height - windowHeight - layerWindowBottomGuard);
    int top = std::clamp(geometry.height - fallbackBottom - windowHeight, layerWindowTopGuard, maxTop);

    if (hasUsableCursorRect(data)) {
        return computeCursorAnchoredWindowPosition(windowWidth, windowHeight, data.cursorX, data.cursorY,
                                                   data.cursorWidth, data.cursorHeight, geometry);
    }

    return {left, top};
}

WindowPosition computeCursorAnchoredWindowPosition(int windowWidth, int windowHeight, int cursorX, int cursorY,
                                                   int cursorWidth, int cursorHeight, const GdkRectangle &geometry) {
    const int maxLeft = std::max(layerWindowHorizontalGuard, geometry.width - windowWidth - layerWindowHorizontalGuard);
    const int maxTop = std::max(layerWindowTopGuard, geometry.height - windowHeight - layerWindowBottomGuard);
    const int relativeX = cursorX - geometry.x;
    const int relativeY = cursorY - geometry.y;
    const int rightGuard = geometry.width - layerWindowHorizontalGuard;
    const int cursorRight = relativeX + std::max(1, cursorWidth);
    const int preferredLeft = relativeX + windowWidth <= rightGuard ? relativeX : cursorRight - windowWidth;
    const int left = std::clamp(preferredLeft, layerWindowHorizontalGuard, maxLeft);
    constexpr int cursorEdgeGap = 4;
    const int belowTop = relativeY + cursorHeight + cursorEdgeGap;
    const int aboveTop = relativeY - windowHeight - cursorEdgeGap;
    const int preferredTop = belowTop + windowHeight <= geometry.height - layerWindowBottomGuard ? belowTop : aboveTop;
    return {left, std::clamp(preferredTop, layerWindowTopGuard, maxTop)};
}

WindowPosition computeFixedWindowPosition(int windowWidth, int windowHeight, const GdkRectangle &geometry,
                                          int defaultLeft, int defaultTop, const char *leftEnv,
                                          const char *topEnv) {
    const int maxLeft = std::max(layerWindowHorizontalGuard, geometry.width - windowWidth - layerWindowHorizontalGuard);
    const int maxTop = std::max(layerWindowTopGuard, geometry.height - windowHeight - layerWindowBottomGuard);
    const int left = std::clamp(envInt(leftEnv).value_or(defaultLeft), layerWindowHorizontalGuard, maxLeft);
    const int top = std::clamp(envInt(topEnv).value_or(defaultTop), layerWindowTopGuard, maxTop);
    return {left, top};
}

GdkMonitor *monitorForPoint(GListModel *monitors, int x, int y, GdkRectangle &geometry) {
    const auto count = g_list_model_get_n_items(monitors);
    GObject *nearestMonitor = nullptr;
    GdkRectangle nearestGeometry{};
    long long nearestDistance = -1;
    for (guint index = 0; index < count; ++index) {
        auto *monitorObject = static_cast<GObject *>(g_list_model_get_item(monitors, index));
        if (!monitorObject) {
            continue;
        }
        GdkRectangle candidateGeometry{};
        gdk_monitor_get_geometry(GDK_MONITOR(monitorObject), &candidateGeometry);
        const bool contains = x >= candidateGeometry.x && x < candidateGeometry.x + candidateGeometry.width &&
                              y >= candidateGeometry.y && y < candidateGeometry.y + candidateGeometry.height;
        if (contains) {
            if (nearestMonitor) {
                g_object_unref(nearestMonitor);
            }
            geometry = candidateGeometry;
            return GDK_MONITOR(monitorObject);
        }

        const int clampedX = std::clamp(x, candidateGeometry.x, candidateGeometry.x + candidateGeometry.width - 1);
        const int clampedY = std::clamp(y, candidateGeometry.y, candidateGeometry.y + candidateGeometry.height - 1);
        const long long dx = static_cast<long long>(x) - clampedX;
        const long long dy = static_cast<long long>(y) - clampedY;
        const long long distance = dx * dx + dy * dy;
        if (nearestDistance < 0 || distance < nearestDistance) {
            if (nearestMonitor) {
                g_object_unref(nearestMonitor);
            }
            nearestMonitor = monitorObject;
            nearestGeometry = candidateGeometry;
            nearestDistance = distance;
        } else {
            g_object_unref(monitorObject);
        }
    }

    if (nearestMonitor) {
        geometry = nearestGeometry;
    }
    return nearestMonitor ? GDK_MONITOR(nearestMonitor) : nullptr;
}

GdkMonitor *primaryMonitor(GListModel *monitors, GdkRectangle &geometry) {
    auto *monitorObject = g_list_model_get_item(monitors, 0);
    if (monitorObject) {
        gdk_monitor_get_geometry(GDK_MONITOR(monitorObject), &geometry);
    }
    return monitorObject ? GDK_MONITOR(monitorObject) : nullptr;
}

std::optional<GdkRectangle> currentMonitorGeometry(const CandidateWindowData &data) {
    auto *display = gdk_display_get_default();
    if (!display) {
        return std::nullopt;
    }
    auto *monitors = gdk_display_get_monitors(display);
    if (!monitors || g_list_model_get_n_items(monitors) == 0) {
        return std::nullopt;
    }

    GdkRectangle geometry{};
    auto *monitor = !data.fixedStatus && hasUsableCursorRect(data)
                        ? monitorForPoint(monitors, data.cursorX, data.cursorY, geometry)
                        : primaryMonitor(monitors, geometry);
    if (!monitor) {
        return std::nullopt;
    }
    g_object_unref(monitor);
    return geometry;
}

void hideWineInlinePreedit(CandidateWindowData &data) {
    if (data.inlinePreeditWindow) {
        gtk_widget_set_visible(data.inlinePreeditWindow, FALSE);
    }
}

void makeWindowInputTransparent(GtkWidget *window) {
    if (!window) {
        return;
    }
    gtk_widget_realize(window);
    auto *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (!surface) {
        return;
    }
    auto *region = cairo_region_create();
    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);
}

void updateWineInlinePreedit(CandidateWindowData &data, GListModel *monitors) {
    if (!data.inlinePreeditWindow || !data.inlinePreeditPanel || !wineNeedsInlinePreedit(data) || !monitors) {
        hideWineInlinePreedit(data);
        return;
    }

    const auto layouts = monitorLayouts(monitors);
    auto anchor = wineInlinePreeditRect(data, layouts);
    if (anchor.height <= 0) {
        hideWineInlinePreedit(data);
        return;
    }

    GdkRectangle geometry{};
    auto *monitor = monitorForPoint(monitors, anchor.x, anchor.y, geometry);
    if (!monitor) {
        hideWineInlinePreedit(data);
        return;
    }

    data.inlinePreeditFontSize = std::clamp(anchor.height - 7, 13, 18);
    const int textWidth = tipe::candidateTextWidth(data.preedit, data.inlinePreeditFontSize, true);
    data.inlinePreeditWidth = std::clamp(textWidth + 5, 12, std::max(12, geometry.width - 8));
    data.inlinePreeditHeight = std::clamp(anchor.height + 2, 20, 30);
    const int maxLeft = std::max(4, geometry.width - data.inlinePreeditWidth - 4);
    const int maxTop = std::max(4, geometry.height - data.inlinePreeditHeight - 4);
    const int left = std::clamp(anchor.x - geometry.x, 4, maxLeft);
    const int top = std::clamp(anchor.y - geometry.y, 4, maxTop);

    gtk_layer_set_monitor(GTK_WINDOW(data.inlinePreeditWindow), monitor);
    gtk_window_set_default_size(GTK_WINDOW(data.inlinePreeditWindow), data.inlinePreeditWidth,
                                data.inlinePreeditHeight);
    gtk_widget_set_size_request(data.inlinePreeditWindow, data.inlinePreeditWidth, data.inlinePreeditHeight);
    gtk_widget_set_size_request(data.inlinePreeditPanel, data.inlinePreeditWidth, data.inlinePreeditHeight);
    gtk_layer_set_margin(GTK_WINDOW(data.inlinePreeditWindow), GTK_LAYER_SHELL_EDGE_LEFT, left);
    gtk_layer_set_margin(GTK_WINDOW(data.inlinePreeditWindow), GTK_LAYER_SHELL_EDGE_TOP, top);
    gtk_widget_queue_draw(data.inlinePreeditPanel);
    if (!gtk_widget_get_visible(data.inlinePreeditWindow)) {
        gtk_widget_set_visible(data.inlinePreeditWindow, TRUE);
        gtk_window_present(GTK_WINDOW(data.inlinePreeditWindow));
    }
    g_object_unref(monitor);
}

void updateStatusLayerPosition(CandidateWindowData &data) {
    if (!data.window || !gtk_layer_is_layer_window(GTK_WINDOW(data.window))) {
        return;
    }

    auto *display = gdk_display_get_default();
    if (!display) {
        return;
    }
    auto *monitors = gdk_display_get_monitors(display);
    if (!monitors || g_list_model_get_n_items(monitors) == 0) {
        return;
    }
    resolveCursorRectForDisplay(data, monitors);

    GdkRectangle geometry{};
    auto *monitor = hasUsableCursorRect(data) ? monitorForPoint(monitors, data.cursorX, data.cursorY, geometry)
                                              : primaryMonitor(monitors, geometry);
    if (!monitor) {
        return;
    }
    gtk_layer_set_monitor(GTK_WINDOW(data.window), monitor);
    constexpr int statusWidth = tipe::tipeUIStatusPopupWidth;
    const int statusHeight = positioningStatusHeight();
    setLayerWindowSize(data, statusWidth, requestedStatusHeight());
    const auto position = !data.fixedStatus && hasUsableCursorRect(data)
                              ? computeCursorAnchoredWindowPosition(statusWidth, statusHeight, data.cursorX,
                                                                    data.cursorY, data.cursorWidth,
                                                                    data.cursorHeight, geometry)
                              : computeFixedWindowPosition(statusWidth, statusHeight, geometry,
                                                           geometry.width - statusWidth - 32, 32,
                                                           "TIPE_STATUS_LEFT", "TIPE_STATUS_TOP");
    gtk_layer_set_margin(GTK_WINDOW(data.window), GTK_LAYER_SHELL_EDGE_LEFT, position.left);
    gtk_layer_set_margin(GTK_WINDOW(data.window), GTK_LAYER_SHELL_EDGE_TOP, position.top);
    logFallbackPosition(data.fixedStatus ? "status-fixed" : "status", data, geometry, position, statusWidth,
                        statusHeight);
    g_object_unref(monitor);
}

int currentWindowWidth(const CandidateWindowData &data) {
    if (const auto geometry = currentMonitorGeometry(data)) {
        return effectiveWindowWidth(data, *geometry);
    }
    return estimatedWindowWidth(data);
}

void updateLayerPosition(CandidateWindowData &data) {
    if (!data.window || !gtk_layer_is_layer_window(GTK_WINDOW(data.window))) {
        return;
    }

    auto *display = gdk_display_get_default();
    if (!display) {
        return;
    }
    auto *monitors = gdk_display_get_monitors(display);
    if (!monitors || g_list_model_get_n_items(monitors) == 0) {
        return;
    }
    resolveCursorRectForDisplay(data, monitors);
    GdkRectangle geometry{};
    const bool plausibleCursor = hasUsableCursorRect(data);
    auto *monitor = plausibleCursor ? monitorForPoint(monitors, data.cursorX, data.cursorY, geometry)
                                    : primaryMonitor(monitors, geometry);
    if (!monitor) {
        return;
    }
    gtk_layer_set_monitor(GTK_WINDOW(data.window), monitor);

    const auto windowWidth = effectiveWindowWidth(data, geometry);
    setLayerWindowSize(data, windowWidth, requestedWindowHeight(data));
    const auto position = computeWindowPosition(data, geometry);

    gtk_layer_set_margin(GTK_WINDOW(data.window), GTK_LAYER_SHELL_EDGE_LEFT, position.left);
    gtk_layer_set_margin(GTK_WINDOW(data.window), GTK_LAYER_SHELL_EDGE_TOP, position.top);
    logFallbackPosition("candidate", data, geometry, position, windowWidth, positioningWindowHeight(data));
    if (debugEnabled()) {
        std::cerr << "position preedit=" << data.preedit << " expanded=" << data.expanded << " cursor=" << data.cursorX
                  << ',' << data.cursorY << ',' << data.cursorWidth << ',' << data.cursorHeight << " monitor="
                  << geometry.x << ',' << geometry.y << ',' << geometry.width << ',' << geometry.height
                  << " plausible=" << plausibleCursor << " left=" << position.left << " top=" << position.top
                  << '\n';
    }
    g_object_unref(monitor);
}

void render(CandidateWindowData &data) {
    if (!data.window || !data.panel) {
        return;
    }

    if (data.preedit.empty() && data.candidates.empty()) {
        data.candidateHitRegions.clear();
        data.hoveredCandidateIndex.reset();
        hideWineInlinePreedit(data);
        gtk_window_set_default_size(GTK_WINDOW(data.window), collapsedMinWindowWidth, 1);
        gtk_widget_set_size_request(data.window, collapsedMinWindowWidth, 1);
        gtk_widget_set_size_request(data.panel, collapsedMinWindowWidth, 1);
        gtk_widget_set_visible(data.window, FALSE);
        return;
    }

    if (data.candidates.empty() || data.selectedIndex >= data.candidates.size()) {
        data.selectedIndex = 0;
    }
    data.preeditCursor = std::min(data.preeditCursor, data.preedit.size());
    if (auto *display = gdk_display_get_default()) {
        auto *monitors = gdk_display_get_monitors(display);
        resolveCursorRectForDisplay(data, monitors);
        updateWineInlinePreedit(data, monitors);
    }
    const auto windowWidth = currentWindowWidth(data);
    data.candidateHitRegions = tipe::candidatePanelHitRegions(
        visibleCellsFor(data), data.candidates, windowWidth, data.expanded, !data.preedit.empty());
    if (data.hoveredCandidateIndex &&
        std::none_of(data.candidateHitRegions.begin(), data.candidateHitRegions.end(), [&](const auto &region) {
            return region.index == *data.hoveredCandidateIndex;
        })) {
        data.hoveredCandidateIndex.reset();
    }
    setLayerWindowSize(data, windowWidth, requestedWindowHeight(data));
    updateLayerPosition(data);
    gtk_widget_queue_draw(data.panel);

    gtk_widget_set_visible(data.window, TRUE);
    gtk_window_present(GTK_WINDOW(data.window));
}

void drawPanel(GtkDrawingArea *, cairo_t *cr, int width, int height, gpointer userData) {
    const auto *data = static_cast<const CandidateWindowData *>(userData);
    if (data->statusMode) {
        tipe::renderCandidateStatus(cr, width, height, data->statusText);
        return;
    }
    tipe::renderCandidatePanel(cr, width, height, data->preedit, static_cast<int>(data->preeditCursor),
                               data->candidates, data->selectedIndex, data->expanded, data->continuous,
                               data->hoveredCandidateIndex);
}

void drawWineInlinePreedit(GtkDrawingArea *, cairo_t *cr, int width, int height, gpointer userData) {
    const auto *data = static_cast<const CandidateWindowData *>(userData);
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const int textY = std::max(0, (height - data->inlinePreeditFontSize - 6) / 2);
    tipe::drawCandidateText(cr, data->preedit, 1, textY, std::max(1, width - 2),
                            data->inlinePreeditFontSize, true, 0.05, 0.38, 0.92);
    const int cursor = std::clamp(static_cast<int>(data->preeditCursor), 0,
                                  static_cast<int>(data->preedit.size()));
    const int cursorX = std::clamp(
        1 + tipe::candidateTextWidth(data->preedit.substr(0, static_cast<std::size_t>(cursor)),
                                     data->inlinePreeditFontSize, true),
        1, std::max(1, width - 2));
    cairo_set_source_rgba(cr, 0.04, 0.52, 1.0, 0.98);
    cairo_set_line_width(cr, 1.4);
    cairo_move_to(cr, 1, height - 1.5);
    cairo_line_to(cr, std::max(1, width - 2), height - 1.5);
    cairo_move_to(cr, cursorX + 0.5, 2);
    cairo_line_to(cr, cursorX + 0.5, height - 3);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void updateHoveredCandidate(CandidateWindowData &data, double x, double y) {
    const auto hovered = tipe::candidateIndexAtPoint(data.candidateHitRegions, x, y);
    if (hovered == data.hoveredCandidateIndex) {
        return;
    }
    data.hoveredCandidateIndex = hovered;
    if (data.panel) {
        gtk_widget_queue_draw(data.panel);
    }
}

void pointerMotion(GtkEventControllerMotion *, double x, double y, gpointer userData) {
    updateHoveredCandidate(*static_cast<CandidateWindowData *>(userData), x, y);
}

void pointerLeave(GtkEventControllerMotion *, gpointer userData) {
    auto &data = *static_cast<CandidateWindowData *>(userData);
    if (!data.hoveredCandidateIndex) {
        return;
    }
    data.hoveredCandidateIndex.reset();
    if (data.panel) {
        gtk_widget_queue_draw(data.panel);
    }
}

void pointerPressed(GtkGestureClick *, int, double x, double y, gpointer userData) {
    auto &data = *static_cast<CandidateWindowData *>(userData);
    if (data.eventsFd < 0) {
        return;
    }
    const auto index = tipe::candidateIndexAtPoint(data.candidateHitRegions, x, y);
    if (!index) {
        return;
    }
    const auto message = std::string("select\t") + std::to_string(data.snapshotSerial) + '\t' +
                         std::to_string(*index) + '\n';
    const char *cursor = message.data();
    std::size_t remaining = message.size();
    while (remaining > 0) {
        const auto written = write(data.eventsFd, cursor, remaining);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return;
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

std::string wineCaretBridgeExecutablePath() {
#ifdef TIPE_WINE_CARET_BRIDGE_PATH
    if (access(TIPE_WINE_CARET_BRIDGE_PATH, R_OK) == 0) {
        return TIPE_WINE_CARET_BRIDGE_PATH;
    }
#endif
    const char *home = std::getenv("HOME");
    if (!home || !*home) {
        return {};
    }
    const auto fallback = std::string(home) + "/.local/libexec/tipe/tipe-wine-caret-bridge.exe";
    return access(fallback.c_str(), R_OK) == 0 ? fallback : std::string{};
}

void cancelWineCaretRenderTimeout(CandidateWindowData &data) {
    if (data.wineCaretRenderTimeoutId != 0) {
        g_source_remove(data.wineCaretRenderTimeoutId);
        data.wineCaretRenderTimeoutId = 0;
    }
    data.pendingWineCaretRender = 0;
}

gboolean wineCaretRenderTimeout(gpointer userData) {
    auto &data = *static_cast<CandidateWindowData *>(userData);
    data.wineCaretRenderTimeoutId = 0;
    const bool current = data.pendingWineCaretRender != 0 &&
                         data.pendingWineCaretRender == data.latestWineCaretRequest;
    data.pendingWineCaretRender = 0;
    if (current && data.pointerFallback && !data.preedit.empty()) {
        render(data);
    }
    return G_SOURCE_REMOVE;
}

void scheduleWineCaretRenderTimeout(CandidateWindowData &data, std::uint64_t serial) {
    cancelWineCaretRenderTimeout(data);
    data.pendingWineCaretRender = serial;
    constexpr guint bridgeWaitMs = 60;
    data.wineCaretRenderTimeoutId = g_timeout_add(bridgeWaitMs, wineCaretRenderTimeout, &data);
}

void removeWineCaretWatch(CandidateWindowData &data, bool stopBridge) {
    if (data.wineCaretWatchId != 0) {
        g_source_remove(data.wineCaretWatchId);
        data.wineCaretWatchId = 0;
    }
    data.wineCaretOutputFd = -1;
    if (stopBridge && data.wineCaretBridge) {
        data.wineCaretBridge->stop();
        data.wineCaretTargetPid = -1;
    }
}

gboolean wineCaretReady(GIOChannel *source, GIOCondition condition, gpointer userData) {
    auto &data = *static_cast<CandidateWindowData *>(userData);
    bool terminal = false;
    if (condition & G_IO_IN) {
        while (true) {
            gchar *line = nullptr;
            gsize length = 0;
            const auto status = g_io_channel_read_line(source, &line, &length, nullptr, nullptr);
            if (status == G_IO_STATUS_NORMAL && line) {
                const auto reply = tipe::parseWineCaretBridgeReply(std::string_view(line, length));
                if (reply && reply->serial == data.latestWineCaretRequest && data.pointerFallback &&
                    !data.preedit.empty()) {
                    const bool pending = data.pendingWineCaretRender == reply->serial;
                    bool changed = false;
                    if (reply->rect && data.wineCaretRootWidth > 0 && data.wineCaretRootHeight > 0) {
                        data.wineHasImmContext = reply->hasImmContext;
                        const auto &rect = *reply->rect;
                        const X11PointerSnapshot anchor{{rect.x, rect.y, rect.width, rect.height},
                                                        data.wineCaretRootWidth, data.wineCaretRootHeight};
                        changed = !data.wineCaretAnchor || data.wineCaretAnchor->rect.x != anchor.rect.x ||
                                  data.wineCaretAnchor->rect.y != anchor.rect.y ||
                                  data.wineCaretAnchor->rect.width != anchor.rect.width ||
                                  data.wineCaretAnchor->rect.height != anchor.rect.height;
                        data.wineCaretAnchor = anchor;
                        data.cursorRectResolved = false;
                        if (debugEnabled() && changed) {
                            std::cerr << "fallback\twine-caret\traw=" << rect.x << ',' << rect.y << ','
                                      << rect.width << ',' << rect.height << "\troot="
                                      << data.wineCaretRootWidth << ',' << data.wineCaretRootHeight
                                      << "\ttargetPid=" << data.wineCaretTargetPid << '\n';
                        }
                    } else {
                        changed = data.wineCaretAnchor.has_value();
                        resetWineCaretTracking(data);
                        data.cursorRectResolved = false;
                    }
                    if (pending) {
                        cancelWineCaretRenderTimeout(data);
                    }
                    if (pending || changed) {
                        render(data);
                    }
                }
                g_free(line);
                continue;
            }
            g_free(line);
            terminal = status == G_IO_STATUS_EOF || status == G_IO_STATUS_ERROR;
            break;
        }
    }
    terminal = terminal || (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL));
    if (!terminal) {
        return G_SOURCE_CONTINUE;
    }
    data.wineCaretWatchId = 0;
    data.wineCaretOutputFd = -1;
    if (data.wineCaretBridge) {
        data.wineCaretBridge->stop();
    }
    data.wineCaretTargetPid = -1;
    return G_SOURCE_REMOVE;
}

void installWineCaretWatch(CandidateWindowData &data, int outputFd) {
    if (outputFd < 0 || (data.wineCaretWatchId != 0 && data.wineCaretOutputFd == outputFd)) {
        return;
    }
    removeWineCaretWatch(data, false);
    auto *channel = g_io_channel_unix_new(outputFd);
    g_io_channel_set_encoding(channel, nullptr, nullptr);
    g_io_channel_set_close_on_unref(channel, FALSE);
    data.wineCaretWatchId =
        g_io_add_watch(channel, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
                       wineCaretReady, &data);
    g_io_channel_unref(channel);
    data.wineCaretOutputFd = data.wineCaretWatchId != 0 ? outputFd : -1;
}

bool requestWineCaret(CandidateWindowData &data) {
    cancelWineCaretRenderTimeout(data);
    if (data.preedit.empty()) {
        resetWineCaretTracking(data);
        return false;
    }
    if (!data.pointerFallback) {
        resetWineCaretTracking(data);
        removeWineCaretWatch(data, true);
        return false;
    }
    if (!data.wineCaretBridge) {
        const auto executable = wineCaretBridgeExecutablePath();
        if (executable.empty()) {
            return false;
        }
        data.wineCaretBridge = std::make_shared<tipe::WineCaretBridge>(executable);
    }
    if (++data.wineCaretRequestSerial == 0) {
        data.wineCaretRequestSerial = 1;
    }
    const auto request = data.wineCaretBridge->request(data.wineCaretRequestSerial);
    if (request.targetPid != data.wineCaretTargetPid) {
        resetWineCaretTracking(data);
    }
    data.wineCaretTargetPid = request.targetPid;
    if (!request.sent) {
        if (request.outputFd < 0) {
            removeWineCaretWatch(data, false);
        }
        return false;
    }
    data.latestWineCaretRequest = data.wineCaretRequestSerial;
    data.wineCaretRootWidth = request.rootWidth;
    data.wineCaretRootHeight = request.rootHeight;
    installWineCaretWatch(data, request.outputFd);
    scheduleWineCaretRenderTimeout(data, data.latestWineCaretRequest);
    return true;
}

gboolean closeStatusWindow(gpointer userData) {
    auto *window = static_cast<GtkWidget *>(userData);
    if (window) {
        gtk_window_destroy(GTK_WINDOW(window));
    }
    return G_SOURCE_REMOVE;
}

gboolean stdinReady(GIOChannel *source, GIOCondition condition, gpointer userData) {
    auto *data = static_cast<CandidateWindowData *>(userData);
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    }

    gchar *line = nullptr;
    gsize length = 0;
    const auto status = g_io_channel_read_line(source, &line, &length, nullptr, nullptr);
    if (status == G_IO_STATUS_NORMAL && line) {
        if (applySnapshotLine(*data, std::string_view(line, length))) {
            if (!requestWineCaret(*data)) {
                render(*data);
            }
        }
    }
    g_free(line);
    return G_SOURCE_CONTINUE;
}

bool selfTest() {
    CandidateWindowData data;
    if (!applySnapshotLine(data, "nihao\t1\t7\t100\t200\t3\t18\t你好|你号|拟好|倪浩|泥豪|你好啊|你不好|你很好\n")) {
        std::cerr << "failed to parse modern snapshot\n";
        return false;
    }
    if (data.preedit != "nihao" || !data.expanded || data.selectedIndex != 7 || data.cursorX != 100 ||
        data.cursorY != 200 || data.cursorWidth != 3 || data.cursorHeight != 18 || data.candidates.size() != 8 ||
        data.candidates[7] != "你很好") {
        std::cerr << "modern snapshot fields did not round-trip\n";
        return false;
    }
    if (!applySnapshotLine(data,
                           "nihao\t0\t0\t100\t200\t3\t18\t你好|你号\tsupervision=1,keys=12,selects=2,reranks=1,continuous=1,preedit_cursor=2,cursor_static=1,snapshot=17,pointer_fallback=1\n")) {
        std::cerr << "failed to parse supervision snapshot metadata\n";
        return false;
    }
    if (!data.supervision || !data.continuous || data.supervisedKeys != 12 || data.selections != 2 ||
        data.reranks != 1 || data.preeditCursor != 2 || !data.staticCursorRect || data.snapshotSerial != 17 ||
        !data.pointerFallback ||
        estimatedWindowHeight(data) != 68) {
        std::cerr << "supervision snapshot metadata should parse without changing panel height\n";
        return false;
    }
    int eventPipe[2]{-1, -1};
    if (pipe(eventPipe) != 0) {
        std::cerr << "failed to create candidate event self-test pipe\n";
        return false;
    }
    data.eventsFd = eventPipe[1];
    data.candidateHitRegions = {{4, 10, 20, 80, 48}};
    pointerPressed(nullptr, 1, 20, 30, &data);
    std::array<char, 64> eventBuffer{};
    const auto eventSize = read(eventPipe[0], eventBuffer.data(), eventBuffer.size());
    close(eventPipe[0]);
    close(eventPipe[1]);
    data.eventsFd = -1;
    if (eventSize <= 0 || std::string_view(eventBuffer.data(), static_cast<std::size_t>(eventSize)) !=
                              "select\t17\t4\n") {
        std::cerr << "candidate pointer event should include snapshot serial and candidate index\n";
        return false;
    }
    const auto previousPreedit = data.preedit;
    if (applySnapshotLine(data, "too\tshort\n") ||
        applySnapshotLine(data, "too\tmany\t0\t1\t2\t3\t4\t候选\textra\n") || data.preedit != previousPreedit) {
        std::cerr << "invalid snapshot field counts should be rejected without changing state\n";
        return false;
    }
    if (applySnapshotLine(data, "bad-number\t1\t7x\t100\t200\t3\t18\t你好\n") ||
        applySnapshotLine(data, "bad-cursor\t0\t10\t20px\t2\t16\t你好\n") || data.preedit != previousPreedit) {
        std::cerr << "invalid snapshot numeric fields should be rejected without changing state\n";
        return false;
    }

    if (!applySnapshotLine(data, "nihao\t0\t10\t20\t2\t16\t你好|你号\n")) {
        std::cerr << "failed to parse legacy snapshot\n";
        return false;
    }
    if (data.expanded || data.selectedIndex != 0 || data.cursorX != 10 || data.cursorY != 20 ||
        data.candidates.size() != 2) {
        std::cerr << "legacy snapshot fields did not round-trip\n";
        return false;
    }

    data.selectedIndex = 1;
    data.cursorX = 10;
    data.cursorY = 20;
    data.cursorWidth = 2;
    data.cursorHeight = 16;
    if (!applySnapshotLine(data, "nihao\t0\t你好|你号\n")) {
        std::cerr << "failed to parse minimal snapshot\n";
        return false;
    }
    if (data.selectedIndex != 0 || data.cursorX != -1 || data.cursorY != -1 || data.cursorWidth != 0 ||
        data.cursorHeight != 0 || data.candidates.size() != 2) {
        std::cerr << "minimal snapshot should reset stale selected index and cursor rect\n";
        return false;
    }

    if (!applySnapshotLine(data, "\t0\t0\t0\t0\t0\t0\t\n")) {
        std::cerr << "failed to parse clear snapshot\n";
        return false;
    }
    if (!data.preedit.empty() || !data.candidates.empty() || data.expanded || data.selectedIndex != 0 ||
        data.preeditCursor != 0 || data.cursorX != 0 || data.cursorY != 0 || data.cursorWidth != 0 ||
        data.cursorHeight != 0 || data.supervision || data.continuous || data.supervisedKeys != 0 ||
        data.selections != 0 || data.reranks != 0 || data.staticCursorRect) {
        std::cerr << "clear snapshot did not clear all reusable-window state\n";
        return false;
    }

    if (!applySnapshotLine(data, "pipe\t0\t0\t1\t2\t3\t4\tA\\|B|slash\\\\value|tab\\tvalue\n")) {
        std::cerr << "failed to parse snapshot after clear\n";
        return false;
    }
    if (data.preedit != "pipe" || data.cursorX != 1 || data.cursorY != 2 || data.cursorWidth != 3 ||
        data.cursorHeight != 4 || data.candidates.size() != 3 || data.candidates[0] != "A|B" ||
        data.candidates[1] != "slash\\value" || data.candidates[2] != "tab\tvalue") {
        std::cerr << "snapshot after clear retained stale state or failed to round-trip\n";
        return false;
    }
    if (!applySnapshotLine(data, "nihao\t1\t99\t1\t2\t3\t4\t你好|你号\n")) {
        std::cerr << "failed to parse out-of-range selected snapshot\n";
        return false;
    }
    if (data.selectedIndex != 0 || data.candidates.size() != 2) {
        std::cerr << "out-of-range selected index should clamp to first candidate\n";
        return false;
    }

    std::vector<tipe::VisualCandidateCell> shortcutCells{{6, 1, 0, 1}, {7, 1, 1, 1}, {0, 0, 0, 1}};
    if (tipe::shortcutForVisualCell(shortcutCells, shortcutCells[0], 1, true) != "1" ||
        tipe::shortcutForVisualCell(shortcutCells, shortcutCells[1], 1, true) != "2" ||
        !tipe::shortcutForVisualCell(shortcutCells, shortcutCells[2], 1, true).empty()) {
        std::cerr << "expanded shortcut labels should follow the selected row only\n";
        return false;
    }
    if (tipe::firstVisibleExpandedRow(0, 10, maxExpandedRows) != 0 ||
        tipe::firstVisibleExpandedRow(7, 10, maxExpandedRows) != 5 ||
        tipe::firstVisibleExpandedRow(9, 10, maxExpandedRows) != 5) {
        std::cerr << "expanded row window should keep the selected row visible and clamped\n";
        return false;
    }
    data.expanded = true;
    data.selectedIndex = 9;
    data.candidates = {"好的我看一下还有美誉", "好的我看一下还有美与", "好的我看一下还有美于", "好的",
                       "好得",                 "好",                 "号",                 "豪",
                       "浩",                   "毫",                 "郝",                 "昊"};
    const auto typoCells = expandedCellsFor(data);
    const auto typoSelected = std::find_if(typoCells.begin(), typoCells.end(), [](const auto &cell) {
        return cell.index == 9;
    });
    if (typoSelected == typoCells.end() ||
        tipe::shortcutForVisualCell(typoCells, *typoSelected, typoSelected->row, true).empty()) {
        std::cerr << "expanded row-local shortcuts should follow visual rows after long candidates\n";
        return false;
    }
    int cursorX = 0;
    int cursorY = 0;
    int cursorWidth = 0;
    int cursorHeight = 0;
    if (!parseCursorRect("12,34,5,18", cursorX, cursorY, cursorWidth, cursorHeight) || cursorX != 12 ||
        cursorY != 34 || cursorWidth != 5 || cursorHeight != 18 ||
        parseCursorRect("12,34,5", cursorX, cursorY, cursorWidth, cursorHeight) ||
        parseCursorRect("12,xx,5,18", cursorX, cursorY, cursorWidth, cursorHeight)) {
        std::cerr << "cursor rectangle CLI parser should accept only x,y,width,height\n";
        return false;
    }
    const auto wineCaretReply = tipe::parseWineCaretBridgeReply("caret\t42\t2094\t1722\t1\t30\n");
    const auto wineCaretWithImm = tipe::parseWineCaretBridgeReply("caret\t43\t2094\t1722\t1\t30\t1\n");
    const auto wineCaretMissing = tipe::parseWineCaretBridgeReply("none\t44\t0\n");
    if (!wineCaretReply || wineCaretReply->serial != 42 || !wineCaretReply->rect ||
        wineCaretReply->rect->x != 2094 || wineCaretReply->rect->y != 1722 ||
        wineCaretReply->rect->width != 1 || wineCaretReply->rect->height != 30 || wineCaretReply->hasImmContext ||
        !wineCaretWithImm || !wineCaretWithImm->hasImmContext ||
        !wineCaretMissing || wineCaretMissing->serial != 44 || wineCaretMissing->rect ||
        tipe::parseWineCaretBridgeReply("caret\t45\t0\t0\t2\t22\t0") ||
        tipe::parseWineCaretBridgeReply("caret\t46\t20\t30\t2\t22\t2") ||
        tipe::parseWineCaretBridgeReply("caret\tbad\t20\t30\t2\t22")) {
        std::cerr << "Wine caret bridge replies should parse IMM state and reject invalid sentinels\n";
        return false;
    }

    GdkRectangle geometry{0, 0, 1280, 720};

    const std::vector<MonitorLayout> hidpiMonitor{{{0, 0, 1560, 1040}, 2.0}};
    const auto normalizedPhysicalCursor = normalizedCursorRect({582, 1762, 4, 44}, hidpiMonitor);
    if (normalizedPhysicalCursor.x != 291 || normalizedPhysicalCursor.y != 881 ||
        normalizedPhysicalCursor.width != 2 || normalizedPhysicalCursor.height != 22) {
        std::cerr << "out-of-bounds physical cursor coordinates should use the display scale\n";
        return false;
    }
    const auto retainedLogicalCursor = normalizedCursorRect({505, 899, 1, 23}, hidpiMonitor);
    if (retainedLogicalCursor.x != 505 || retainedLogicalCursor.y != 899 ||
        retainedLogicalCursor.width != 1 || retainedLogicalCursor.height != 23) {
        std::cerr << "in-bounds logical cursor coordinates should never be scaled twice\n";
        return false;
    }
    CandidateWindowData staticCursorData;
    staticCursorData.preedit = "nihao";
    staticCursorData.preeditCursor = staticCursorData.preedit.size();
    staticCursorData.cursorX = 505;
    staticCursorData.cursorY = 899;
    staticCursorData.cursorWidth = 1;
    staticCursorData.cursorHeight = 23;
    staticCursorData.staticCursorRect = true;
    resolveCursorRect(staticCursorData, hidpiMonitor);
    if (staticCursorData.cursorX != 505 + tipe::candidateTextWidth("nihao", 15, true) ||
        staticCursorData.cursorY != 899) {
        std::cerr << "a proven static client cursor should follow the measured preedit caret\n";
        return false;
    }
    CandidateWindowData liveCursorData = staticCursorData;
    liveCursorData.cursorX = 505;
    liveCursorData.staticCursorRect = false;
    liveCursorData.cursorRectResolved = false;
    resolveCursorRect(liveCursorData, hidpiMonitor);
    if (liveCursorData.cursorX != 505) {
        std::cerr << "a live client cursor should not receive a synthetic preedit offset\n";
        return false;
    }
    CandidateWindowData pointerFallbackData;
    pointerFallbackData.preedit = "nihao";
    pointerFallbackData.preeditCursor = 2;
    pointerFallbackData.cursorX = 0;
    pointerFallbackData.cursorY = 0;
    pointerFallbackData.cursorWidth = 0;
    pointerFallbackData.cursorHeight = 0;
    pointerFallbackData.staticCursorRect = true;
    pointerFallbackData.pointerFallback = true;
    pointerFallbackData.pointerFallbackAnchor = X11PointerSnapshot{{420, 360, 1, 22}, 1560, 1040};
    resolveCursorRect(pointerFallbackData, hidpiMonitor);
    if (pointerFallbackData.cursorX != 420 + tipe::candidateTextWidth("ni", 15, true) ||
        pointerFallbackData.cursorY != 360 ||
        pointerFallbackData.cursorWidth != 1 || pointerFallbackData.cursorHeight != 22) {
        std::cerr << "missing XIM spot locations should advance from one stable pointer origin\n";
        return false;
    }
    CandidateWindowData physicalPointerFallbackData;
    physicalPointerFallbackData.preedit = "nihao";
    physicalPointerFallbackData.preeditCursor = 2;
    physicalPointerFallbackData.cursorX = 0;
    physicalPointerFallbackData.cursorY = 0;
    physicalPointerFallbackData.cursorWidth = 0;
    physicalPointerFallbackData.cursorHeight = 0;
    physicalPointerFallbackData.staticCursorRect = true;
    physicalPointerFallbackData.pointerFallback = true;
    physicalPointerFallbackData.pointerFallbackAnchor =
        X11PointerSnapshot{{1500, 860, 1, 22}, 3120, 2080};
    resolveCursorRect(physicalPointerFallbackData, hidpiMonitor);
    if (physicalPointerFallbackData.cursorX != 750 + tipe::candidateTextWidth("ni", 15, true) ||
        physicalPointerFallbackData.cursorY != 430 || physicalPointerFallbackData.cursorWidth != 1 ||
        physicalPointerFallbackData.cursorHeight != 22) {
        std::cerr << "Xwayland physical pointer coordinates should map to the GTK logical desktop\n";
        return false;
    }
    CandidateWindowData wineCaretFallbackData;
    wineCaretFallbackData.preedit = "nihao";
    wineCaretFallbackData.preeditCursor = 2;
    wineCaretFallbackData.cursorX = 0;
    wineCaretFallbackData.cursorY = 0;
    wineCaretFallbackData.cursorWidth = 0;
    wineCaretFallbackData.cursorHeight = 0;
    wineCaretFallbackData.staticCursorRect = true;
    wineCaretFallbackData.pointerFallback = true;
    wineCaretFallbackData.pointerFallbackAnchor = X11PointerSnapshot{{80, 90, 1, 22}, 3120, 2080};
    wineCaretFallbackData.wineCaretAnchor = X11PointerSnapshot{{2094, 1722, 1, 30}, 3120, 2080};
    resolveCursorRect(wineCaretFallbackData, hidpiMonitor);
    if (wineCaretFallbackData.cursorX != 1047 + tipe::candidateTextWidth("ni", 15, true) ||
        wineCaretFallbackData.cursorY != 861 || wineCaretFallbackData.cursorWidth != 1 ||
        wineCaretFallbackData.cursorHeight != 22) {
        std::cerr << "Wine MSAA caret coordinates should override and scale like the Xwayland root\n";
        return false;
    }
    const auto staticInlineRect = wineInlinePreeditRect(wineCaretFallbackData, hidpiMonitor);
    if (staticInlineRect.x != 1047 || staticInlineRect.y != 861) {
        std::cerr << "a static Wine caret should anchor inline preedit at the application caret\n";
        return false;
    }
    if (!wineNeedsInlinePreedit(wineCaretFallbackData)) {
        std::cerr << "a Wine control without IMM support should receive an inline preedit overlay\n";
        return false;
    }
    CandidateWindowData immWineCaretData = wineCaretFallbackData;
    immWineCaretData.cursorX = 0;
    immWineCaretData.cursorY = 0;
    immWineCaretData.cursorWidth = 0;
    immWineCaretData.cursorHeight = 0;
    immWineCaretData.cursorRectResolved = false;
    immWineCaretData.wineHasImmContext = true;
    resolveCursorRect(immWineCaretData, hidpiMonitor);
    if (immWineCaretData.cursorX != 1047 || immWineCaretData.cursorY != 861) {
        std::cerr << "a Wine control with IMM support should use its client-rendered caret directly\n";
        return false;
    }
    if (wineNeedsInlinePreedit(immWineCaretData)) {
        std::cerr << "a Wine control with IMM support should render its own preedit\n";
        return false;
    }

    data.expanded = true;
    data.candidates.assign(40, "候选");
    if (estimatedWindowWidth(data) < expandedMinWindowWidth || estimatedWindowWidth(data) > expandedMaxWindowWidth) {
        std::cerr << "expanded candidate window width should stay within configured bounds\n";
        return false;
    }
    data.cursorX = 1240;
    data.cursorY = 700;
    data.cursorWidth = 2;
    data.cursorHeight = 18;
    auto position = computeWindowPosition(data, geometry);
    if (position.left < layerWindowHorizontalGuard ||
        position.left + estimatedWindowWidth(data) > geometry.width - layerWindowHorizontalGuard ||
        position.top < layerWindowTopGuard ||
        position.top + positioningWindowHeight(data) > geometry.height - layerWindowBottomGuard) {
        std::cerr << "bottom-right cursor position should stay inside the monitor\n";
        return false;
    }
    geometry = {0, 0, 1560, 1040};
    data.expanded = false;
    data.candidates = {"这个候选比较长", "第二个候选", "第三个候选"};
    data.cursorX = 32;
    data.cursorY = 901;
    data.cursorWidth = 21;
    data.cursorHeight = 24;
    position = computeWindowPosition(data, geometry);
    if (position.top >= data.cursorY ||
        position.top + positioningWindowHeight(data) > geometry.height - layerWindowBottomGuard) {
        std::cerr << "candidate window near the lower edge should flip above the cursor\n";
        return false;
    }
    data.cursorX = 1510;
    data.cursorY = 320;
    data.cursorWidth = 18;
    data.cursorHeight = 24;
    position = computeWindowPosition(data, geometry);
    if (position.left < layerWindowHorizontalGuard ||
        position.left + effectiveWindowWidth(data, geometry) > geometry.width - layerWindowHorizontalGuard ||
        position.left + effectiveWindowWidth(data, geometry) != data.cursorX + data.cursorWidth) {
        std::cerr << "candidate window near the right edge should flip left and remain attached to the cursor\n";
        return false;
    }
    const auto firstRightEdgePosition = position;
    data.cursorX += 8;
    position = computeWindowPosition(data, geometry);
    if (position.left != firstRightEdgePosition.left + 8) {
        std::cerr << "a right-aligned candidate window should keep following a moving edge cursor\n";
        return false;
    }
    geometry = {0, 0, 1280, 720};
    GdkRectangle narrowGeometry{0, 0, 480, 720};
    data.cursorX = 470;
    data.cursorY = 360;
    position = computeWindowPosition(data, narrowGeometry);
    if (effectiveWindowWidth(data, narrowGeometry) > narrowGeometry.width - layerWindowHorizontalGuard ||
        position.left < layerWindowHorizontalGuard ||
        position.left + effectiveWindowWidth(data, narrowGeometry) > narrowGeometry.width - layerWindowHorizontalGuard) {
        std::cerr << "candidate window should shrink and clamp on narrow monitors\n";
        return false;
    }

    data.expanded = false;
    data.candidates.assign(2, "候选");
    if (estimatedWindowWidth(data) >= 480 || estimatedWindowWidth(data) < collapsedMinWindowWidth) {
        std::cerr << "collapsed short candidate window should hug content within minimum width\n";
        return false;
    }
    data.candidates.assign(6, "很长很长很长很长的候选");
    if (estimatedWindowWidth(data) < 420 || estimatedWindowWidth(data) > 560) {
        std::cerr << "collapsed long candidate window should merge visual cells without expanding to full width\n";
        return false;
    }
    const auto collapsedCells = collapsedCellsFor(data);
    if (collapsedCells.size() != 2 || collapsedCells[0].span != 3 || collapsedCells[0].column != 0 ||
        collapsedCells[1].span != 3 || collapsedCells[1].column != 3) {
        std::cerr << "collapsed long candidates should occupy enough visual cells without losing digit order\n";
        return false;
    }
    data.candidates = {"很长很长很长很长很长很长很长很长很长",
                       "很长很长很长很长很长很长很长很长很长", "较短候选项"};
    const auto gapFillCells = collapsedCellsFor(data);
    if (gapFillCells.size() != 2 || gapFillCells[0].index != 0 || gapFillCells[0].span != 4 ||
        gapFillCells[1].index != 2 || gapFillCells[1].span != 2) {
        std::cerr << "collapsed candidate window should backfill first-row gaps like expanded layout\n";
        return false;
    }
    data.candidates.assign(2, "候选");
    data.cursorX = 1;
    data.cursorY = 1;
    data.cursorWidth = 1;
    data.cursorHeight = 16;
    position = computeWindowPosition(data, geometry);
    if (position.left < layerWindowHorizontalGuard || position.top < layerWindowTopGuard) {
        std::cerr << "top-left cursor position should be clamped inside the monitor\n";
        return false;
    }

    data.expanded = true;
    data.candidates.assign(40, "候选");
    data.cursorX = 600;
    data.cursorY = 200;
    data.cursorWidth = 2;
    data.cursorHeight = 18;
    if (!tipe::tipeUIPopupEdgeFallbackNeededForRect(data.cursorX, data.cursorY, data.cursorWidth, data.cursorHeight,
                                                   tipe::tipeUIPopupEdgeFallbackLeftThreshold,
                                                   tipe::tipeUIPopupEdgeFallbackTopThreshold,
                                                   estimatedWindowWidth(data), estimatedWindowHeight(data))) {
        std::cerr << "wide popup should trigger edge fallback before overflowing the right safe edge\n";
        return false;
    }
    if (!tipe::tipeUIPopupEdgeFallbackNeededForRect(32, 901, 21, 24,
                                                   tipe::tipeUIPopupEdgeFallbackLeftThreshold,
                                                   tipe::tipeUIPopupEdgeFallbackTopThreshold,
                                                   estimatedWindowWidth(data), estimatedWindowHeight(data))) {
        std::cerr << "wayland popup layer fallback should protect the bottom edge\n";
        return false;
    }
    if (!tipe::tipeUIPopupEdgeFallbackNeededForRect(980, 180, 21, 24,
                                                   tipe::tipeUIPopupEdgeFallbackLeftThreshold,
                                                   tipe::tipeUIPopupEdgeFallbackTopThreshold,
                                                   estimatedWindowWidth(data), estimatedWindowHeight(data))) {
        std::cerr << "wayland popup layer fallback should protect the right edge\n";
        return false;
    }

    data.cursorX = 0;
    data.cursorY = 0;
    data.cursorWidth = 0;
    data.cursorHeight = 0;
    position = computeWindowPosition(data, geometry);
    if (position.left < layerWindowHorizontalGuard || position.left > 900 ||
        position.top < layerWindowTopGuard ||
        position.top + positioningWindowHeight(data) > geometry.height - layerWindowBottomGuard) {
        std::cerr << "fallback candidate position should stay inside the monitor\n";
        return false;
    }
    if (hasUsableCursorRect(data)) {
        std::cerr << "zero cursor rectangle should not be treated as a real monitor point\n";
        return false;
    }
    data.cursorX = 38;
    data.cursorY = 38;
    if (hasUsableCursorRect(data)) {
        std::cerr << "position-only frontend sentinel should not be treated as a text cursor rectangle\n";
        return false;
    }

    geometry = {1280, 0, 1280, 720};
    data.expanded = true;
    data.candidates.assign(40, "候选");
    data.cursorX = 2520;
    data.cursorY = 700;
    data.cursorWidth = 2;
    data.cursorHeight = 18;
    position = computeWindowPosition(data, geometry);
    if (position.left < layerWindowHorizontalGuard ||
        position.left + estimatedWindowWidth(data) > geometry.width - layerWindowHorizontalGuard ||
        position.top < layerWindowTopGuard ||
        position.top + positioningWindowHeight(data) > geometry.height - layerWindowBottomGuard) {
        std::cerr << "right-hand monitor cursor position should use monitor-relative layer margins\n";
        return false;
    }

    geometry = {-1280, 0, 1280, 720};
    data.expanded = false;
    data.candidates.assign(2, "候选");
    data.cursorX = -1279;
    data.cursorY = 1;
    data.cursorWidth = 1;
    data.cursorHeight = 16;
    position = computeWindowPosition(data, geometry);
    if (position.left < layerWindowHorizontalGuard || position.top < layerWindowTopGuard) {
        std::cerr << "left-hand monitor cursor position should use monitor-relative layer margins\n";
        return false;
    }

    geometry = {0, 0, 320, 180};
    position = computeFixedWindowPosition(tipe::tipeUIStatusPopupWidth, positioningStatusHeight(), geometry, 920, 920,
                                          "TIPE_STATUS_LEFT", "TIPE_STATUS_TOP");
    if (position.left < layerWindowHorizontalGuard || position.top < layerWindowTopGuard ||
        position.left + tipe::tipeUIStatusPopupWidth > geometry.width - layerWindowHorizontalGuard ||
        position.top + positioningStatusHeight() > geometry.height - layerWindowBottomGuard) {
        std::cerr << "status indicator should stay inside small monitors\n";
        return false;
    }
    position = computeCursorAnchoredWindowPosition(tipe::tipeUIStatusPopupWidth, positioningStatusHeight(), 300, 160,
                                                   2, 18, geometry);
    if (position.left < layerWindowHorizontalGuard || position.top < layerWindowTopGuard ||
        position.left + tipe::tipeUIStatusPopupWidth > geometry.width - layerWindowHorizontalGuard ||
        position.top + positioningStatusHeight() > geometry.height - layerWindowBottomGuard) {
        std::cerr << "status indicator should clamp when following a bottom-right cursor\n";
        return false;
    }
    geometry = {-1280, 0, 1280, 720};
    position = computeCursorAnchoredWindowPosition(tipe::tipeUIStatusPopupWidth, positioningStatusHeight(), -1279,
                                                   700, 2, 18, geometry);
    if (position.left < layerWindowHorizontalGuard || position.top < layerWindowTopGuard ||
        position.left + tipe::tipeUIStatusPopupWidth > geometry.width - layerWindowHorizontalGuard ||
        position.top + positioningStatusHeight() > geometry.height - layerWindowBottomGuard) {
        std::cerr << "status indicator should use monitor-relative margins on left-hand monitors\n";
        return false;
    }
    geometry = {0, 0, 1560, 1040};
    position = computeCursorAnchoredWindowPosition(tipe::tipeUIStatusPopupWidth, positioningStatusHeight(), 32, 948,
                                                   21, 24, geometry);
    if (position.top != 948 - positioningStatusHeight() - 4 || position.top >= 948 ||
        position.top + positioningStatusHeight() > geometry.height - layerWindowBottomGuard) {
        std::cerr << "status indicator near the lower edge should flip above the cursor\n";
        return false;
    }
    data.expanded = false;
    data.candidates = {"这个候选比较长", "第二个候选", "第三个候选"};
    data.cursorX = 32;
    data.cursorY = 901;
    data.cursorWidth = 21;
    data.cursorHeight = 24;
    position = computeWindowPosition(data, geometry);
    if (position.top != data.cursorY - positioningWindowHeight(data) - 4 || position.top >= data.cursorY ||
        position.top + positioningWindowHeight(data) > geometry.height - layerWindowBottomGuard) {
        std::cerr << "candidate window near the lower edge should use the measured popup height when flipping\n";
        return false;
    }

    const char *oldXdgCacheHome = std::getenv("XDG_CACHE_HOME");
    const std::string savedXdgCacheHome = oldXdgCacheHome ? oldXdgCacheHome : "";
    setenv("XDG_CACHE_HOME", "/tmp/tipe-candidate-window-self-test-cache", 1);
    data.statusMode = false;
    if (fallbackPidPath(data) != "/tmp/tipe-candidate-window-self-test-cache/tipe/candidate-window.pid") {
        std::cerr << "candidate fallback pid path should honor XDG_CACHE_HOME\n";
        if (oldXdgCacheHome) {
            setenv("XDG_CACHE_HOME", savedXdgCacheHome.c_str(), 1);
        } else {
            unsetenv("XDG_CACHE_HOME");
        }
        return false;
    }
    data.statusMode = true;
    if (fallbackPidPath(data) != "/tmp/tipe-candidate-window-self-test-cache/tipe/status-window.pid") {
        std::cerr << "status fallback pid path should honor XDG_CACHE_HOME\n";
        if (oldXdgCacheHome) {
            setenv("XDG_CACHE_HOME", savedXdgCacheHome.c_str(), 1);
        } else {
            unsetenv("XDG_CACHE_HOME");
        }
        return false;
    }
    if (oldXdgCacheHome) {
        setenv("XDG_CACHE_HOME", savedXdgCacheHome.c_str(), 1);
    } else {
        unsetenv("XDG_CACHE_HOME");
    }
    char boundedLogPath[] = "/tmp/tipe-candidate-log-self-test-XXXXXX";
    const int boundedLogFd = mkstemp(boundedLogPath);
    struct stat boundedLogStatus {};
    const bool boundedLogOk =
        boundedLogFd >= 0 &&
        ftruncate(boundedLogFd, static_cast<off_t>(tipe::diagnosticLogMaxBytes + 1)) == 0 &&
        tipe::trimOpenDiagnosticLog(boundedLogFd) && fstat(boundedLogFd, &boundedLogStatus) == 0 &&
        boundedLogStatus.st_size == 0;
    if (boundedLogFd >= 0) {
        close(boundedLogFd);
    }
    std::remove(boundedLogPath);
    if (!boundedLogOk) {
        std::cerr << "open fallback diagnostic log should be truncated at the bounded size\n";
        return false;
    }
    return true;
}

void activate(GtkApplication *app, gpointer userData) {
    auto *data = static_cast<CandidateWindowData *>(userData);

    data->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(data->window), data->statusMode ? "TiPE 状态" : "TiPE 候选");
    gtk_window_set_icon_name(GTK_WINDOW(data->window), "tipe");
    gtk_window_set_decorated(GTK_WINDOW(data->window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(data->window), FALSE);
    gtk_widget_set_focusable(data->window, FALSE);
    gtk_window_set_default_size(GTK_WINDOW(data->window),
                                data->statusMode ? tipe::tipeUIStatusPopupWidth : estimatedWindowWidth(*data), -1);

    if (gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(data->window));
        gtk_layer_set_namespace(GTK_WINDOW(data->window), data->statusMode ? "tipe-status-window" : "tipe-candidate-window");
        gtk_layer_set_layer(GTK_WINDOW(data->window), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(data->window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_anchor(GTK_WINDOW(data->window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(data->window), GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
        gtk_layer_set_anchor(GTK_WINDOW(data->window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(data->window), GTK_LAYER_SHELL_EDGE_RIGHT, FALSE);
        if (!data->statusMode) {
            gtk_layer_set_margin(GTK_WINDOW(data->window), GTK_LAYER_SHELL_EDGE_TOP,
                                 std::clamp(envInt("TIPE_CANDIDATE_TOP").value_or(920), 12, 1200));
            gtk_layer_set_margin(GTK_WINDOW(data->window), GTK_LAYER_SHELL_EDGE_LEFT,
                                 envInt("TIPE_CANDIDATE_LEFT").value_or(64));
        }
    }

    installCss();

    if (!data->statusMode && gtk_layer_is_supported()) {
        data->inlinePreeditWindow = gtk_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(data->inlinePreeditWindow), "TiPE 预编辑");
        gtk_window_set_icon_name(GTK_WINDOW(data->inlinePreeditWindow), "tipe");
        gtk_window_set_decorated(GTK_WINDOW(data->inlinePreeditWindow), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(data->inlinePreeditWindow), FALSE);
        gtk_widget_set_focusable(data->inlinePreeditWindow, FALSE);
        gtk_layer_init_for_window(GTK_WINDOW(data->inlinePreeditWindow));
        gtk_layer_set_namespace(GTK_WINDOW(data->inlinePreeditWindow), "tipe-wine-inline-preedit");
        gtk_layer_set_layer(GTK_WINDOW(data->inlinePreeditWindow), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(data->inlinePreeditWindow), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_layer_set_anchor(GTK_WINDOW(data->inlinePreeditWindow), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(data->inlinePreeditWindow), GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
        gtk_layer_set_anchor(GTK_WINDOW(data->inlinePreeditWindow), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(data->inlinePreeditWindow), GTK_LAYER_SHELL_EDGE_RIGHT, FALSE);
        data->inlinePreeditPanel = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(data->inlinePreeditPanel), drawWineInlinePreedit, data,
                                       nullptr);
        gtk_window_set_child(GTK_WINDOW(data->inlinePreeditWindow), data->inlinePreeditPanel);
        makeWindowInputTransparent(data->inlinePreeditWindow);
        gtk_widget_set_visible(data->inlinePreeditWindow, FALSE);
    }

    data->panel = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(data->panel), drawPanel, data, nullptr);
    auto *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(pointerMotion), data);
    g_signal_connect(motion, "leave", G_CALLBACK(pointerLeave), data);
    gtk_widget_add_controller(data->panel, motion);
    auto *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(pointerPressed), data);
    gtk_widget_add_controller(data->panel, GTK_EVENT_CONTROLLER(click));
    gtk_window_set_child(GTK_WINDOW(data->window), data->panel);

    if (data->statusMode) {
        updateStatusLayerPosition(*data);
        gtk_widget_queue_draw(data->panel);
        gtk_widget_set_visible(data->window, TRUE);
        gtk_window_present(GTK_WINDOW(data->window));
        g_timeout_add(static_cast<guint>(data->ttlMs > 0 ? data->ttlMs : 1000), closeStatusWindow, data->window);
    } else if (data->stdinMode) {
        auto *channel = g_io_channel_unix_new(STDIN_FILENO);
        g_io_channel_set_encoding(channel, nullptr, nullptr);
        g_io_add_watch(channel, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL), stdinReady, data);
        g_io_channel_unref(channel);
        gtk_widget_set_visible(data->window, FALSE);
    } else {
        render(*data);
        if (data->ttlMs > 0) {
            g_timeout_add(static_cast<guint>(data->ttlMs), closeStatusWindow, data->window);
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);
    auto data = parseArgs(argc, argv);
    if (data.argumentError) {
        return 2;
    }
    if (data.selfTest) {
        return selfTest() ? 0 : 1;
    }
    if (data.parseSnapshot) {
        if (data.snapshotLine == "-") {
            if (!std::getline(std::cin, data.snapshotLine)) {
                return 1;
            }
        }
        return printParsedSnapshot(data.snapshotLine, data.layoutGeometry) ? 0 : 1;
    }
    replaceExistingFallbackInstance(data);
    auto args = gtkArgs(argc, argv);
    int gtkArgc = static_cast<int>(args.size());
    auto *app = gtk_application_new("dev.tipe.CandidateWindow", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &data);
    const int status = g_application_run(G_APPLICATION(app), gtkArgc, args.data());
    g_object_unref(app);
    clearFallbackPid(data);
    return status;
}
