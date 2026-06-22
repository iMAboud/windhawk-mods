// ==WindhawkMod==
// @id              taskbar-brightness-control
// @name            Taskbar Brightness Control
// @description     Control monitor brightness by scrolling over the taskbar
// @version         1.0.0
// @author          iMAboud
// @github          https://github.com/iMAboud
// @include         explorer.exe
// @compilerOptions -ldwmapi -lgdi32 -lole32 -lversion -ldxva2
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- syncMonitors: false
  $name: Sync all monitors
  $description: Scrolling will adjust the brightness of all connected monitors.
- brightnessChangeStep: 5
  $name: Brightness change step
  $description: Brightness step change with each scroll.
- modifierKey: none
  $name: Modifier key
  $description: Require holding a modifier key to change brightness.
  $options:
  - none: None
  - shift: Shift
  - ctrl: Ctrl
  - alt: Alt
  - shift_ctrl: Shift + Ctrl
  - shift_alt: Shift + Alt
  - ctrl_alt: Ctrl + Alt
- osdDelay: 1500
  $name: OSD Timeout (ms)
  $description: How long the brightness indicator stays on screen.
- allowInFullscreen: false
  $name: Allow in Fullscreen
  $description: Allow brightness scrolling in fullscreen apps.
*/
// ==/WindhawkModSettings==

