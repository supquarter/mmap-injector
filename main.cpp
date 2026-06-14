#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
#include <ShlObj.h>

#include "ManualMap.hpp"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static constexpr int IDC_SCRIPT_EDIT = 100;
static constexpr int IDC_INJECT_BTN = 101;
static constexpr int IDC_EXECUTE_BTN = 102;
static constexpr int IDC_STATUS = 103;
static constexpr int IDC_CLEAR_BTN = 104;

static HINSTANCE g_Inst = nullptr;
static HWND g_Edit = nullptr;
static HWND g_Status = nullptr;
static HWND g_InjectBtn = nullptr;
static HWND g_ExecuteBtn = nullptr;
static std::atomic<bool> g_Injected{ false };
static DWORD g_TargetPid = 0;

static void SetStatus(HWND hwnd, const char* text, bool isError = false)
{
    if (!g_Status) return;
    SetWindowTextA(g_Status, text);

    if (isError)
    {
        SendMessageW(g_Status, WM_SETFONT,
            reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), 0);
        InvalidateRect(g_Status, nullptr, TRUE);
    }
}

static DWORD WINAPI InjectThread(LPVOID lpParam)
{
    auto* hwnd = static_cast<HWND>(lpParam);

    SetStatus(hwnd, "Finding RobloxPlayerBeta.exe...", false);

    DWORD pid = ManualMap::FindProcess("RobloxPlayerBeta.exe");
    if (!pid)
    {
        SetStatus(hwnd, "Error: RobloxPlayerBeta.exe not found", true);
        return 1;
    }

    g_TargetPid = pid;

    char buf[128];
    snprintf(buf, sizeof(buf), "Found PID %lu. Injecting...", pid);
    SetStatus(hwnd, buf, false);

    char dllPath[MAX_PATH];
    GetModuleFileNameA(nullptr, dllPath, MAX_PATH);

    std::string path(dllPath);
    auto pos = path.find_last_of('\\');
    if (pos != std::string::npos)
        path = path.substr(0, pos + 1) + "supq.dll";
    else
        path = "supq.dll";

    if (!ManualMap::Inject(pid, path))
    {
        SetStatus(hwnd, "Injection failed! Is Roblox running?", true);
        return 1;
    }

    g_Injected.store(true);

    snprintf(buf, sizeof(buf), "Injected successfully (PID: %lu)", pid);
    SetStatus(hwnd, buf, false);

    EnableWindow(g_InjectBtn, FALSE);
    EnableWindow(g_ExecuteBtn, TRUE);
    SetWindowTextA(g_InjectBtn, "Injected");

    return 0;
}

static void DoInject(HWND hwnd)
{
    SetStatus(hwnd, "Starting injection...", false);
    EnableWindow(g_InjectBtn, FALSE);
    SetWindowTextA(g_InjectBtn, "Injecting...");

    HANDLE hThread = CreateThread(nullptr, 0, InjectThread, hwnd, 0, nullptr);
    if (hThread)
        CloseHandle(hThread);
    else
    {
        SetStatus(hwnd, "Failed to create injection thread", true);
        EnableWindow(g_InjectBtn, TRUE);
        SetWindowTextA(g_InjectBtn, "Inject");
    }
}

static void DoExecute(HWND hwnd)
{
    if (!g_Injected.load() || !g_TargetPid)
    {
        SetStatus(hwnd, "Not injected yet. Click Inject first.", true);
        return;
    }

    int len = GetWindowTextLengthA(g_Edit);
    if (len <= 0)
    {
        SetStatus(hwnd, "Script editor is empty", true);
        return;
    }

    std::string script(len, '\0');
    GetWindowTextA(g_Edit, script.data(), len + 1);

    if (!ManualMap::SendScript(g_TargetPid, script))
    {
        SetStatus(hwnd, "Execute failed - pipe not available", true);
        return;
    }

    SetStatus(hwnd, "Script sent for execution", false);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HDC hdc = GetDC(hwnd);
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(hwnd, hdc);

        int baseFontSize = -MulDiv(11, dpi, 72);

        HFONT hFont = CreateFontA(baseFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        HFONT hMonoFont = CreateFontA(-MulDiv(13, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), 0);

        g_InjectBtn = CreateWindowW(
            L"BUTTON", L"Inject",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 12, 100, 32,
            hwnd, reinterpret_cast<HMENU>(IDC_INJECT_BTN), g_Inst, nullptr);
        SendMessageW(g_InjectBtn, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), 0);

        g_ExecuteBtn = CreateWindowW(
            L"BUTTON", L"Execute",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            120, 12, 100, 32,
            hwnd, reinterpret_cast<HMENU>(IDC_EXECUTE_BTN), g_Inst, nullptr);
        SendMessageW(g_ExecuteBtn, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), 0);
        EnableWindow(g_ExecuteBtn, FALSE);

        HWND hClearBtn = CreateWindowW(
            L"BUTTON", L"Clear",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            228, 12, 80, 32,
            hwnd, reinterpret_cast<HMENU>(IDC_CLEAR_BTN), g_Inst, nullptr);
        SendMessageW(hClearBtn, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), 0);

        g_Edit = CreateWindowW(
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | ES_NOHIDESEL,
            12, 54, 540, 340,
            hwnd, reinterpret_cast<HMENU>(IDC_SCRIPT_EDIT), g_Inst, nullptr);
        SendMessageW(g_Edit, WM_SETFONT, reinterpret_cast<WPARAM>(hMonoFont), 0);
        SendMessageW(g_Edit, EM_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(4));

        SetWindowTextA(g_Edit,
            "-- Script editor\n"
            "-- Type your Luau script here\n"
            "print(\"Hello from YuB-X!\")\n");

        g_Status = CreateWindowW(
            L"STATIC", L"Ready - Click Inject to begin",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            12, 404, 540, 20,
            hwnd, reinterpret_cast<HMENU>(IDC_STATUS), g_Inst, nullptr);
        SendMessageW(g_Status, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), 0);

        break;
    }

    case WM_CTLCOLORSTATIC:
    {
        auto* hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, RGB(30, 30, 30));
        SetTextColor(hdc, RGB(200, 200, 200));
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }

    case WM_CTLCOLOREDIT:
    {
        auto* hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, RGB(25, 25, 25));
        SetTextColor(hdc, RGB(220, 220, 220));
        auto* brush = CreateSolidBrush(RGB(25, 25, 25));
        return reinterpret_cast<LRESULT>(brush);
    }

    case WM_CTLCOLORBTN:
    {
        auto* hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, RGB(45, 45, 45));
        SetTextColor(hdc, RGB(220, 220, 220));
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }

    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case IDC_INJECT_BTN:
            DoInject(hwnd);
            break;

        case IDC_EXECUTE_BTN:
            DoExecute(hwnd);
            break;

        case IDC_CLEAR_BTN:
            SetWindowTextA(g_Edit, "");
            break;
        }
        break;
    }

    case WM_ERASEBKGND:
    {
        auto* dc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        auto brush = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        return 1;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    g_Inst = hInstance;

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = L"YuBX_Injector";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
        return 1;

    RECT wr = { 0, 0, 564, 436 };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, FALSE);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - winW) / 2;
    int posY = (screenH - winH) / 2;

    HWND hwnd = CreateWindowExW(
        0, L"YuBX_Injector", L"YuB-X Injector",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
        return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
