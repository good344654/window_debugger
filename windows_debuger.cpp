#include <windows.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>
#include <psapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "psapi.lib")

// 拖尾点结构
struct TrailPoint {
    int x, y;
    int alpha;
    DWORD time;
};

// 全局变量
std::vector<TrailPoint> trailPoints;
const int MAX_TRAIL_LENGTH = 200;
const int TRAIL_FADE_TIME = 300; // 毫秒
const int TRAIL_SIZE = 8;
const float PREDICTION_SCALE = 7.5f;
const int PREDICTION_MAX_DISTANCE = 220;
const int PREDICTION_MIN_DISTANCE = 32;
const float VELOCITY_SMOOTHING = 0.18f;
const float PREDICTION_SMOOTHING = 0.22f;
const float TURN_SMOOTHING = 0.20f;
const float TURN_PREDICTION_FACTOR = 0.90f;
HWND hwnd;
POINT lastMousePos = {-1, -1};
POINT prevMousePos = {-1, -1};
POINT predictedMousePos = {-1, -1};
float smoothVelocityX = 0.0f;
float smoothVelocityY = 0.0f;
float smoothPredictX = -1.0f;
float smoothPredictY = -1.0f;
float lastMoveAngle = 0.0f;
bool hasLastMoveAngle = false;
float smoothTurnAngle = 0.0f;

HWND hoveredWindow = NULL;
RECT hoveredWindowRect = {0, 0, 0, 0};
char hoveredWindowTitle[256] = "";
char hoveredProcessName[MAX_PATH] = "unknown";
DWORD hoveredProcessId = 0;
SIZE_T hoveredMemoryKB = 0;
double hoveredCpuUsage = 0.0;
bool hoveredWindowFocused = false;

HWND lockedWindow = NULL;
RECT lockedWindowRect = {0, 0, 0, 0};
char lockedWindowTitle[256] = "";
char lockedProcessName[MAX_PATH] = "unknown";
DWORD lockedProcessId = 0;
SIZE_T lockedMemoryKB = 0;
double lockedCpuUsage = 0.0;
bool lockedWindowFocused = false;

struct FrozenWindowInfo {
    HWND window = NULL;
    RECT rect = {0, 0, 0, 0};
    char title[256] = "";
    char processName[MAX_PATH] = "unknown";
    DWORD processId = 0;
    SIZE_T memoryKB = 0;
    double cpuUsage = 0.0;
    bool focused = false;
};

std::vector<FrozenWindowInfo> frozenWindows;

const int HOTKEY_ID_LOCK_WINDOW = 1001;
const int HOTKEY_ID_TOGGLE_FREEZE = 1002;
const int HOTKEY_ID_TOPMOST_WINDOW = 1003;
const int HOTKEY_ID_TOGGLE_INFO = 1004;
bool showInfoPanel = true;

struct ProcessCpuSample {
    DWORD processId = 0;
    ULONGLONG lastProcessTime = 0;
    ULONGLONG lastSystemTime = 0;
    double cpuUsage = 0.0;
    bool initialized = false;
};

ProcessCpuSample cpuSample;

