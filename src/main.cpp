/**
 * AlwaysOnTop – Windows system-tray utility
 *
 * Pins / unpins any window as always-on-top via a configurable global hotkey.
 * All state is stored in AlwaysOnTop.ini next to the executable.
 *
 * Build (cross-compile from Linux):
 *   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake .
 *   cmake --build build
 *
 * Dependencies: user32, shell32, advapi32  (all built-in on Windows)
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>

#include <algorithm>
#include <vector>

#include "resource.h"

/* ═══════════════════════════════════════════════════════════════════════════
   Constants
   ══════════════════════════════════════════════════════════════════════════*/

static const WCHAR APP_NAME[]   = L"AlwaysOnTop";
static const WCHAR WND_CLASS[]  = L"AlwaysOnTopHiddenWnd";
static const WCHAR MUTEX_NAME[] = L"AlwaysOnTop_SingleInstance_v1";
static const WCHAR REG_RUN[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR INI_SECT[]   = L"Settings";
static const WCHAR INI_MOD[]    = L"HotkeyModifiers";
static const WCHAR INI_VK[]     = L"HotkeyVK";
static const WCHAR INI_AUTO[]   = L"StartWithWindows";

/** Custom message sent by Shell_NotifyIcon to our hidden window. */
static const UINT WM_TRAYICON  = WM_APP + 1;
/** Posted from the LL keyboard hook to the hotkey-capture dialog. */
static const UINT WM_KEYCAP    = WM_APP + 2;

static const UINT TRAY_UID     = 1;
static const int  HOTKEY_ID    = 1;

/** First menu ID for pinned-window entries (up to +99). */
static const UINT IDM_PIN_BASE   = 2000;
static const UINT IDM_PINNED_DLG = 3000; /* "Zarządzaj przypiętymi…" */
static const UINT IDM_HOTKEY     = 3001;
static const UINT IDM_UNPIN_ALL  = 3002;
static const UINT IDM_AUTOSTART  = 3003;
static const UINT IDM_EXIT       = 3004;

/* ═══════════════════════════════════════════════════════════════════════════
   Global state
   ══════════════════════════════════════════════════════════════════════════*/

static HINSTANCE        g_hInst      = nullptr;
static HWND             g_hwnd       = nullptr;     /* hidden message-only window */
static HICON            g_iconIdle   = nullptr;
static HICON            g_iconActive = nullptr;
static NOTIFYICONDATAW  g_nid        = {};

static std::vector<HWND> g_pinned;                  /* currently pinned HWNDs */

static UINT  g_modifiers = MOD_ALT | MOD_SHIFT;
static UINT  g_vk        = (UINT)'T';
static bool  g_autostart = true;
static WCHAR g_iniPath[MAX_PATH];

/* ── Hotkey-capture dialog state ────────────────────────────────────────── */
static HHOOK g_capHook  = nullptr;   /* active WH_KEYBOARD_LL hook */
static HWND  g_capDlg   = nullptr;   /* the capture dialog HWND    */
static UINT  g_capMods  = 0;
static UINT  g_capVK    = 0;
static bool  g_capValid = false;

/* ═══════════════════════════════════════════════════════════════════════════
   INI configuration helpers
   ══════════════════════════════════════════════════════════════════════════*/

static void Config_GetPath()
{
    GetModuleFileNameW(nullptr, g_iniPath, MAX_PATH);
    WCHAR* dot = wcsrchr(g_iniPath, L'.');
    if (dot) wcscpy_s(dot, 5, L".ini");
    else StringCchCatW(g_iniPath, MAX_PATH, L".ini");
}

static void Config_Load()
{
    g_modifiers = GetPrivateProfileIntW(INI_SECT, INI_MOD,  MOD_ALT | MOD_SHIFT, g_iniPath);
    g_vk        = GetPrivateProfileIntW(INI_SECT, INI_VK,   (UINT)'T',            g_iniPath);
    g_autostart = GetPrivateProfileIntW(INI_SECT, INI_AUTO, 1,                    g_iniPath) != 0;

    if (g_modifiers == 0) g_modifiers = MOD_ALT | MOD_SHIFT;
    if (g_vk        == 0) g_vk        = (UINT)'T';
}

static void Config_Save()
{
    WCHAR buf[16];
    StringCchPrintfW(buf, 16, L"%u", g_modifiers);
    WritePrivateProfileStringW(INI_SECT, INI_MOD,  buf,                    g_iniPath);
    StringCchPrintfW(buf, 16, L"%u", g_vk);
    WritePrivateProfileStringW(INI_SECT, INI_VK,   buf,                    g_iniPath);
    WritePrivateProfileStringW(INI_SECT, INI_AUTO,  g_autostart ? L"1" : L"0", g_iniPath);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Registry – autostart
   ══════════════════════════════════════════════════════════════════════════*/

static bool Reg_GetAutostart()
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    DWORD sz = 0;
    bool found = RegQueryValueExW(hk, APP_NAME, nullptr, nullptr, nullptr, &sz) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return found;
}

static void Reg_SetAutostart(bool enable)
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return;
    if (enable) {
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        RegSetValueExW(hk, APP_NAME, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(path),
            static_cast<DWORD>((wcslen(path) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(hk, APP_NAME);
    }
    RegCloseKey(hk);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Pinned-window management
   ══════════════════════════════════════════════════════════════════════════*/

/** Remove stale HWNDs (destroyed windows) from the pinned list. */
static void Pinned_Clean()
{
    g_pinned.erase(
        std::remove_if(g_pinned.begin(), g_pinned.end(),
            [](HWND h){ return !IsWindow(h); }),
        g_pinned.end());
}

static bool Pinned_Contains(HWND hwnd)
{
    for (HWND h : g_pinned) if (h == hwnd) return true;
    return false;
}

static void Pin(HWND hwnd)
{
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    g_pinned.push_back(hwnd);
}

static void Unpin(HWND hwnd)
{
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    g_pinned.erase(std::remove(g_pinned.begin(), g_pinned.end(), hwnd), g_pinned.end());
}

static void Unpin_All()
{
    Pinned_Clean();
    /* copy because Unpin() modifies g_pinned */
    auto copy = g_pinned;
    for (HWND h : copy) Unpin(h);
}

/**
 * Toggle the always-on-top state of the given window.
 * Ignores our own hidden window.
 */
static void ToggleWindow(HWND hwnd)
{
    if (!hwnd || hwnd == g_hwnd) return;
    Pinned_Clean();
    if (Pinned_Contains(hwnd)) Unpin(hwnd);
    else                       Pin(hwnd);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Tray icon
   ══════════════════════════════════════════════════════════════════════════*/

static void Tray_Update()
{
    Pinned_Clean();
    bool active  = !g_pinned.empty();
    g_nid.hIcon  = active ? g_iconActive : g_iconIdle;
    wcscpy_s(g_nid.szTip,
        active ? L"AlwaysOnTop – Active" : L"AlwaysOnTop");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void Tray_Add()
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = TRAY_UID;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_iconIdle;
    wcscpy_s(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void Tray_Remove()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Hotkey formatting
   ══════════════════════════════════════════════════════════════════════════*/

/**
 * Write a human-readable hotkey string (e.g. "Alt+Shift+T") into buf.
 * Uses GetKeyNameText for the key name so it respects the keyboard layout.
 */
static void FormatHotkey(WCHAR* buf, size_t cch, UINT mods, UINT vk)
{
    buf[0] = L'\0';
    if (mods & MOD_CONTROL) StringCchCatW(buf, cch, L"Ctrl+");
    if (mods & MOD_ALT)     StringCchCatW(buf, cch, L"Alt+");
    if (mods & MOD_SHIFT)   StringCchCatW(buf, cch, L"Shift+");
    if (mods & MOD_WIN)     StringCchCatW(buf, cch, L"Win+");

    WCHAR key[64] = L"?";
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc) GetKeyNameTextW(static_cast<LONG>(sc << 16), key, 64);
    StringCchCatW(buf, cch, key);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Low-level keyboard hook  (active only while the capture dialog is open)
   ══════════════════════════════════════════════════════════════════════════*/

static LRESULT CALLBACK KeyboardLLProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        UINT vk  = kb->vkCode;

        /* Suppress all key-up events while capture dialog is open */
        if (wp == WM_KEYUP || wp == WM_SYSKEYUP)
            return 1;

        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
            /* Skip pure modifier keys – we need a non-modifier key */
            bool isPureMod = (vk == VK_SHIFT   || vk == VK_LSHIFT  || vk == VK_RSHIFT  ||
                              vk == VK_CONTROL  || vk == VK_LCONTROL|| vk == VK_RCONTROL||
                              vk == VK_MENU     || vk == VK_LMENU   || vk == VK_RMENU   ||
                              vk == VK_LWIN     || vk == VK_RWIN);
            if (!isPureMod) {
                UINT mods = 0;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
                if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
                if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)
                    mods |= MOD_WIN;

                if (mods != 0) {  /* at least one modifier required */
                    g_capVK    = vk;
                    g_capMods  = mods;
                    g_capValid = true;
                    PostMessageW(g_capDlg, WM_KEYCAP, 0, 0);
                }
            }
            return 1; /* suppress – input goes nowhere while dialog is open */
        }
    }
    return CallNextHookEx(g_capHook, code, wp, lp);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Hotkey-capture dialog
   ══════════════════════════════════════════════════════════════════════════*/

static INT_PTR CALLBACK HotkeyDlgProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM)
{
    switch (msg) {

    case WM_INITDIALOG: {
        g_capDlg   = hdlg;
        g_capValid = false;

        /* Force dialog to stay on top of other pinned windows */
        SetWindowPos(hdlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        /* Show current hotkey as the starting value */
        WCHAR buf[128];
        FormatHotkey(buf, 128, g_modifiers, g_vk);
        SetDlgItemTextW(hdlg, IDC_HOTKEY_EDIT, buf);

        /* Install LL hook to capture all keyboard input system-wide */
        g_capHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardLLProc, g_hInst, 0);
        return TRUE;
    }

    case WM_KEYCAP: {
        /* Fired by KeyboardLLProc when a valid combination is pressed */
        WCHAR buf[128];
        FormatHotkey(buf, 128, g_capMods, g_capVK);
        SetDlgItemTextW(hdlg, IDC_HOTKEY_EDIT, buf);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            if (!g_capValid) {
                MessageBoxW(hdlg,
                    L"Press a key combination that includes at least\n"
                    L"one modifier: Ctrl, Alt, Shift, or Win.",
                    L"Hotkey", MB_ICONINFORMATION);
                return TRUE;
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (g_capHook) {
            UnhookWindowsHookEx(g_capHook);
            g_capHook = nullptr;
        }
        g_capDlg = nullptr;
        break;
    }
    return FALSE;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Pinned-windows list dialog
   ══════════════════════════════════════════════════════════════════════════*/

static INT_PTR CALLBACK PinnedDlgProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM)
{
    switch (msg) {

    case WM_INITDIALOG: {
        /* Force list dialog to stay on top of everything */
        SetWindowPos(hdlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        Pinned_Clean();
        HWND lb = GetDlgItem(hdlg, IDC_PINNED_LIST);

        if (g_pinned.empty()) {
            SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"(no pinned windows)");
            EnableWindow(lb,                              FALSE);
            EnableWindow(GetDlgItem(hdlg, IDC_UNPIN_BTN), FALSE);
        } else {
            for (HWND h : g_pinned) {
                WCHAR title[256] = {};
                GetWindowTextW(h, title, 256);
                if (!title[0]) wcscpy_s(title, L"(untitled)");

                int idx = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)title);
                SendMessageW(lb, LB_SETITEMDATA, idx, (LPARAM)h);
            }
        }
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {

        case IDC_UNPIN_BTN: {
            HWND lb  = GetDlgItem(hdlg, IDC_PINNED_LIST);
            int  sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) break;

            HWND tgt = (HWND)SendMessageW(lb, LB_GETITEMDATA, sel, 0);
            Unpin(tgt);
            SendMessageW(lb, LB_DELETESTRING, sel, 0);
            Tray_Update();

            /* If the list is now empty, show placeholder and disable controls */
            if (SendMessageW(lb, LB_GETCOUNT, 0, 0) == 0) {
                SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"(no pinned windows)");
                EnableWindow(lb,                                FALSE);
                EnableWindow(GetDlgItem(hdlg, IDC_UNPIN_BTN), FALSE);
            }
            break;
        }

        case IDOK:
        case IDCANCEL:
            EndDialog(hdlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Tray context menu
   ══════════════════════════════════════════════════════════════════════════*/

static void ShowContextMenu()
{
    Pinned_Clean();

    HMENU hMenu = CreatePopupMenu();

    /* ── Pinned windows (direct entries, click = unpin) ── */
    if (g_pinned.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0,
            L"(no pinned windows)");
    } else {
        for (size_t i = 0; i < g_pinned.size() && i < 99; i++) {
            WCHAR title[256] = {};
            GetWindowTextW(g_pinned[i], title, 256);
            if (!title[0]) wcscpy_s(title, L"(untitled)");

            /* Prepend check-mark to signal "currently pinned" */
            WCHAR item[270];
            StringCchPrintfW(item, 270, L"\u2714 %s", title);
            AppendMenuW(hMenu, MF_STRING, IDM_PIN_BASE + (UINT)i, item);
        }
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING,    IDM_PINNED_DLG, L"Manage Pinned Windows...");

    /* ── Settings ── */
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    WCHAR hkLabel[200] = L"Change Hotkey (";
    WCHAR hkBuf[64];
    FormatHotkey(hkBuf, 64, g_modifiers, g_vk);
    StringCchCatW(hkLabel, 200, hkBuf);
    StringCchCatW(hkLabel, 200, L")");
    AppendMenuW(hMenu, MF_STRING, IDM_HOTKEY, hkLabel);

    UINT unpinAllFlags = g_pinned.empty()
        ? (MF_STRING | MF_GRAYED)
        : MF_STRING;
    AppendMenuW(hMenu, unpinAllFlags, IDM_UNPIN_ALL, L"Unpin All");

    bool curAuto = Reg_GetAutostart();
    AppendMenuW(hMenu, MF_STRING | (curAuto ? MF_CHECKED : 0),
        IDM_AUTOSTART, L"Start with Windows");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    /* Display menu at cursor */
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = (UINT)TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_NONOTIFY,
        pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0); /* flush – required by SetForegroundWindow */
    DestroyMenu(hMenu);

    if (cmd == 0) return;

    /* ── Pinned-window range: click = unpin that window ── */
    if (cmd >= IDM_PIN_BASE && cmd < IDM_PIN_BASE + 99) {
        size_t idx = cmd - IDM_PIN_BASE;
        if (idx < g_pinned.size()) {
            Unpin(g_pinned[idx]);
            Tray_Update();
        }
        return;
    }

    switch (cmd) {

    case IDM_PINNED_DLG:
        DialogBoxW(g_hInst, MAKEINTRESOURCEW(IDD_PINNED), nullptr, PinnedDlgProc);
        break;

    case IDM_HOTKEY: {
        /* Must unregister while capture dialog runs, then re-register */
        UnregisterHotKey(g_hwnd, HOTKEY_ID);

        INT_PTR res = DialogBoxW(g_hInst, MAKEINTRESOURCEW(IDD_HOTKEY),
                                 nullptr, HotkeyDlgProc);
        if (res == IDOK && g_capValid) {
            g_modifiers = g_capMods;
            g_vk        = g_capVK;
            Config_Save();
        }

        if (!RegisterHotKey(g_hwnd, HOTKEY_ID, g_modifiers | MOD_NOREPEAT, g_vk)) {
            WCHAR hk[64], err[256];
            FormatHotkey(hk, 64, g_modifiers, g_vk);
            StringCchPrintfW(err, 256,
                L"Cannot register hotkey %s.\n"
                L"It might be in use by another application.", hk);
            MessageBoxW(g_hwnd, err, APP_NAME, MB_ICONWARNING);
        }
        break;
    }

    case IDM_UNPIN_ALL:
        Unpin_All();
        Tray_Update();
        break;

    case IDM_AUTOSTART: {
        bool newVal = !Reg_GetAutostart();
        Reg_SetAutostart(newVal);
        g_autostart = newVal;
        Config_Save();
        break;
    }

    case IDM_EXIT:
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main window procedure  (message-only hidden window)
   ══════════════════════════════════════════════════════════════════════════*/

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /** Registered message fired when the Taskbar/Explorer restarts. */
    static UINT s_taskbarCreated = 0;

    switch (msg) {

    case WM_CREATE:
        g_hwnd = hwnd;
        s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        Tray_Add();
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        Tray_Remove();
        /*
         * Intentionally do NOT unpin windows on exit.
         * Windows stay topmost so the user's layout is preserved
         * even after this helper exits.  "Odepnij wszystkie" exists
         * in the tray menu for an explicit full reset.
         */
        PostQuitMessage(0);
        return 0;

    /* ── Global hotkey pressed ──────────────────────────────────────────── */
    case WM_HOTKEY:
        if ((int)wp == HOTKEY_ID) {
            HWND fg = GetForegroundWindow();
            ToggleWindow(fg);
            Tray_Update();
        }
        return 0;

    /* ── Tray icon interactions ─────────────────────────────────────────── */
    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
            ShowContextMenu();
            break;
        case WM_LBUTTONDBLCLK:
            /* Double-click: open the pinned-windows manager dialog */
            DialogBoxW(g_hInst, MAKEINTRESOURCEW(IDD_PINNED), nullptr, PinnedDlgProc);
            break;
        }
        return 0;

    /* ── Explorer restart ───────────────────────────────────────────────── */
    default:
        if (s_taskbarCreated && msg == s_taskbarCreated) {
            Tray_Add();
            Tray_Update();
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Entry point
   ══════════════════════════════════════════════════════════════════════════*/

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    g_hInst = hInst;

    /* ── Single instance guard ──────────────────────────────────────────── */
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    Config_GetPath();
    Config_Load();

    /* ── Load tray icons ────────────────────────────────────────────────── */
    g_iconIdle   = static_cast<HICON>(LoadImageW(hInst,
        MAKEINTRESOURCEW(IDI_ICON_IDLE),   IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    g_iconActive = static_cast<HICON>(LoadImageW(hInst,
        MAKEINTRESOURCEW(IDI_ICON_ACTIVE), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    /* Fallback to standard application icon if resources are missing */
    if (!g_iconIdle)   g_iconIdle   = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_iconActive) g_iconActive = g_iconIdle;

    /* ── Register hidden window class ───────────────────────────────────── */
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    /*
     * We use a standard hidden top-level window (WS_POPUP) with the ToolWindow
     * extended style. This avoids Alt+Tab clutter, but still allows it to receive
     * taskbar restart broadcast messages (unlike HWND_MESSAGE).
     */
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WND_CLASS, APP_NAME, WS_POPUP,
                              0, 0, 0, 0,
                              nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    /* ── Autostart ──────────────────────────────────────────────────────── */
    if (g_autostart) Reg_SetAutostart(true);

    /* ── Register global hotkey ─────────────────────────────────────────── */
    if (!RegisterHotKey(g_hwnd, HOTKEY_ID, g_modifiers | MOD_NOREPEAT, g_vk)) {
        WCHAR hk[64], err[300];
        FormatHotkey(hk, 64, g_modifiers, g_vk);
        StringCchPrintfW(err, 300,
            L"Cannot register hotkey %s.\n"
            L"It might be in use by another application.\n\n"
            L"Right-click the tray icon to set a different hotkey.", hk);
        MessageBoxW(nullptr, err, APP_NAME, MB_ICONWARNING);
    }

    /* ── Message loop ───────────────────────────────────────────────────── */
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_hwnd, HOTKEY_ID);
    CloseHandle(hMutex);
    return static_cast<int>(msg.wParam);
}
