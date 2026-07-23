#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imm.h>
#include <initguid.h>
#include <oleacc.h>
#include <oleauto.h>
#include <uiautomationclient.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct CaretResult {
    std::string source;
    RECT rect{};
};

using Diagnostics = std::vector<std::string>;

std::string handleText(HWND window) {
    return std::to_string(static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(window)));
}

std::string rectText(const RECT &rect) {
    return std::to_string(rect.left) + "," + std::to_string(rect.top) + "," +
           std::to_string(rect.right) + "," + std::to_string(rect.bottom);
}

std::string hresultText(HRESULT result) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(result));
    return text;
}

bool usableRect(const RECT &rect) {
    const int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualRight = virtualLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualBottom = virtualTop + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rect.left >= virtualLeft && rect.left < virtualRight && rect.top >= virtualTop &&
           rect.top < virtualBottom && rect.right >= rect.left && rect.bottom >= rect.top;
}

RECT normalizedCaretRect(RECT rect) {
    if (rect.right <= rect.left) {
        rect.right = rect.left + 2;
    }
    if (rect.bottom <= rect.top) {
        rect.bottom = rect.top + 22;
    }
    return rect;
}

bool clientRectToScreen(HWND window, RECT &rect) {
    if (!window || !IsWindow(window)) {
        return false;
    }
    POINT points[2]{{rect.left, rect.top}, {rect.right, rect.bottom}};
    if (!ClientToScreen(window, &points[0]) || !ClientToScreen(window, &points[1])) {
        return false;
    }
    rect = {points[0].x, points[0].y, points[1].x, points[1].y};
    return true;
}

void addResult(std::vector<CaretResult> &results, const char *source, RECT rect) {
    rect = normalizedCaretRect(rect);
    if (!usableRect(rect)) {
        return;
    }
    const auto duplicate = std::find_if(results.begin(), results.end(), [&](const auto &result) {
        return result.source == source && result.rect.left == rect.left && result.rect.top == rect.top &&
               result.rect.right == rect.right && result.rect.bottom == rect.bottom;
    });
    if (duplicate == results.end()) {
        results.push_back({source, rect});
    }
}

void addRawResult(std::vector<CaretResult> &results, const char *source, RECT rect) {
    rect = normalizedCaretRect(rect);
    const auto duplicate = std::find_if(results.begin(), results.end(), [&](const auto &result) {
        return result.source == source && result.rect.left == rect.left && result.rect.top == rect.top &&
               result.rect.right == rect.right && result.rect.bottom == rect.bottom;
    });
    if (duplicate == results.end()) {
        results.push_back({source, rect});
    }
}

void addUIAutomationRange(std::vector<CaretResult> &results, Diagnostics &diagnostics,
                          const char *source, IUIAutomationTextRange *range) {
    IUIAutomationTextRange *expanded = nullptr;
    const HRESULT cloneResult = range->Clone(&expanded);
    diagnostics.push_back(std::string(source) + "-clone=" + hresultText(cloneResult));
    if (FAILED(cloneResult) || !expanded) {
        return;
    }

    const HRESULT expandResult = expanded->ExpandToEnclosingUnit(TextUnit_Character);
    SAFEARRAY *bounds = nullptr;
    const HRESULT boundsResult = expanded->GetBoundingRectangles(&bounds);
    diagnostics.push_back(std::string(source) + "-bounds=" + hresultText(boundsResult) +
                          " expand=" + hresultText(expandResult));
    if (SUCCEEDED(boundsResult) && bounds) {
        LONG lower = 0;
        LONG upper = -1;
        if (SUCCEEDED(SafeArrayGetLBound(bounds, 1, &lower)) &&
            SUCCEEDED(SafeArrayGetUBound(bounds, 1, &upper))) {
            diagnostics.push_back(std::string(source) + "-bound-count=" +
                                  std::to_string(upper - lower + 1));
            if (upper - lower + 1 >= 4) {
                std::array<double, 4> values{};
                for (LONG index = 0; index < 4; ++index) {
                    LONG offset = lower + index;
                    SafeArrayGetElement(bounds, &offset,
                                        &values[static_cast<std::size_t>(index)]);
                }
                RECT rect{static_cast<LONG>(std::lround(values[0])),
                          static_cast<LONG>(std::lround(values[1])),
                          static_cast<LONG>(std::lround(values[0] + values[2])),
                          static_cast<LONG>(std::lround(values[1] + values[3]))};
                addResult(results, source, rect);
            }
        }
        SafeArrayDestroy(bounds);
    }
    expanded->Release();
}