// ==WindhawkModReadme==
/*
# Taskbar Brightness Control

Control your monitor's brightness by simply scrolling your mouse wheel over the Windows taskbar.

### ✨ Features
* **Hardware & Software Dimming**: Adjusts your monitor's physical hardware brightness. If you scroll past 0%, it dynamically applies a dark software filter to dim your screen even darker!
* **Multi-Monitor Sync**: Automatically sync all your connected monitors so they adjust simultaneously.
* **Change Step**: Adjust the steps it changes on each mouse scroll.
* **Dynamic OSD**: An indicator shows your exact brightness percentage.
* **Modifier Keys**: Require holding `Shift`, `Ctrl`, or `Alt` to prevent accidental adjustments.
* **Fullscreen Support**: Automatically ignores your scrolls when playing fullscreen games, with an option to force-allow it.

### ℹ Notice
This mod works by injecting directly into `explorer.exe`.

*(If your monitor does not support hardware brightness commands over DDC/CI, the mod will automatically detect this and seamlessly fall back to pure software dimming.)*
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <vector>
#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

// Settings
bool g_syncMonitors = false;
int g_brightnessChangeStep = 5;
std::wstring g_modifierKey = L"none";
int g_osdDelay = 1500;
bool g_allowInFullscreen = false;

void LoadSettings() {
    g_syncMonitors = Wh_GetIntSetting(L"syncMonitors") != 0;
    g_brightnessChangeStep = Wh_GetIntSetting(L"brightnessChangeStep");
    if (g_brightnessChangeStep <= 0) g_brightnessChangeStep = 5;
    
    PCWSTR modKey = Wh_GetStringSetting(L"modifierKey");
    g_modifierKey = modKey ? modKey : L"none";
    if (modKey) Wh_FreeStringSetting(modKey);
    
    g_osdDelay = Wh_GetIntSetting(L"osdDelay");
    if (g_osdDelay <= 0) g_osdDelay = 1500;

    g_allowInFullscreen = Wh_GetIntSetting(L"allowInFullscreen") != 0;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

// Background Thread State
std::mutex g_mutex;
int g_pendingDelta = 0;
POINT g_lastPoint = {0, 0};
std::atomic<bool> g_threadRunning(false);
HANDLE g_hWakeEvent = NULL;
std::thread* g_pWorkerThread = nullptr;

struct MonitorDimInfo {
    bool initialized = false;
    int currentCombined = 100;
    DWORD maxB = 100;
};
std::map<HMONITOR, MonitorDimInfo> g_workerDimStates;

HWND g_hHiddenWnd = NULL;
#define WM_USER_UPDATE_BRIGHTNESS (WM_USER + 100)
#define WM_USER_UPDATE_BRIGHTNESS_SW (WM_USER + 101)

// UI State
struct UIOverlayState {
    HWND hOverlay = NULL;
};
std::map<HMONITOR, UIOverlayState> g_uiOverlays;
std::map<HMONITOR, int> g_uiBrightnesses;

HWND g_hOsdWnd = NULL;
UINT_PTR g_osdTimer = 0;
std::wstring g_osdText;

// -------------------------------------------------------------
// Background Hardware Communication
// -------------------------------------------------------------
void ApplyBrightnessDeltaToMonitor(HMONITOR hMonitor, int delta, bool isTarget) {
    DWORD numPhysicalMonitors = 0;
    bool hardwareHandled = false;

    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &numPhysicalMonitors) && numPhysicalMonitors > 0) {
        std::vector<PHYSICAL_MONITOR> physicalMonitors(numPhysicalMonitors);
        if (GetPhysicalMonitorsFromHMONITOR(hMonitor, numPhysicalMonitors, physicalMonitors.data())) {
            for (DWORD i = 0; i < numPhysicalMonitors; ++i) {
                HANDLE hPhysicalMonitor = physicalMonitors[i].hPhysicalMonitor;
                
                auto& state = g_workerDimStates[hMonitor];
                if (!state.initialized) {
                    DWORD minB = 0, currentB = 0, maxB = 0;
                    if (GetMonitorBrightness(hPhysicalMonitor, &minB, &currentB, &maxB)) {
                        state.currentCombined = static_cast<int>(currentB);
                        state.maxB = maxB;
                        state.initialized = true;
                    } else {
                        continue;
                    }
                }
                hardwareHandled = true;

                int newCombined = state.currentCombined + delta;
                if (newCombined > static_cast<int>(state.maxB)) newCombined = state.maxB;
                if (newCombined < -100) newCombined = -100;

                if (newCombined != state.currentCombined) {
                    if (newCombined >= 0) {
                        SetMonitorBrightness(hPhysicalMonitor, static_cast<DWORD>(newCombined));
                    } else if (state.currentCombined > 0) {
                        SetMonitorBrightness(hPhysicalMonitor, 0); 
                    }
                    state.currentCombined = newCombined;
                }
                
                PostMessage(g_hHiddenWnd, WM_USER_UPDATE_BRIGHTNESS, MAKEWPARAM((short)newCombined, isTarget ? 1 : 0), (LPARAM)hMonitor);
            }
            DestroyPhysicalMonitors(numPhysicalMonitors, physicalMonitors.data());
        }
    }

    if (!hardwareHandled) {
        // Fallback to pure software dimming
        auto& state = g_workerDimStates[hMonitor];
        if (!state.initialized) {
            state.currentCombined = 100;
            state.initialized = true;
        }

        int newSwDim = state.currentCombined + delta;
        if (newSwDim > 100) newSwDim = 100;
        if (newSwDim < -100) newSwDim = -100;

        if (newSwDim != state.currentCombined) {
            state.currentCombined = newSwDim;
        }
        PostMessage(g_hHiddenWnd, WM_USER_UPDATE_BRIGHTNESS_SW, MAKEWPARAM((short)newSwDim, isTarget ? 1 : 0), (LPARAM)hMonitor);
    }
}

struct SyncContext {
    int delta;
    HMONITOR targetMonitor;
};

BOOL CALLBACK EnumMonitorsProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    SyncContext* ctx = (SyncContext*)dwData;
    ApplyBrightnessDeltaToMonitor(hMonitor, ctx->delta, hMonitor == ctx->targetMonitor);
    return TRUE;
}

void BrightnessWorkerThread() {
    while (g_threadRunning) {
        WaitForSingleObject(g_hWakeEvent, INFINITE);
        if (!g_threadRunning) break;

        int delta = 0;
        POINT pt;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            delta = g_pendingDelta;
            g_pendingDelta = 0;
            pt = g_lastPoint;
        }

        if (delta != 0) {
            HMONITOR hTarget = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            if (hTarget) {
                if (g_syncMonitors) {
                    SyncContext ctx = { delta, hTarget };
                    EnumDisplayMonitors(NULL, NULL, EnumMonitorsProc, (LPARAM)&ctx);
                } else {
                    ApplyBrightnessDeltaToMonitor(hTarget, delta, true);
                }
            }
        }
    }
}

// UI Rendering
LRESULT CALLBACK OsdWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        
        HBRUSH bgBrush = CreateSolidBrush(RGB(25, 25, 25));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);
        
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        
        HFONT hFont = CreateFont(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
        
        DrawText(hdc, g_osdText.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        
        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
        EndPaint(hWnd, &ps);
        return 0;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

VOID CALLBACK OsdTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    KillTimer(NULL, idEvent);
    g_osdTimer = 0;
    if (g_hOsdWnd) ShowWindow(g_hOsdWnd, SW_HIDE);
}

void ShowOsd(std::wstring text) {
    if (!g_hOsdWnd) {
        WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
        wc.lpfnWndProc = OsdWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"WindhawkBrightnessOSD";
        RegisterClassEx(&wc);

        g_hOsdWnd = CreateWindowEx(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            L"WindhawkBrightnessOSD", L"",
            WS_POPUP, 0, 0, 80, 36, NULL, NULL, wc.hInstance, NULL);
        SetLayeredWindowAttributes(g_hOsdWnd, 0, 230, LWA_ALPHA);
    }
    g_osdText = text;

    HFONT hFont = CreateFont(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            
    HDC hdc = GetDC(g_hOsdWnd);
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SIZE size;
    GetTextExtentPoint32(hdc, text.c_str(), text.length(), &size);
    SelectObject(hdc, hOldFont);
    ReleaseDC(g_hOsdWnd, hdc);
    DeleteObject(hFont);
    
    int width = size.cx + 24; 

    POINT pt;
    GetCursorPos(&pt);
    SetWindowPos(g_hOsdWnd, HWND_TOPMOST, pt.x - width/2, pt.y - 60, width, 36, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hOsdWnd, NULL, TRUE);

    if (g_osdTimer) KillTimer(NULL, g_osdTimer);
    g_osdTimer = SetTimer(NULL, 1, g_osdDelay, OsdTimerProc);
}

LRESULT CALLBACK HiddenWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_USER_UPDATE_BRIGHTNESS || uMsg == WM_USER_UPDATE_BRIGHTNESS_SW) {
        int newCombined = (int)(short)LOWORD(wParam);
        bool isTarget = HIWORD(wParam) != 0;
        HMONITOR hMonitor = (HMONITOR)lParam;
        
        g_uiBrightnesses[hMonitor] = newCombined;
        
        auto& state = g_uiOverlays[hMonitor];
        int alpha = 0;
        
        if (uMsg == WM_USER_UPDATE_BRIGHTNESS) {
            if (newCombined < 0) alpha = (abs(newCombined) * 230) / 100;
        } else {
            if (newCombined < 100) alpha = ((100 - newCombined) * 230) / 200;
        }
        
        if (alpha == 0) {
            if (state.hOverlay) {
                DestroyWindow(state.hOverlay);
                state.hOverlay = NULL;
            }
        } else {
            if (!state.hOverlay) {
                WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
                wc.lpfnWndProc = DefWindowProc;
                wc.hInstance = GetModuleHandle(NULL);
                wc.lpszClassName = L"WindhawkDimOverlay";
                wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
                RegisterClassEx(&wc);

                MONITORINFO mi = { sizeof(MONITORINFO) };
                GetMonitorInfo(hMonitor, &mi);

                state.hOverlay = CreateWindowEx(
                    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                    L"WindhawkDimOverlay", L"",
                    WS_POPUP,
                    mi.rcMonitor.left, mi.rcMonitor.top, 
                    mi.rcMonitor.right - mi.rcMonitor.left, 
                    mi.rcMonitor.bottom - mi.rcMonitor.top,
                    NULL, NULL, wc.hInstance, NULL);
            }
            if (state.hOverlay) {
                SetWindowPos(state.hOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                SetLayeredWindowAttributes(state.hOverlay, 0, alpha, LWA_ALPHA);
            }
        }
        
        if (isTarget) {
            std::wstring text;
            if (g_syncMonitors && g_uiBrightnesses.size() > 1) {
                std::map<int, int> orderedBrightnesses;
                for (auto& pair : g_uiBrightnesses) {
                    MONITORINFOEX mi;
                    mi.cbSize = sizeof(MONITORINFOEX);
                    if (GetMonitorInfo(pair.first, &mi)) {
                        int num = 0;
                        swscanf_s(mi.szDevice, L"\\\\.\\DISPLAY%d", &num);
                        if (num > 0) orderedBrightnesses[num] = pair.second;
                    }
                }
                for (auto& pair : orderedBrightnesses) {
                    text += L"{" + std::to_wstring(pair.first) + L" %" + std::to_wstring(pair.second) + L"} ";
                }
                if (!text.empty()) text.pop_back();
            } else {
                text = L"%" + std::to_wstring(newCombined);
            }
            ShowOsd(text);
        }
        return 0;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

// Mouse Hooking
bool CheckModifierKeys() {
    bool shift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
    bool ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000;
    bool alt = GetAsyncKeyState(VK_MENU) & 0x8000;

    if (g_modifierKey == L"shift") return shift && !ctrl && !alt;
    if (g_modifierKey == L"ctrl") return !shift && ctrl && !alt;
    if (g_modifierKey == L"alt") return !shift && !ctrl && alt;
    if (g_modifierKey == L"shift_ctrl") return shift && ctrl && !alt;
    if (g_modifierKey == L"shift_alt") return shift && !ctrl && alt;
    if (g_modifierKey == L"ctrl_alt") return !shift && ctrl && alt;
    return true; 
}

bool IsPointInTaskbar(POINT pt) {
    if (g_allowInFullscreen) {
        HWND hTray = FindWindow(L"Shell_TrayWnd", NULL);
        if (hTray) {
            RECT rc;
            if (GetWindowRect(hTray, &rc) && PtInRect(&rc, pt)) return true;
        }
        HWND hSecTray = FindWindow(L"Shell_SecondaryTrayWnd", NULL);
        while (hSecTray) {
            RECT rc;
            if (GetWindowRect(hSecTray, &rc) && PtInRect(&rc, pt)) return true;
            hSecTray = FindWindowEx(NULL, hSecTray, L"Shell_SecondaryTrayWnd", NULL);
        }
        return false;
    } else {
        HWND hWnd = WindowFromPoint(pt);
        if (!hWnd) return false;
        
        HWND hRoot = GetAncestor(hWnd, GA_ROOT);
        WCHAR className[256] = {0};
        GetClassName(hRoot, className, 256);
        
        if (_wcsicmp(className, L"Shell_TrayWnd") == 0 || _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
            return true;
        }
        return false;
    }
}

HHOOK g_hMouseHook = NULL;

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;
        if (IsPointInTaskbar(pMouse->pt) && CheckModifierKeys()) {
            short zDelta = HIWORD(pMouse->mouseData);
            int delta = (zDelta > 0) ? g_brightnessChangeStep : -g_brightnessChangeStep;
            
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_pendingDelta += delta;
                g_lastPoint = pMouse->pt;
            }
            SetEvent(g_hWakeEvent);
            
            return 1; 
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}


DWORD g_dwHookThreadId = 0;
std::thread* g_pHookThread = nullptr;

void MouseHookThread() {
    g_dwHookThreadId = GetCurrentThreadId();
    g_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, GetModuleHandle(NULL), 0);
    
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"WindhawkBrightnessHidden";
    RegisterClassEx(&wc);
    g_hHiddenWnd = CreateWindowEx(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    if (g_hMouseHook) {
        UnhookWindowsHookEx(g_hMouseHook);
        g_hMouseHook = NULL;
    }
    
    if (g_hHiddenWnd) DestroyWindow(g_hHiddenWnd);
    if (g_hOsdWnd) DestroyWindow(g_hOsdWnd);
    for (auto& pair : g_uiOverlays) {
        if (pair.second.hOverlay) DestroyWindow(pair.second.hOverlay);
    }
}

// Initialization

BOOL Wh_ModInit() {
    LoadSettings();

    g_hWakeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_threadRunning = true;
    g_pWorkerThread = new std::thread(BrightnessWorkerThread);

    g_pHookThread = new std::thread(MouseHookThread);
    return TRUE;
}

void Wh_ModUninit() {
    if (g_dwHookThreadId) {
        PostThreadMessage(g_dwHookThreadId, WM_QUIT, 0, 0);
    }
    if (g_pHookThread) {
        if (g_pHookThread->joinable()) g_pHookThread->join();
        delete g_pHookThread;
        g_pHookThread = nullptr;
    }
    
    g_threadRunning = false;
    SetEvent(g_hWakeEvent);
    if (g_pWorkerThread) {
        if (g_pWorkerThread->joinable()) g_pWorkerThread->join();
        delete g_pWorkerThread;
        g_pWorkerThread = nullptr;
    }
    if (g_hWakeEvent) {
        CloseHandle(g_hWakeEvent);
    }
}