ULONGLONG FileTimeToUInt64(const FILETIME& ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

void ResetHoveredWindowInfo() {
    hoveredWindow = NULL;
    hoveredWindowFocused = false;
    SetRectEmpty(&hoveredWindowRect);
    hoveredWindowTitle[0] = '\0';
    lstrcpyA(hoveredProcessName, "unknown");
    hoveredProcessId = 0;
    hoveredMemoryKB = 0;
    hoveredCpuUsage = 0.0;
}

void CopyWindowInfo(HWND sourceWindow, const RECT& sourceRect, const char* sourceTitle,
                    const char* sourceProcessName, DWORD sourceProcessId, SIZE_T sourceMemoryKB,
                    double sourceCpuUsage, bool sourceFocused,
                    HWND& targetWindow, RECT& targetRect, char* targetTitle,
                    char* targetProcessName, DWORD& targetProcessId, SIZE_T& targetMemoryKB,
                    double& targetCpuUsage, bool& targetFocused) {
    targetWindow = sourceWindow;
    targetRect = sourceRect;
    lstrcpynA(targetTitle, sourceTitle, 256);
    lstrcpynA(targetProcessName, sourceProcessName, MAX_PATH);
    targetProcessId = sourceProcessId;
    targetMemoryKB = sourceMemoryKB;
    targetCpuUsage = sourceCpuUsage;
    targetFocused = sourceFocused;
}

void ClearLockedWindowInfo() {
    lockedWindow = NULL;
    lockedWindowFocused = false;
    SetRectEmpty(&lockedWindowRect);
    lockedWindowTitle[0] = '\0';
    lstrcpyA(lockedProcessName, "unknown");
    lockedProcessId = 0;
    lockedMemoryKB = 0;
    lockedCpuUsage = 0.0;
}

int FindFrozenWindowIndex(HWND targetWindow) {
    for (size_t i = 0; i < frozenWindows.size(); ++i) {
        if (frozenWindows[i].window == targetWindow) {
            return (int)i;
        }
    }
    return -1;
}

bool IsFrozenWindow(HWND targetWindow) {
    return FindFrozenWindowIndex(targetWindow) >= 0;
}

void RemoveFrozenWindowAt(int index) {
    if (index >= 0 && index < (int)frozenWindows.size()) {
        frozenWindows.erase(frozenWindows.begin() + index);
    }
}

bool UpdateProcessCpuUsage(DWORD processId, double& cpuUsageOut) {
    cpuUsageOut = 0.0;
    if (processId == 0) {
        cpuSample = ProcessCpuSample{};
        return false;
    }

    HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!processHandle) {
        cpuSample = ProcessCpuSample{};
        return false;
    }

    FILETIME createTime, exitTime, kernelTime, userTime;
    FILETIME idleTime, systemKernelTime, systemUserTime;
    bool ok = GetProcessTimes(processHandle, &createTime, &exitTime, &kernelTime, &userTime) &&
              GetSystemTimes(&idleTime, &systemKernelTime, &systemUserTime);

    if (ok) {
        ULONGLONG processTime = FileTimeToUInt64(kernelTime) + FileTimeToUInt64(userTime);
        ULONGLONG systemTime = FileTimeToUInt64(systemKernelTime) + FileTimeToUInt64(systemUserTime);

        if (cpuSample.initialized && cpuSample.processId == processId && systemTime > cpuSample.lastSystemTime) {
            ULONGLONG processDelta = processTime - cpuSample.lastProcessTime;
            ULONGLONG systemDelta = systemTime - cpuSample.lastSystemTime;
            SYSTEM_INFO sysInfo;
            GetSystemInfo(&sysInfo);
            double normalizedCpu = 0.0;
            if (systemDelta > 0 && sysInfo.dwNumberOfProcessors > 0) {
                normalizedCpu = (double)processDelta / (double)systemDelta;
                normalizedCpu *= 100.0 * sysInfo.dwNumberOfProcessors;
            }
            if (normalizedCpu < 0.0) normalizedCpu = 0.0;
            cpuSample.cpuUsage = normalizedCpu;
        }

        cpuSample.processId = processId;
        cpuSample.lastProcessTime = processTime;
        cpuSample.lastSystemTime = systemTime;
        cpuSample.initialized = true;
        cpuUsageOut = cpuSample.cpuUsage;
    }

    CloseHandle(processHandle);
    return ok;
}

bool QueryWindowInfo(HWND targetWindow, RECT& targetRect, char* targetTitle, char* targetProcessName,
                     DWORD& targetProcessId, SIZE_T& targetMemoryKB, double& targetCpuUsage,
                     bool& targetFocused) {
    if (!targetWindow || targetWindow == hwnd || !IsWindow(targetWindow) || !IsWindowVisible(targetWindow)) {
        return false;
    }

    if (!GetWindowRect(targetWindow, &targetRect)) {
        return false;
    }

    GetWindowTextA(targetWindow, targetTitle, 256);
    if (targetTitle[0] == '\0') {
        lstrcpyA(targetTitle, "Untitled Window");
    }

    HWND foregroundWindow = GetForegroundWindow();
    if (foregroundWindow) {
        foregroundWindow = GetAncestor(foregroundWindow, GA_ROOT);
    }
    targetFocused = (foregroundWindow == targetWindow);

    DWORD processId = 0;
    GetWindowThreadProcessId(targetWindow, &processId);
    targetProcessId = processId;
    targetMemoryKB = 0;
    targetCpuUsage = 0.0;
    lstrcpyA(targetProcessName, "unknown");

    HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (processHandle) {
        HMODULE moduleHandle = NULL;
        DWORD needed = 0;
        if (EnumProcessModules(processHandle, &moduleHandle, sizeof(moduleHandle), &needed)) {
            GetModuleBaseNameA(processHandle, moduleHandle, targetProcessName, MAX_PATH);
        }

        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(processHandle, &pmc, sizeof(pmc))) {
            targetMemoryKB = pmc.WorkingSetSize / 1024;
        }
        CloseHandle(processHandle);
    }

    UpdateProcessCpuUsage(processId, targetCpuUsage);
    return true;
}