void queryUIAutomation(std::vector<CaretResult> &results, Diagnostics &diagnostics) {
    IUIAutomation *automation = nullptr;
    const HRESULT createResult =
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                         reinterpret_cast<void **>(&automation));
    diagnostics.push_back("uia-create=" + hresultText(createResult));
    if (FAILED(createResult) || !automation) {
        return;
    }

    IUIAutomationElement *focused = nullptr;
    const HRESULT focusResult = automation->GetFocusedElement(&focused);
    diagnostics.push_back("uia-focused-element=" + hresultText(focusResult));
    if (SUCCEEDED(focusResult) && focused) {
        int processId = 0;
        int controlType = 0;
        BOOL textAvailable = FALSE;
        BOOL text2Available = FALSE;
        UIA_HWND nativeWindow = nullptr;
        RECT bounds{};
        focused->get_CurrentProcessId(&processId);
        focused->get_CurrentControlType(&controlType);
        focused->get_CurrentNativeWindowHandle(&nativeWindow);
        focused->get_CurrentBoundingRectangle(&bounds);
        VARIANT textProperty{};
        if (SUCCEEDED(focused->GetCurrentPropertyValue(UIA_IsTextPatternAvailablePropertyId,
                                                       &textProperty)) &&
            textProperty.vt == VT_BOOL) {
            textAvailable = textProperty.boolVal == VARIANT_TRUE;
        }
        VariantClear(&textProperty);
        VARIANT text2Property{};
        if (SUCCEEDED(focused->GetCurrentPropertyValue(UIA_IsTextPattern2AvailablePropertyId,
                                                       &text2Property)) &&
            text2Property.vt == VT_BOOL) {
            text2Available = text2Property.boolVal == VARIANT_TRUE;
        }
        VariantClear(&text2Property);
        diagnostics.push_back("uia-element pid=" + std::to_string(processId) +
                              " type=" + std::to_string(controlType) +
                              " hwnd=" + handleText(reinterpret_cast<HWND>(nativeWindow)) +
                              " bounds=" + rectText(bounds) +
                              " text=" + std::to_string(textAvailable != FALSE) +
                              " text2=" + std::to_string(text2Available != FALSE));

        IUIAutomationTextPattern2 *pattern = nullptr;
        const HRESULT pattern2Result =
            focused->GetCurrentPatternAs(UIA_TextPattern2Id, IID_IUIAutomationTextPattern2,
                                         reinterpret_cast<void **>(&pattern));
        diagnostics.push_back("uia-text2-pattern=" + hresultText(pattern2Result));
        if (SUCCEEDED(pattern2Result) && pattern) {
            BOOL active = FALSE;
            IUIAutomationTextRange *range = nullptr;
            const HRESULT caretResult = pattern->GetCaretRange(&active, &range);
            diagnostics.push_back("uia-text2-caret=" + hresultText(caretResult) +
                                  " active=" + std::to_string(active != FALSE));
            if (SUCCEEDED(caretResult) && active && range) {
                addUIAutomationRange(results, diagnostics, "uia-text2-caret", range);
                range->Release();
            }
            pattern->Release();
        }

        IUIAutomationTextPattern *textPattern = nullptr;
        const HRESULT textPatternResult =
            focused->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                         reinterpret_cast<void **>(&textPattern));
        diagnostics.push_back("uia-text-pattern=" + hresultText(textPatternResult));
        if (SUCCEEDED(textPatternResult) && textPattern) {
            IUIAutomationTextRangeArray *selection = nullptr;
            const HRESULT selectionResult = textPattern->GetSelection(&selection);
            diagnostics.push_back("uia-text-selection=" + hresultText(selectionResult));
            if (SUCCEEDED(selectionResult) && selection) {
                int length = 0;
                selection->get_Length(&length);
                diagnostics.push_back("uia-text-selection-count=" + std::to_string(length));
                if (length > 0) {
                    IUIAutomationTextRange *range = nullptr;
                    if (SUCCEEDED(selection->GetElement(0, &range)) && range) {
                        addUIAutomationRange(results, diagnostics, "uia-text-selection", range);
                        range->Release();
                    }
                }
                selection->Release();
            }
            textPattern->Release();
        }
        focused->Release();
    }
    automation->Release();
}

