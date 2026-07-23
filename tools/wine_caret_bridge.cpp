#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imm.h>
#include <initguid.h>
#include <oleacc.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

bool plausibleCaret(LONG left, LONG top, LONG width, LONG height) {
    constexpr LONG limit = 1'000'000;
    return !(left == 0 && top == 0) && left > -limit && left < limit && top > -limit && top < limit &&
           width >= 0 && width < limit && height > 0 && height < limit;
}

bool accessibleCaretFor(HWND window, RECT &rect) {
    if (!window || !IsWindow(window)) {
        return false;
    }
    IAccessible *accessible = nullptr;
    const HRESULT objectResult =
        AccessibleObjectFromWindow(window, OBJID_CARET, IID_IAccessible,
                                   reinterpret_cast<void **>(&accessible));
    if (FAILED(objectResult) || !accessible) {
        return false;
    }

    LONG left = 0;
    LONG top = 0;
    LONG width = 0;
    LONG height = 0;
    VARIANT child{};
    child.vt = VT_I4;
    child.lVal = CHILDID_SELF;
    const HRESULT locationResult = accessible->accLocation(&left, &top, &width, &height, child);
    accessible->Release();
    if (FAILED(locationResult) || !plausibleCaret(left, top, width, height)) {
        return false;
    }
    rect = {left, top, left + (width > 0 ? width : 1), top + height};
    return true;
}

bool queryCaret(RECT &rect, HWND &focusWindow) {
    const HWND foreground = GetForegroundWindow();
    const DWORD thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (thread) {
        GetGUIThreadInfo(thread, &info);
    }
    focusWindow = info.hwndFocus ? info.hwndFocus : (info.hwndActive ? info.hwndActive : foreground);
    if (accessibleCaretFor(info.hwndFocus, rect)) {
        return true;
    }
    if (info.hwndActive != info.hwndFocus && accessibleCaretFor(info.hwndActive, rect)) {
        return true;
    }
    return foreground != info.hwndFocus && foreground != info.hwndActive &&
           accessibleCaretFor(foreground, rect);
}

bool writeAll(HANDLE output, const char *data, DWORD size) {
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(output, data, size, &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

bool answerQuery(HANDLE output, unsigned long long serial) {
    RECT rect{};
    HWND focusWindow = nullptr;
    char line[160]{};
    int length = 0;
    const bool hasCaret = queryCaret(rect, focusWindow);
    bool hasImmContext = false;
    if (focusWindow) {
        if (const HIMC context = ImmGetContext(focusWindow)) {
            hasImmContext = true;
            ImmReleaseContext(focusWindow, context);
        }
    }
    if (hasCaret) {
        length = std::snprintf(line, sizeof(line), "caret\t%llu\t%ld\t%ld\t%ld\t%ld\t%d\n", serial,
                               rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                               hasImmContext ? 1 : 0);
    } else {
        length = std::snprintf(line, sizeof(line), "none\t%llu\t%d\n", serial, hasImmContext ? 1 : 0);
    }
    return length > 0 && static_cast<std::size_t>(length) < sizeof(line) &&
           writeAll(output, line, static_cast<DWORD>(length));
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR commandLine, int) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) {
        return 2;
    }

    if (commandLine && std::string_view(commandLine) == "--once") {
        const bool ok = answerQuery(output, 0);
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
        return ok ? 0 : 3;
    }

    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE) {
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
        return 2;
    }

    char line[96]{};
    std::size_t length = 0;
    char byte = 0;
    DWORD read = 0;
    while (ReadFile(input, &byte, 1, &read, nullptr) && read == 1) {
        if (byte != '\n' && length + 1 < sizeof(line)) {
            line[length++] = byte;
            continue;
        }
        line[length] = '\0';
        char *end = nullptr;
        const auto serial = std::strtoull(line, &end, 10);
        if (end != line && *end == '\0' && !answerQuery(output, serial)) {
            break;
        }
        length = 0;
    }

    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    return 0;
}