void UpdateHoveredWindowInfo(const POINT& cursorPos) {
    HWND targetWindow = WindowFromPoint(cursorPos);
    if (targetWindow) {
        targetWindow = GetAncestor(targetWindow, GA_ROOT);
    }

    if (!QueryWindowInfo(targetWindow, hoveredWindowRect, hoveredWindowTitle, hoveredProcessName,
                         hoveredProcessId, hoveredMemoryKB, hoveredCpuUsage, hoveredWindowFocused)) {
        ResetHoveredWindowInfo();
        return;
    }

    hoveredWindow = targetWindow;
}

bool SetProcessFrozenState(DWORD processId, bool freeze) {
    if (processId == 0) {
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    THREADENTRY32 entry = {0};
    entry.dwSize = sizeof(entry);
    bool anyThreadProcessed = false;

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != processId) {
                continue;
            }

            HANDLE threadHandle = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (!threadHandle) {
                continue;
            }

            if (freeze) {
                if (SuspendThread(threadHandle) != (DWORD)-1) {
                    anyThreadProcessed = true;
                }
            } else {
                DWORD previousCount;
                do {
                    previousCount = ResumeThread(threadHandle);
                    if (previousCount == (DWORD)-1) {
                        break;
                    }
                    anyThreadProcessed = true;
                } while (previousCount > 1);
            }

            CloseHandle(threadHandle);
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return anyThreadProcessed;
}

void ToggleLockState() {
    if (lockedWindow) {
        ClearLockedWindowInfo();
        return;
    }

    if (hoveredWindow && hoveredProcessId != 0) {
        CopyWindowInfo(hoveredWindow, hoveredWindowRect, hoveredWindowTitle, hoveredProcessName,
                       hoveredProcessId, hoveredMemoryKB, hoveredCpuUsage, hoveredWindowFocused,
                       lockedWindow, lockedWindowRect, lockedWindowTitle, lockedProcessName,
                       lockedProcessId, lockedMemoryKB, lockedCpuUsage, lockedWindowFocused);
    }
}

void ToggleFreezeState() {
    if (!hoveredWindow || hoveredProcessId == 0) {
        return;
    }

    int frozenIndex = FindFrozenWindowIndex(hoveredWindow);
    if (frozenIndex >= 0) {
        if (SetProcessFrozenState(frozenWindows[frozenIndex].processId, false)) {
            RemoveFrozenWindowAt(frozenIndex);
        }
        return;
    }

    if (SetProcessFrozenState(hoveredProcessId, true)) {
        FrozenWindowInfo frozenInfo;
        frozenInfo.window = hoveredWindow;
        frozenInfo.rect = hoveredWindowRect;
        lstrcpynA(frozenInfo.title, hoveredWindowTitle, 256);
        lstrcpynA(frozenInfo.processName, hoveredProcessName, MAX_PATH);
        frozenInfo.processId = hoveredProcessId;
        frozenInfo.memoryKB = hoveredMemoryKB;
        frozenInfo.cpuUsage = hoveredCpuUsage;
        frozenInfo.focused = hoveredWindowFocused;
        frozenWindows.push_back(frozenInfo);
    }
}