GUITHREADINFO foregroundThreadInfo(Diagnostics &diagnostics) {
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    const HWND foreground = GetForegroundWindow();
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(foreground, &processId);
    char title[256]{};
    char className[256]{};
    GetWindowTextA(foreground, title, static_cast<int>(std::size(title)));
    GetClassNameA(foreground, className, static_cast<int>(std::size(className)));
    diagnostics.push_back("foreground hwnd=" + handleText(foreground) +
                          " thread=" + std::to_string(threadId) +
                          " pid=" + std::to_string(processId) + " class=" + className +
                          " title=" + title);
    const BOOL success = GetGUIThreadInfo(threadId, &info);
    diagnostics.push_back("gui-thread-info ok=" + std::to_string(success != FALSE) +
                          " error=" + std::to_string(GetLastError()) +
                          " active=" + handleText(info.hwndActive) +
                          " focus=" + handleText(info.hwndFocus) +
                          " caret=" + handleText(info.hwndCaret) +
                          " caret-rect=" + rectText(info.rcCaret));
    return info;
}

void queryGuiCaret(std::vector<CaretResult> &results, Diagnostics &diagnostics,
                   const GUITHREADINFO &info) {
    if (!info.hwndCaret) {
        diagnostics.push_back("gui-thread-caret=missing");
        return;
    }
    auto rect = info.rcCaret;
    const bool converted = clientRectToScreen(info.hwndCaret, rect);
    diagnostics.push_back("gui-thread-caret converted=" + std::to_string(converted) +
                          " rect=" + rectText(rect));
    if (converted) {
        addResult(results, "gui-thread-caret", rect);
    }
}

void queryAttachedCaret(std::vector<CaretResult> &results, Diagnostics &diagnostics,
                        const GUITHREADINFO &info) {
    HWND focus = info.hwndFocus ? info.hwndFocus : GetForegroundWindow();
    if (!focus) {
        diagnostics.push_back("attached-thread-caret=no-focus");
        return;
    }
    const DWORD targetThread = GetWindowThreadProcessId(focus, nullptr);
    const DWORD currentThread = GetCurrentThreadId();
    const bool attached = targetThread && targetThread != currentThread &&
                          AttachThreadInput(currentThread, targetThread, TRUE);
    POINT point{};
    HWND attachedFocus = GetFocus();
    HWND caretWindow = info.hwndCaret ? info.hwndCaret : (attachedFocus ? attachedFocus : focus);
    const BOOL success = GetCaretPos(&point);
    diagnostics.push_back("attached-thread-caret attached=" + std::to_string(attached) +
                          " focus=" + handleText(attachedFocus) +
                          " window=" + handleText(caretWindow) +
                          " ok=" + std::to_string(success != FALSE) +
                          " point=" + std::to_string(point.x) + "," + std::to_string(point.y) +
                          " error=" + std::to_string(GetLastError()));
    if (success) {
        RECT rect{point.x, point.y, point.x + 2, point.y + 22};
        if (clientRectToScreen(caretWindow, rect)) {
            addResult(results, "attached-thread-caret", rect);
        }
    }
    if (attached) {
        AttachThreadInput(currentThread, targetThread, FALSE);
    }
}

void queryImm(std::vector<CaretResult> &results, Diagnostics &diagnostics,
              const GUITHREADINFO &info) {
    HWND focus = info.hwndFocus ? info.hwndFocus : GetForegroundWindow();
    if (!focus) {
        diagnostics.push_back("imm=no-focus");
        return;
    }
    HIMC context = ImmGetContext(focus);
    diagnostics.push_back("imm-context=" +
                          std::to_string(static_cast<unsigned long long>(
                              reinterpret_cast<std::uintptr_t>(context))));
    if (!context) {
        return;
    }

    COMPOSITIONFORM composition{};
    const BOOL compositionResult = ImmGetCompositionWindow(context, &composition);
    diagnostics.push_back("imm-composition ok=" + std::to_string(compositionResult != FALSE) +
                          " style=" + std::to_string(composition.dwStyle) +
                          " point=" + std::to_string(composition.ptCurrentPos.x) + "," +
                          std::to_string(composition.ptCurrentPos.y) +
                          " area=" + rectText(composition.rcArea));
    if (compositionResult) {
        RECT rect{composition.ptCurrentPos.x, composition.ptCurrentPos.y,
                  composition.ptCurrentPos.x + 2, composition.ptCurrentPos.y + 22};
        if (clientRectToScreen(focus, rect)) {
            addResult(results, "imm-composition", rect);
        }
    }

    for (DWORD index = 0; index < 4; ++index) {
        CANDIDATEFORM candidate{};
        candidate.dwIndex = index;
        const BOOL candidateResult = ImmGetCandidateWindow(context, index, &candidate);
        diagnostics.push_back("imm-candidate-" + std::to_string(index) +
                              " ok=" + std::to_string(candidateResult != FALSE) +
                              " style=" + std::to_string(candidate.dwStyle) +
                              " point=" + std::to_string(candidate.ptCurrentPos.x) + "," +
                              std::to_string(candidate.ptCurrentPos.y) +
                              " area=" + rectText(candidate.rcArea));
        if (!candidateResult) {
            continue;
        }
        RECT rect{candidate.ptCurrentPos.x, candidate.ptCurrentPos.y,
                  candidate.ptCurrentPos.x + 2, candidate.ptCurrentPos.y + 22};
        if (clientRectToScreen(focus, rect)) {
            addResult(results, ("imm-candidate-" + std::to_string(index)).c_str(), rect);
        }
    }
    ImmReleaseContext(focus, context);
}

void queryMsaaCaret(std::vector<CaretResult> &results, Diagnostics &diagnostics,
                    const char *source, HWND window) {
    if (!window) {
        diagnostics.push_back(std::string(source) + "=no-window");
        return;
    }
    IAccessible *accessible = nullptr;
    const HRESULT objectResult =
        AccessibleObjectFromWindow(window, OBJID_CARET, IID_IAccessible,
                                   reinterpret_cast<void **>(&accessible));
    diagnostics.push_back(std::string(source) + "-object=" + hresultText(objectResult));
    if (FAILED(objectResult) || !accessible) {
        return;
    }
    LONG left = 0;
    LONG top = 0;
    LONG width = 0;
    LONG height = 0;
    VARIANT child{};
    child.vt = VT_I4;
    child.lVal = CHILDID_SELF;
    const HRESULT locationResult = accessible->accLocation(&left, &top, &width, &height, child);
    diagnostics.push_back(std::string(source) + "-location=" + hresultText(locationResult) +
                          " rect=" + std::to_string(left) + "," + std::to_string(top) + "," +
                          std::to_string(width) + "," + std::to_string(height));
    if (SUCCEEDED(locationResult)) {
        // Wine may expose MSAA bounds in Xwayland physical pixels while Win32
        // screen metrics are logical pixels. Preserve the raw value for the
        // Linux-side bridge, which knows the actual root-to-GTK scale.
        addRawResult(results, source, {left, top, left + width, top + height});
    }
    accessible->Release();
}

void writeResults(const std::vector<CaretResult> &results, const Diagnostics &diagnostics) {
    std::string output;
    for (const auto &diagnostic : diagnostics) {
        output += "diag\t" + diagnostic + "\n";
    }
    if (results.empty()) {
        output += "none\n";
    } else {
        for (const auto &result : results) {
            output += result.source + "\t" + std::to_string(result.rect.left) + "\t" +
                      std::to_string(result.rect.top) + "\t" +
                      std::to_string(result.rect.right - result.rect.left) + "\t" +
                      std::to_string(result.rect.bottom - result.rect.top) + "\n";
        }
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<CaretResult> results;
    Diagnostics diagnostics;
    diagnostics.push_back("com-init=" + hresultText(initialized));
    diagnostics.push_back("virtual-screen=" +
                          std::to_string(GetSystemMetrics(SM_XVIRTUALSCREEN)) + "," +
                          std::to_string(GetSystemMetrics(SM_YVIRTUALSCREEN)) + "," +
                          std::to_string(GetSystemMetrics(SM_CXVIRTUALSCREEN)) + "," +
                          std::to_string(GetSystemMetrics(SM_CYVIRTUALSCREEN)));
    queryUIAutomation(results, diagnostics);
    const auto info = foregroundThreadInfo(diagnostics);
    queryGuiCaret(results, diagnostics, info);
    queryAttachedCaret(results, diagnostics, info);
    queryImm(results, diagnostics, info);
    queryMsaaCaret(results, diagnostics, "msaa-focus-caret", info.hwndFocus);
    if (info.hwndActive != info.hwndFocus) {
        queryMsaaCaret(results, diagnostics, "msaa-active-caret", info.hwndActive);
    }
    writeResults(results, diagnostics);
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    return results.empty() ? 1 : 0;
}