void ToggleTopmostState() {
    if (!hoveredWindow || !IsWindow(hoveredWindow)) {
        return;
    }

    LONG_PTR style = GetWindowLongPtr(hoveredWindow, GWL_EXSTYLE);
    bool isTopmost = (style & WS_EX_TOPMOST) != 0;
    SetWindowPos(hoveredWindow,
                 isTopmost ? HWND_NOTOPMOST : HWND_TOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RefreshFrozenWindows() {
    for (int i = (int)frozenWindows.size() - 1; i >= 0; --i) {
        FrozenWindowInfo& frozenInfo = frozenWindows[i];
        if (!QueryWindowInfo(frozenInfo.window, frozenInfo.rect, frozenInfo.title, frozenInfo.processName,
                             frozenInfo.processId, frozenInfo.memoryKB, frozenInfo.cpuUsage, frozenInfo.focused)) {
            RemoveFrozenWindowAt(i);
        }
    }
}


LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // 创建双缓冲
            RECT rect;
            GetClientRect(hwnd, &rect);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
            SelectObject(memDC, memBitmap);
            
            // 清空背景（透明）
            HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(memDC, &rect, hBrush);
            DeleteObject(hBrush);
            
            // 绘制拖尾
            DWORD currentTime = GetTickCount();
            for (size_t i = 0; i < trailPoints.size(); i++) {
                TrailPoint& point = trailPoints[i];
                
                // 计算透明度
                DWORD elapsed = currentTime - point.time;
                if (elapsed > TRAIL_FADE_TIME) continue;
                
                float fadeRatio = 1.0f - (float)elapsed / TRAIL_FADE_TIME;
                
                // 计算大小（越老越小）
                int size = (int)(TRAIL_SIZE * fadeRatio);
                if (size < 2) size = 2;
                
                // 创建炫酷渐变色（从青色到紫色到橙色）
                int r, g, b;
                if (fadeRatio > 0.5f) {
                    // 前半段：青色 -> 紫色
                    float t = (fadeRatio - 0.5f) * 2.0f;
                    r = (int)(128 * (1.0f - t) + 138 * t);
                    g = (int)(255 * (1.0f - t) + 43 * t);
                    b = (int)(255 * (1.0f - t) + 226 * t);
                } else {
                    // 后半段：紫色 -> 橙色
                    float t = fadeRatio * 2.0f;
                    r = (int)(138 * (1.0f - t) + 255 * t);
                    g = (int)(43 * (1.0f - t) + 140 * t);
                    b = (int)(226 * (1.0f - t) + 0 * t);
                }
                
                // 绘制圆形拖尾点
                HBRUSH colorBrush = CreateSolidBrush(RGB(r, g, b));
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(r, g, b));
                SelectObject(memDC, colorBrush);
                SelectObject(memDC, pen);
                
                Ellipse(memDC,
                    point.x - size, point.y - size,
                    point.x + size, point.y + size);
                
                DeleteObject(colorBrush);
                DeleteObject(pen);
            }

            // 绘制红色路径预测线（平滑霓虹感）
            if (lastMousePos.x >= 0 && predictedMousePos.x >= 0) {
                HPEN outerPredPen = CreatePen(PS_SOLID, 6, RGB(90, 0, 0));
                HPEN innerPredPen = CreatePen(PS_SOLID, 2, RGB(255, 50, 50));

                SelectObject(memDC, outerPredPen);
                MoveToEx(memDC, lastMousePos.x, lastMousePos.y, NULL);
                LineTo(memDC, predictedMousePos.x, predictedMousePos.y);

                SelectObject(memDC, innerPredPen);
                MoveToEx(memDC, lastMousePos.x, lastMousePos.y, NULL);
                LineTo(memDC, predictedMousePos.x, predictedMousePos.y);

                const int tipSize = 12;
                int dx = predictedMousePos.x - lastMousePos.x;
                int dy = predictedMousePos.y - lastMousePos.y;
                float length = sqrt((float)(dx * dx + dy * dy));
                if (length > 14.0f) {
                    float ux = dx / length;
                    float uy = dy / length;
                    float px = -uy;
                    float py = ux;

                    POINT arrow[3];
                    arrow[0] = predictedMousePos;
                    arrow[1].x = (LONG)(predictedMousePos.x - ux * tipSize + px * (tipSize * 0.55f));
                    arrow[1].y = (LONG)(predictedMousePos.y - uy * tipSize + py * (tipSize * 0.55f));
                    arrow[2].x = (LONG)(predictedMousePos.x - ux * tipSize - px * (tipSize * 0.55f));
                    arrow[2].y = (LONG)(predictedMousePos.y - uy * tipSize - py * (tipSize * 0.55f));

                    HGDIOBJ oldBrush = SelectObject(memDC, CreateSolidBrush(RGB(255, 50, 50)));
                    Polygon(memDC, arrow, 3);
                    DeleteObject(SelectObject(memDC, oldBrush));
                }

                DeleteObject(outerPredPen);
                DeleteObject(innerPredPen);
            }

            // 绘制科技感准星框，锁定后切换为黄色
            if (lastMousePos.x >= 0) {
                const int boxSize = 26;
                const int corner = 9;
                const int cross = 5;
                int left = lastMousePos.x - boxSize;
                int right = lastMousePos.x + boxSize;
                int top = lastMousePos.y - boxSize;
                int bottom = lastMousePos.y + boxSize;

                COLORREF cursorGlowColor = lockedWindow ? RGB(110, 90, 0) : RGB(0, 70, 0);
                COLORREF cursorFrameColor = lockedWindow ? RGB(255, 220, 0) : RGB(0, 255, 120);
                COLORREF cursorDetailColor = lockedWindow ? RGB(255, 245, 120) : RGB(120, 255, 180);

                HPEN glowPen = CreatePen(PS_SOLID, 3, cursorGlowColor);
                HPEN framePen = CreatePen(PS_SOLID, 2, cursorFrameColor);
                HPEN detailPen = CreatePen(PS_DOT, 1, cursorDetailColor);
                HGDIOBJ oldPen = SelectObject(memDC, glowPen);
                HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));

                Rectangle(memDC, left, top, right, bottom);

                SelectObject(memDC, framePen);
                MoveToEx(memDC, left, top + corner, NULL); LineTo(memDC, left, top);
                LineTo(memDC, left + corner, top);
                MoveToEx(memDC, right - corner, top, NULL); LineTo(memDC, right, top);
                LineTo(memDC, right, top + corner);
                MoveToEx(memDC, left, bottom - corner, NULL); LineTo(memDC, left, bottom);
                LineTo(memDC, left + corner, bottom);
                MoveToEx(memDC, right - corner, bottom, NULL); LineTo(memDC, right, bottom);
                LineTo(memDC, right, bottom - corner);

                MoveToEx(memDC, lastMousePos.x - cross, lastMousePos.y, NULL);
                LineTo(memDC, lastMousePos.x + cross, lastMousePos.y);
                MoveToEx(memDC, lastMousePos.x, lastMousePos.y - cross, NULL);
                LineTo(memDC, lastMousePos.x, lastMousePos.y + cross);

                SelectObject(memDC, detailPen);
                Arc(memDC,
                    lastMousePos.x - boxSize - 6, lastMousePos.y - boxSize - 6,
                    lastMousePos.x + boxSize + 6, lastMousePos.y + boxSize + 6,
                    lastMousePos.x - boxSize, lastMousePos.y,
                    lastMousePos.x + boxSize, lastMousePos.y);
                MoveToEx(memDC, left - 6, lastMousePos.y + 12, NULL);
                LineTo(memDC, right + 6, lastMousePos.y + 12);

                SelectObject(memDC, oldBrush);
                SelectObject(memDC, oldPen);
                DeleteObject(glowPen);
                DeleteObject(framePen);
                DeleteObject(detailPen);
            }

            // 绘制冻结窗口指示器（红色，可同时存在多个）
            for (size_t i = 0; i < frozenWindows.size(); ++i) {
                if (IsRectEmpty(&frozenWindows[i].rect)) {
                    continue;
                }

                RECT winRect = frozenWindows[i].rect;
                InflateRect(&winRect, 4, 4);

                HPEN winGlowPen = CreatePen(PS_SOLID, 4, RGB(90, 0, 0));
                HPEN winFramePen = CreatePen(PS_SOLID, 2, RGB(255, 60, 60));
                HPEN winDetailPen = CreatePen(PS_DOT, 1, RGB(255, 150, 150));
                HGDIOBJ oldPen = SelectObject(memDC, winGlowPen);
                HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));

                Rectangle(memDC, winRect.left, winRect.top, winRect.right, winRect.bottom);
                SelectObject(memDC, winFramePen);
                Rectangle(memDC, winRect.left + 2, winRect.top + 2, winRect.right - 2, winRect.bottom - 2);

                SelectObject(memDC, winDetailPen);
                MoveToEx(memDC, winRect.left - 10, winRect.top - 10, NULL);
                LineTo(memDC, winRect.left + 24, winRect.top - 10);
                MoveToEx(memDC, winRect.left - 10, winRect.top - 10, NULL);
                LineTo(memDC, winRect.left - 10, winRect.top + 24);
                MoveToEx(memDC, winRect.right + 10, winRect.bottom + 10, NULL);
                LineTo(memDC, winRect.right - 24, winRect.bottom + 10);
                MoveToEx(memDC, winRect.right + 10, winRect.bottom + 10, NULL);
                LineTo(memDC, winRect.right + 10, winRect.bottom - 24);

                SelectObject(memDC, oldBrush);
                SelectObject(memDC, oldPen);
                DeleteObject(winGlowPen);
                DeleteObject(winFramePen);
                DeleteObject(winDetailPen);
            }

            // 绘制当前窗口指示器
            HWND displayWindow = lockedWindow ? lockedWindow : hoveredWindow;
            RECT displayRect = lockedWindow ? lockedWindowRect : hoveredWindowRect;
            const char* displayTitle = lockedWindow ? lockedWindowTitle : hoveredWindowTitle;
            const char* displayProcessName = lockedWindow ? lockedProcessName : hoveredProcessName;
            DWORD displayProcessId = lockedWindow ? lockedProcessId : hoveredProcessId;
            SIZE_T displayMemoryKB = lockedWindow ? lockedMemoryKB : hoveredMemoryKB;
            double displayCpuUsage = lockedWindow ? lockedCpuUsage : hoveredCpuUsage;
            bool displayFocused = lockedWindow ? lockedWindowFocused : hoveredWindowFocused;
            bool displayLocked = (lockedWindow != NULL);
            bool displayFrozen = displayWindow && IsFrozenWindow(displayWindow);

            if (displayWindow && !IsRectEmpty(&displayRect)) {
                RECT winRect = displayRect;
                InflateRect(&winRect, 4, 4);

                COLORREF glowColor = displayFrozen ? RGB(90, 0, 0) : (displayLocked ? RGB(110, 90, 0) : (displayFocused ? RGB(0, 70, 20) : RGB(0, 40, 80)));
                COLORREF frameColor = displayFrozen ? RGB(255, 60, 60) : (displayLocked ? RGB(255, 220, 0) : (displayFocused ? RGB(0, 255, 120) : RGB(0, 180, 255)));
                COLORREF detailColor = displayFrozen ? RGB(255, 150, 150) : (displayLocked ? RGB(255, 245, 120) : (displayFocused ? RGB(120, 255, 180) : RGB(120, 240, 255)));

                HPEN winGlowPen = CreatePen(PS_SOLID, 4, glowColor);
                HPEN winFramePen = CreatePen(PS_SOLID, 2, frameColor);
                HPEN winDetailPen = CreatePen(PS_DOT, 1, detailColor);
                HGDIOBJ oldPen = SelectObject(memDC, winGlowPen);
                HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));

                Rectangle(memDC, winRect.left, winRect.top, winRect.right, winRect.bottom);
                SelectObject(memDC, winFramePen);
                Rectangle(memDC, winRect.left + 2, winRect.top + 2, winRect.right - 2, winRect.bottom - 2);

                SelectObject(memDC, winDetailPen);
                MoveToEx(memDC, winRect.left - 10, winRect.top - 10, NULL);
                LineTo(memDC, winRect.left + 24, winRect.top - 10);
                MoveToEx(memDC, winRect.left - 10, winRect.top - 10, NULL);
                LineTo(memDC, winRect.left - 10, winRect.top + 24);
                MoveToEx(memDC, winRect.right + 10, winRect.bottom + 10, NULL);
                LineTo(memDC, winRect.right - 24, winRect.bottom + 10);
                MoveToEx(memDC, winRect.right + 10, winRect.bottom + 10, NULL);
                LineTo(memDC, winRect.right + 10, winRect.bottom - 24);

                SelectObject(memDC, oldBrush);
                SelectObject(memDC, oldPen);
                DeleteObject(winGlowPen);
                DeleteObject(winFramePen);
                DeleteObject(winDetailPen);

                if (showInfoPanel) {
                    char infoLine1[512];
                    char infoLine2[512];
                    char infoLine3[512];
                    char infoLine4[256];
                    int width = displayRect.right - displayRect.left;
                    int height = displayRect.bottom - displayRect.top;
                    char memoryText[96];
                    double displayMemory = (double)displayMemoryKB;
                    const char* memoryUnit = "KB";
                    if (displayMemory >= 1024.0) {
                        displayMemory /= 1024.0;
                        memoryUnit = "MB";
                    }
                    if (displayMemory >= 1024.0) {
                        displayMemory /= 1024.0;
                        memoryUnit = "GB";
                    }
                    snprintf(memoryText, sizeof(memoryText), "%.2f %s (%lu KB)", displayMemory, memoryUnit, (unsigned long)displayMemoryKB);
                    snprintf(infoLine1, sizeof(infoLine1), "HWND: 0x%p  PID: %lu  CPU: %.2f%%", displayWindow, displayProcessId, displayCpuUsage);
                    snprintf(infoLine2, sizeof(infoLine2), "TASK: %s  MEM: %s", displayProcessName, memoryText);
                    snprintf(infoLine3, sizeof(infoLine3), "TITLE: %s", displayTitle);
                    snprintf(infoLine4, sizeof(infoLine4), "RES: %d x %d  STATE: %s", width, height,
                             displayFrozen ? "FROZEN" : (displayLocked ? "LOCKED" : "TRACKING"));

                    RECT panelRect;
                    panelRect.left = displayRect.left;
                    panelRect.top = displayRect.top - 96;
                    panelRect.right = panelRect.left + 520;
                    panelRect.bottom = panelRect.top + 86;
                    if (panelRect.top < 10) {
                        OffsetRect(&panelRect, 0, 106 - panelRect.top);
                    }

                    RECT screenRect;
                    GetClientRect(hwnd, &screenRect);
                    if (panelRect.right > screenRect.right - 10) {
                        OffsetRect(&panelRect, (screenRect.right - 10) - panelRect.right, 0);
                    }
                    if (panelRect.left < 10) {
                        OffsetRect(&panelRect, 10 - panelRect.left, 0);
                    }

                    HBRUSH panelBrush = CreateSolidBrush(RGB(5, 18, 26));
                    HPEN panelPen = CreatePen(PS_SOLID, 1, frameColor);
                    SetBkMode(memDC, TRANSPARENT);
                    SetTextColor(memDC, displayFrozen ? RGB(255, 170, 170) : (displayLocked ? RGB(255, 240, 140) : (displayFocused ? RGB(120, 255, 180) : RGB(120, 255, 220))));
                    oldPen = SelectObject(memDC, panelPen);
                    oldBrush = SelectObject(memDC, panelBrush);
                    RoundRect(memDC, panelRect.left, panelRect.top, panelRect.right, panelRect.bottom, 8, 8);

                    RECT textRect1 = {panelRect.left + 10, panelRect.top + 8, panelRect.right - 10, panelRect.top + 24};
                    RECT textRect2 = {panelRect.left + 10, panelRect.top + 28, panelRect.right - 10, panelRect.top + 44};
                    RECT textRect3 = {panelRect.left + 10, panelRect.top + 48, panelRect.right - 10, panelRect.top + 78};
                    RECT textRect4 = {panelRect.left + 10, panelRect.top + 64, panelRect.right - 10, panelRect.top + 80};

                    DrawTextA(memDC, infoLine1, -1, &textRect1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    DrawTextA(memDC, infoLine2, -1, &textRect2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    DrawTextA(memDC, infoLine3, -1, &textRect3, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    DrawTextA(memDC, infoLine4, -1, &textRect4, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

                    SelectObject(memDC, oldBrush);
                    SelectObject(memDC, oldPen);
                    DeleteObject(panelBrush);
                    DeleteObject(panelPen);
                }
            }
            
            // 复制到屏幕
            BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);
            
            DeleteObject(memBitmap);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_HOTKEY:
            if (wParam == HOTKEY_ID_LOCK_WINDOW) {
                ToggleLockState();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == HOTKEY_ID_TOGGLE_FREEZE) {
                ToggleFreezeState();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == HOTKEY_ID_TOPMOST_WINDOW) {
                ToggleTopmostState();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == HOTKEY_ID_TOGGLE_INFO) {
                showInfoPanel = !showInfoPanel;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            break;

        case WM_DESTROY:
            for (int i = (int)frozenWindows.size() - 1; i >= 0; --i) {
                SetProcessFrozenState(frozenWindows[i].processId, false);
            }
            while (ShowCursor(TRUE) < 0) {
            }
            UnregisterHotKey(hwnd, HOTKEY_ID_LOCK_WINDOW);
            UnregisterHotKey(hwnd, HOTKEY_ID_TOGGLE_FREEZE);
            UnregisterHotKey(hwnd, HOTKEY_ID_TOPMOST_WINDOW);
            UnregisterHotKey(hwnd, HOTKEY_ID_TOGGLE_INFO);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 更新拖尾点
void UpdateTrail() {
    POINT cursorPos;
    GetCursorPos(&cursorPos);

    UpdateHoveredWindowInfo(cursorPos);
    RefreshFrozenWindows();

    if (lockedWindow) {
        if (!QueryWindowInfo(lockedWindow, lockedWindowRect, lockedWindowTitle, lockedProcessName,
                             lockedProcessId, lockedMemoryKB, lockedCpuUsage, lockedWindowFocused)) {
            ClearLockedWindowInfo();
        }
    }

    // 只有当鼠标移动时才添加新点
    if (lastMousePos.x != -1 && lastMousePos.y != -1) {
        // 计算鼠标移动的距离
        int dx = cursorPos.x - lastMousePos.x;
        int dy = cursorPos.y - lastMousePos.y;
        float distance = sqrt((float)(dx * dx + dy * dy));
        
        // 如果鼠标移动了,在路径上插入多个点
        if (distance > 0) {
            int numPoints = (int)distance;
            if (numPoints > 100) numPoints = 100; // 限制单次最多添加的点数
            
            for (int i = 0; i <= numPoints; i++) {
                float t = (float)i / (float)numPoints;
                TrailPoint newPoint;
                newPoint.x = (int)(lastMousePos.x + dx * t);
                newPoint.y = (int)(lastMousePos.y + dy * t);
                newPoint.time = GetTickCount();
                trailPoints.push_back(newPoint);
            }
        }
    }
    
    // 使用平滑速度 + 转向预测，让箭头同步鼠标旋转趋势
    if (lastMousePos.x != -1) {
        float instantVelocityX = (float)(cursorPos.x - lastMousePos.x);
        float instantVelocityY = (float)(cursorPos.y - lastMousePos.y);

        smoothVelocityX += (instantVelocityX - smoothVelocityX) * VELOCITY_SMOOTHING;
        smoothVelocityY += (instantVelocityY - smoothVelocityY) * VELOCITY_SMOOTHING;

        float speed = sqrt(smoothVelocityX * smoothVelocityX + smoothVelocityY * smoothVelocityY);
        float predictDirX = smoothVelocityX;
        float predictDirY = smoothVelocityY;

        if (speed > 0.01f) {
            float currentAngle = atan2(smoothVelocityY, smoothVelocityX);

            if (hasLastMoveAngle) {
                float deltaAngle = currentAngle - lastMoveAngle;
                while (deltaAngle > 3.1415926f) deltaAngle -= 6.2831852f;
                while (deltaAngle < -3.1415926f) deltaAngle += 6.2831852f;
                smoothTurnAngle += (deltaAngle - smoothTurnAngle) * TURN_SMOOTHING;
            } else {
                hasLastMoveAngle = true;
                smoothTurnAngle = 0.0f;
            }

            lastMoveAngle = currentAngle;

            float predictedAngle = currentAngle + smoothTurnAngle * TURN_PREDICTION_FACTOR;
            predictDirX = cos(predictedAngle) * speed;
            predictDirY = sin(predictedAngle) * speed;
        }

        float targetPredictX = cursorPos.x + predictDirX * PREDICTION_SCALE;
        float targetPredictY = cursorPos.y + predictDirY * PREDICTION_SCALE;

        float predDx = targetPredictX - cursorPos.x;
        float predDy = targetPredictY - cursorPos.y;
        float predDistance = sqrt(predDx * predDx + predDy * predDy);
        if (predDistance > 0.01f) {
            if (predDistance < (float)PREDICTION_MIN_DISTANCE) {
                float boostRatio = (float)PREDICTION_MIN_DISTANCE / predDistance;
                targetPredictX = cursorPos.x + predDx * boostRatio;
                targetPredictY = cursorPos.y + predDy * boostRatio;
                predDx = targetPredictX - cursorPos.x;
                predDy = targetPredictY - cursorPos.y;
                predDistance = (float)PREDICTION_MIN_DISTANCE;
            }

            if (predDistance > (float)PREDICTION_MAX_DISTANCE) {
                float clampRatio = (float)PREDICTION_MAX_DISTANCE / predDistance;
                targetPredictX = cursorPos.x + predDx * clampRatio;
                targetPredictY = cursorPos.y + predDy * clampRatio;
            }
        }

        if (smoothPredictX < 0.0f || smoothPredictY < 0.0f) {
            smoothPredictX = targetPredictX;
            smoothPredictY = targetPredictY;
        } else {
            smoothPredictX += (targetPredictX - smoothPredictX) * PREDICTION_SMOOTHING;
            smoothPredictY += (targetPredictY - smoothPredictY) * PREDICTION_SMOOTHING;
        }

        predictedMousePos.x = (LONG)smoothPredictX;
        predictedMousePos.y = (LONG)smoothPredictY;
    } else {
        smoothVelocityX = 0.0f;
        smoothVelocityY = 0.0f;
        smoothPredictX = (float)cursorPos.x;
        smoothPredictY = (float)cursorPos.y;
        hasLastMoveAngle = false;
        smoothTurnAngle = 0.0f;
        predictedMousePos = cursorPos;
    }

    prevMousePos = lastMousePos;
    lastMousePos = cursorPos;
    
    // 移除过期的点
    DWORD currentTime = GetTickCount();
    trailPoints.erase(
        std::remove_if(trailPoints.begin(), trailPoints.end(),
            [currentTime](const TrailPoint& p) {
                return (currentTime - p.time) > TRAIL_FADE_TIME;
            }),
        trailPoints.end()
    );
    
    // 限制最大长度
    while (trailPoints.size() > MAX_TRAIL_LENGTH) {
        trailPoints.erase(trailPoints.begin());
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPSTR lpCmdLine, int nCmdShow) {
    
    // 注册窗口类
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "MouseTrailClass";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    
    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, "窗口注册失败！", "错误", MB_ICONERROR);
        return 0;
    }
    
    // 获取屏幕尺寸
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    // 创建全屏透明窗口
    hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "MouseTrailClass",
        "鼠标拖尾",
        WS_POPUP,
        0, 0, screenWidth, screenHeight,
        NULL, NULL, hInstance, NULL
    );
    
    if (!hwnd) {
        MessageBox(NULL, "窗口创建失败！", "错误", MB_ICONERROR);
        return 0;
    }
    
    // 设置窗口透明度和透明色键
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (ShowCursor(FALSE) >= 0) {
    }

    RegisterHotKey(hwnd, HOTKEY_ID_LOCK_WINDOW, 0, VK_F1);
    RegisterHotKey(hwnd, HOTKEY_ID_TOGGLE_FREEZE, 0, VK_F2);
    RegisterHotKey(hwnd, HOTKEY_ID_TOPMOST_WINDOW, 0, VK_F3);
    RegisterHotKey(hwnd, HOTKEY_ID_TOGGLE_INFO, 0, VK_F4);
    
    // 设置定时器（每16ms更新一次，约60fps）
    SetTimer(hwnd, 1, 16, NULL);
    
    // 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER) {
            UpdateTrail();
            InvalidateRect(hwnd, NULL, FALSE);
            // 强制窗口保持在最顶层
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    return (int)msg.wParam;
}
