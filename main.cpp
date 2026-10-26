// QuickNote: press a global hotkey, type a line, press Enter, and the line is appended
// to today's note file. Plain Win32; needs nothing beyond Windows 10/11 itself.

#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wchar.h>
#include <wctype.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {

const wchar_t kAppName[] = L"QuickNote";
const wchar_t kPlaceholder[] = L"Quick note…   Enter to save · Esc to cancel";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const UINT WM_TRAY = WM_APP + 1;
const int kHotkeyId = 1;
const int kIconId = 1;  // see quicknote.rc
const int kMaxNoteLength = 4000;
const int kHistoryDepth = 100;
enum MenuId : UINT { IDM_OPEN = 1, IDM_CHANGE, IDM_STARTUP, IDM_EXIT };

// Popup size and font, in 96-DPI pixels; scaled to the monitor's DPI when shown.
const int kPopupWidth = 520, kPopupHeight = 48, kPadding = 14, kFontSize = 16;
const COLORREF kBackColor = RGB(0x20, 0x21, 0x24);
const COLORREF kTextColor = RGB(0xE8, 0xEA, 0xED);
const COLORREF kBorderColor = RGB(0x3C, 0x40, 0x43);

// Windows 11 DWM attributes, spelled out so older SDKs still build.
const DWORD kDwmCornerPreference = 33, kDwmBorderColor = 34;
const int kDwmCornerRound = 2;

struct Settings {
    wchar_t file[MAX_PATH];
    wchar_t notesFolder[MAX_PATH];
    wchar_t hotkey[64];
    bool startWithWindows;
};

Settings g_settings;
HWND g_messageWnd, g_popup, g_edit;
HFONT g_font;
UINT g_fontDpi;
HBRUSH g_backBrush;
HICON g_icon;
UINT g_taskbarCreated;
bool g_hiding;

// Undo history: the standard edit control only has a single undo step.
struct History {
    wchar_t* items[kHistoryDepth];
    int count;
};
History g_undo, g_redo;
wchar_t* g_currentText;  // the box's text as of the last change
bool g_restoringText;    // text is being set by the app, not edited by the user
bool g_typing;           // the change being processed is a typed character
wchar_t g_typedChar;
bool g_lastChangeTyped;
wchar_t g_lastTypedChar;

// ---- Settings (%AppData%\QuickNote\settings.ini) ----

void SaveSettings() {
    const Settings& s = g_settings;
    WritePrivateProfileStringW(kAppName, L"NotesFolder", s.notesFolder, s.file);
    WritePrivateProfileStringW(kAppName, L"Hotkey", s.hotkey, s.file);
    WritePrivateProfileStringW(kAppName, L"StartWithWindows", s.startWithWindows ? L"1" : L"0", s.file);
}

void LoadSettings() {
    Settings& s = g_settings;
    wchar_t folder[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, folder);
    wcscat_s(folder, L"\\QuickNote");
    CreateDirectoryW(folder, nullptr);
    swprintf_s(s.file, L"%ls\\settings.ini", folder);

    // A new file starts with a UTF-16 BOM so Windows writes the INI as Unicode, not ANSI.
    bool firstRun = GetFileAttributesW(s.file) == INVALID_FILE_ATTRIBUTES;
    if (firstRun) {
        HANDLE f = CreateFileW(s.file, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(f, "\xFF\xFE", 2, &written, nullptr);
            CloseHandle(f);
        }
    }

    wchar_t defaultFolder[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, defaultFolder);
    wcscat_s(defaultFolder, L"\\QuickNotes");
    GetPrivateProfileStringW(kAppName, L"NotesFolder", defaultFolder, s.notesFolder, MAX_PATH, s.file);
    GetPrivateProfileStringW(kAppName, L"Hotkey", L"Ctrl+Space", s.hotkey, ARRAYSIZE(s.hotkey), s.file);
    s.startWithWindows = GetPrivateProfileIntW(kAppName, L"StartWithWindows", 0, s.file) != 0;
    if (firstRun)
        SaveSettings();
}

// ---- Hotkey parsing ("Ctrl+Space", "Ctrl+Alt+N", ...) ----

UINT KeyFromName(const wchar_t* name) {
    if (name[0] && !name[1]) {
        wchar_t c = towupper(name[0]);
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))
            return c;
    }
    if ((name[0] == L'F' || name[0] == L'f') && name[1]) {
        int n = _wtoi(name + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + n - 1;
    }
    static const struct { const wchar_t* name; UINT vk; } keys[] = {
        {L"Space", VK_SPACE}, {L"Enter", VK_RETURN}, {L"Tab", VK_TAB}, {L"Insert", VK_INSERT},
        {L"Delete", VK_DELETE}, {L"Home", VK_HOME}, {L"End", VK_END}, {L"PageUp", VK_PRIOR},
        {L"PageDown", VK_NEXT}, {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"Left", VK_LEFT},
        {L"Right", VK_RIGHT}, {L"Backquote", VK_OEM_3},
    };
    for (const auto& key : keys)
        if (!_wcsicmp(name, key.name))
            return key.vk;
    return 0;
}

bool ParseHotkey(const wchar_t* text, UINT* modifiers, UINT* vk) {
    *modifiers = 0;
    *vk = 0;
    wchar_t buffer[64];
    wcsncpy_s(buffer, text, _TRUNCATE);
    wchar_t* context = nullptr;
    for (wchar_t* part = wcstok_s(buffer, L"+", &context); part; part = wcstok_s(nullptr, L"+", &context)) {
        while (*part == L' ')
            ++part;
        size_t len = wcslen(part);
        while (len && part[len - 1] == L' ')
            part[--len] = 0;

        if (!_wcsicmp(part, L"Ctrl") || !_wcsicmp(part, L"Control")) *modifiers |= MOD_CONTROL;
        else if (!_wcsicmp(part, L"Alt")) *modifiers |= MOD_ALT;
        else if (!_wcsicmp(part, L"Shift")) *modifiers |= MOD_SHIFT;
        else if (!_wcsicmp(part, L"Win")) *modifiers |= MOD_WIN;
        else if (*vk || !(*vk = KeyFromName(part)))
            return false;
    }
    return *vk != 0;
}

// ---- Tray icon ----

NOTIFYICONDATAW TrayData() {
    NOTIFYICONDATAW nid = {sizeof nid};
    nid.hWnd = g_messageWnd;
    nid.uID = 1;
    return nid;
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    swprintf_s(nid.szTip, L"QuickNote (%ls)", g_settings.hotkey);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void ShowError(const wchar_t* message) {
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, kAppName);
    wcsncpy_s(nid.szInfo, message, _TRUNCATE);
    nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---- Tray menu actions ----

void OpenNotesFolder() {
    SHCreateDirectoryExW(nullptr, g_settings.notesFolder, nullptr);
    ShellExecuteW(nullptr, L"open", g_settings.notesFolder, nullptr, nullptr, SW_SHOWNORMAL);
}

void ChangeNotesFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose where QuickNote saves notes");
    IShellItem* current = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(g_settings.notesFolder, nullptr, IID_PPV_ARGS(&current)))) {
        dialog->SetFolder(current);
        current->Release();
    }
    IShellItem* result = nullptr;
    if (SUCCEEDED(dialog->Show(g_messageWnd)) && SUCCEEDED(dialog->GetResult(&result))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            wcsncpy_s(g_settings.notesFolder, path, _TRUNCATE);
            CoTaskMemFree(path);
            SaveSettings();
        }
        result->Release();
    }
    dialog->Release();
}

// Adds or removes QuickNote from the current user's "run at login" list.
bool SetStartWithWindows(bool enable) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS status;
    if (enable) {
        wchar_t exe[MAX_PATH], value[MAX_PATH + 2];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        swprintf_s(value, L"\"%ls\"", exe);
        status = RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                                static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kAppName);
        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

void ToggleStartWithWindows() {
    bool enable = !g_settings.startWithWindows;
    if (!SetStartWithWindows(enable)) {
        ShowError(L"Couldn't change the startup setting.");
        return;
    }
    g_settings.startWithWindows = enable;
    SaveSettings();
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open notes folder");
    AppendMenuW(menu, MF_STRING, IDM_CHANGE, L"Change notes folder…");
    AppendMenuW(menu, MF_STRING | (g_settings.startWithWindows ? MF_CHECKED : 0), IDM_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(menu, IDM_OPEN, FALSE);

    // The menu only closes on an outside click if our window is foreground while it's open.
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_messageWnd);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_messageWnd, nullptr);
    PostMessageW(g_messageWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (command) {
    case IDM_OPEN: OpenNotesFolder(); break;
    case IDM_CHANGE: ChangeNotesFolder(); break;
    case IDM_STARTUP: ToggleStartWithWindows(); break;
    case IDM_EXIT: PostQuitMessage(0); break;
    }
}

// ---- Notes ----

// Appends "[HH:MM:SS] text" to <notes folder>\YYYY-MM-DD.txt as UTF-8. Returns a Win32 error code.
DWORD AppendNote(const wchar_t* text) {
    SHCreateDirectoryExW(nullptr, g_settings.notesFolder, nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH + 16];
    swprintf_s(path, L"%ls\\%04u-%02u-%02u.txt", g_settings.notesFolder, t.wYear, t.wMonth, t.wDay);

    static wchar_t line[kMaxNoteLength + 32];
    static char utf8[ARRAYSIZE(line) * 3];
    swprintf_s(line, L"[%02u:%02u:%02u] %ls\r\n", t.wHour, t.wMinute, t.wSecond, text);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, nullptr, nullptr) - 1;

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError();
    DWORD written;
    DWORD error = WriteFile(file, utf8, bytes, &written, nullptr) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return error;
}

// ---- Note box editing: undo history and word deletion ----

wchar_t* CopyText(const wchar_t* text) {
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    wchar_t* copy = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, bytes));
    memcpy(copy, text, bytes);
    return copy;
}

void FreeText(wchar_t* text) {
    if (text)
        HeapFree(GetProcessHeap(), 0, text);
}

wchar_t* GetBoxText() {
    int length = GetWindowTextLengthW(g_edit);
    wchar_t* text = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(wchar_t)));
    GetWindowTextW(g_edit, text, length + 1);
    return text;
}

void PushHistory(History* history, wchar_t* text) {
    if (history->count == kHistoryDepth) {  // drop the oldest step
        FreeText(history->items[0]);
        memmove(history->items, history->items + 1, (kHistoryDepth - 1) * sizeof(wchar_t*));
        --history->count;
    }
    history->items[history->count++] = text;
}

wchar_t* PopHistory(History* history) {
    return history->count ? history->items[--history->count] : nullptr;
}

void ClearHistory(History* history) {
    while (history->count)
        FreeText(history->items[--history->count]);
}

void ResetHistory() {
    ClearHistory(&g_undo);
    ClearHistory(&g_redo);
    FreeText(g_currentText);
    g_currentText = CopyText(L"");
    g_lastChangeTyped = false;
}

// Called whenever the box's text changes. Typing is grouped into word-sized undo steps:
// a new step starts at the first letter typed after a space, or after any non-typing edit.
void RecordChange() {
    if (g_restoringText)
        return;
    bool continuesWord = g_typing && g_lastChangeTyped && !(iswspace(g_lastTypedChar) && !iswspace(g_typedChar));
    if (continuesWord)
        FreeText(g_currentText);
    else
        PushHistory(&g_undo, g_currentText);
    g_currentText = GetBoxText();
    ClearHistory(&g_redo);
    g_lastChangeTyped = g_typing;
    if (g_typing)
        g_lastTypedChar = g_typedChar;
}

// Shows a text from the history, with the caret where it differs from `previous`.
void RestoreText(wchar_t* text, const wchar_t* previous) {
    g_restoringText = true;
    SetWindowTextW(g_edit, text);
    g_restoringText = false;
    size_t length = wcslen(text), previousLength = wcslen(previous), sameEnding = 0;
    while (sameEnding < length && sameEnding < previousLength &&
           text[length - 1 - sameEnding] == previous[previousLength - 1 - sameEnding])
        ++sameEnding;
    SendMessageW(g_edit, EM_SETSEL, length - sameEnding, length - sameEnding);
    g_currentText = text;
    g_lastChangeTyped = false;
}

void Undo() {
    if (wchar_t* text = PopHistory(&g_undo)) {
        PushHistory(&g_redo, g_currentText);
        RestoreText(text, g_redo.items[g_redo.count - 1]);
    }
}

void Redo() {
    if (wchar_t* text = PopHistory(&g_redo)) {
        PushHistory(&g_undo, g_currentText);
        RestoreText(text, g_undo.items[g_undo.count - 1]);
    }
}

bool IsWordChar(wchar_t c) {
    return iswalnum(c) || c == L'_';
}

// Ctrl+Backspace / Ctrl+Delete: deletes to the previous / next word boundary, like most editors.
void DeleteWord(bool forward) {
    DWORD start = 0, end = 0;
    SendMessageW(g_edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (start == end) {
        wchar_t* text = GetBoxText();
        DWORD length = static_cast<DWORD>(wcslen(text));
        if (forward) {
            // The rest of this word (or run of punctuation), then the spaces after it.
            if (end < length && IsWordChar(text[end]))
                while (end < length && IsWordChar(text[end])) ++end;
            else
                while (end < length && !IsWordChar(text[end]) && !iswspace(text[end])) ++end;
            while (end < length && iswspace(text[end])) ++end;
        } else {
            // The spaces before the caret, then the word (or run of punctuation) before them.
            while (start > 0 && iswspace(text[start - 1])) --start;
            if (start > 0 && IsWordChar(text[start - 1]))
                while (start > 0 && IsWordChar(text[start - 1])) --start;
            else
                while (start > 0 && !IsWordChar(text[start - 1]) && !iswspace(text[start - 1])) --start;
        }
        FreeText(text);
        SendMessageW(g_edit, EM_SETSEL, start, end);
    }
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
}

// ---- Popup ----

void HidePopup() {
    if (g_hiding || !IsWindowVisible(g_popup))
        return;
    g_hiding = true;
    g_restoringText = true;
    SetWindowTextW(g_edit, L"");
    g_restoringText = false;
    ResetHistory();
    ShowWindow(g_popup, SW_HIDE);
    g_hiding = false;
}

void SubmitNote() {
    static wchar_t text[kMaxNoteLength + 1];
    GetWindowTextW(g_edit, text, ARRAYSIZE(text));

    // Flatten pasted line breaks and trim, so each note stays one line.
    for (wchar_t* c = text; *c; ++c)
        if (*c == L'\r' || *c == L'\n' || *c == L'\t')
            *c = L' ';
    wchar_t* start = text;
    while (*start == L' ')
        ++start;
    size_t len = wcslen(start);
    while (len && start[len - 1] == L' ')
        start[--len] = 0;

    if (*start) {
        if (DWORD error = AppendNote(start)) {
            // Keep the box open with the text so the note isn't lost.
            wchar_t reason[256] = L"";
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                           reason, ARRAYSIZE(reason), nullptr);
            wchar_t message[320];
            swprintf_s(message, L"Couldn't save the note: %ls", reason);
            ShowError(message);
            return;
        }
    }
    HidePopup();
}

// Keyboard handling for the note box that the standard edit control lacks.
// Returns true if the message was handled and shouldn't reach the control.
bool HandleBoxMessage(const MSG& msg) {
    if (msg.message == WM_CHAR) {
        g_typing = msg.wParam >= L' ';  // cleared again after the character is dispatched
        g_typedChar = static_cast<wchar_t>(msg.wParam);
        return false;
    }
    if (msg.message != WM_KEYDOWN)
        return false;

    bool ctrl = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0;  // not AltGr
    bool shift = GetKeyState(VK_SHIFT) < 0;
    switch (msg.wParam) {
    case VK_RETURN: SubmitNote(); return true;
    case VK_ESCAPE: HidePopup(); return true;
    case VK_BACK: if (ctrl) { DeleteWord(false); return true; } break;
    case VK_DELETE: if (ctrl && !shift) { DeleteWord(true); return true; } break;
    case 'A': if (ctrl) { SendMessageW(g_edit, EM_SETSEL, 0, -1); return true; } break;
    case 'Z': if (ctrl) { shift ? Redo() : Undo(); return true; } break;
    case 'Y': if (ctrl) { Redo(); return true; } break;
    }
    return false;
}

// Windows may refuse to let a background app take focus. Attaching to the
// foreground window's input queue for a moment lets the request through.
void ForceForeground(HWND hwnd) {
    if (SetForegroundWindow(hwnd))
        return;
    DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD thisThread = GetCurrentThreadId();
    if (foregroundThread != thisThread && AttachThreadInput(thisThread, foregroundThread, TRUE)) {
        SetForegroundWindow(hwnd);
        AttachThreadInput(thisThread, foregroundThread, FALSE);
    }
}

// Centers the box horizontally, a quarter of the way down the monitor the cursor is on.
void ShowPopup() {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof info};
    GetMonitorInfoW(monitor, &info);
    UINT dpi = 96, dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &dpiY);

    if (dpi != g_fontDpi) {
        if (g_font)
            DeleteObject(g_font);
        g_font = CreateFontW(-MulDiv(kFontSize, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_fontDpi = dpi;
        SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }

    HDC dc = GetDC(g_edit);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    TEXTMETRICW metrics;
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, oldFont);
    ReleaseDC(g_edit, dc);

    int width = MulDiv(kPopupWidth, dpi, 96), height = MulDiv(kPopupHeight, dpi, 96);
    int padding = MulDiv(kPadding, dpi, 96), lineHeight = metrics.tmHeight + 2;
    const RECT& work = info.rcWork;
    int x = work.left + (work.right - work.left - width) / 2;
    int y = work.top + (work.bottom - work.top) / 4;

    SetWindowPos(g_popup, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    SetWindowPos(g_edit, nullptr, padding, (height - lineHeight) / 2, width - 2 * padding, lineHeight, SWP_NOZORDER);
    ShowWindow(g_popup, SW_SHOW);
    ForceForeground(g_popup);
    SetFocus(g_edit);
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            HidePopup();  // clicked somewhere else
        return 0;
    case WM_SETFOCUS:
        SetFocus(g_edit);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE)
            RecordChange();
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wParam), kTextColor);
        SetBkColor(reinterpret_cast<HDC>(wParam), kBackColor);
        return reinterpret_cast<LRESULT>(g_backBrush);
    case WM_CLOSE:  // Alt+F4 only hides; the window is reused for the app's lifetime
        HidePopup();
        return 0;
    case WM_DPICHANGED:  // ShowPopup already sizes for the target monitor
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Hidden window that receives the hotkey and tray icon messages.
LRESULT CALLBACK MessageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HOTKEY:
        if (IsWindowVisible(g_popup))
            HidePopup();
        else
            ShowPopup();
        return 0;
    case WM_TRAY:
        switch (LOWORD(lParam)) {
        case WM_CONTEXTMENU: ShowTrayMenu(); break;
        case WM_LBUTTONDBLCLK: OpenNotesFolder(); break;
        }
        return 0;
    }
    if (msg == g_taskbarCreated) {  // Explorer restarted; put the icon back
        AddTrayIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateWindows(HINSTANCE instance) {
    WNDCLASSEXW messageClass = {sizeof messageClass};
    messageClass.lpfnWndProc = MessageProc;
    messageClass.hInstance = instance;
    messageClass.lpszClassName = L"QuickNoteMessage";
    RegisterClassExW(&messageClass);
    g_messageWnd = CreateWindowExW(WS_EX_TOOLWINDOW, messageClass.lpszClassName, kAppName, WS_POPUP,
                                   0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW popupClass = {sizeof popupClass};
    popupClass.style = CS_DROPSHADOW;
    popupClass.lpfnWndProc = PopupProc;
    popupClass.hInstance = instance;
    popupClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    popupClass.hbrBackground = g_backBrush;
    popupClass.lpszClassName = L"QuickNotePopup";
    RegisterClassExW(&popupClass);

    // Tool window + no owner: never appears in the taskbar or Alt+Tab.
    g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, popupClass.lpszClassName, kAppName, WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    DwmSetWindowAttribute(g_popup, kDwmCornerPreference, &kDwmCornerRound, sizeof kDwmCornerRound);
    DwmSetWindowAttribute(g_popup, kDwmBorderColor, &kBorderColor, sizeof kBorderColor);

    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             0, 0, 0, 0, g_popup, nullptr, instance, nullptr);
    SendMessageW(g_edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(kPlaceholder));
    SendMessageW(g_edit, EM_SETLIMITTEXT, kMaxNoteLength, 0);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Only one copy may run, otherwise the second one can't register the hotkey.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\QuickNote.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls = {sizeof controls, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    LoadSettings();
    g_backBrush = CreateSolidBrush(kBackColor);
    // Picks the icon size that matches the tray at the current display scaling.
    LoadIconMetric(instance, MAKEINTRESOURCEW(kIconId), LIM_SMALL, &g_icon);
    CreateWindows(instance);
    AddTrayIcon();

    // Re-point the startup entry at this exe in case it was moved.
    if (g_settings.startWithWindows && !SetStartWithWindows(true))
        ShowError(L"Couldn't update the startup entry.");

    UINT modifiers, vk;
    wchar_t message[256];
    if (!ParseHotkey(g_settings.hotkey, &modifiers, &vk)) {
        swprintf_s(message, L"Couldn't understand the hotkey \"%ls\" in settings.ini.", g_settings.hotkey);
        ShowError(message);
    } else if (!RegisterHotKey(g_messageWnd, kHotkeyId, modifiers | MOD_NOREPEAT, vk)) {
        swprintf_s(message, L"%ls is already used by another app. Change Hotkey in settings.ini and restart QuickNote.",
                   g_settings.hotkey);
        ShowError(message);
    }

    ResetHistory();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == g_edit && HandleBoxMessage(msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        g_typing = false;
    }

    UnregisterHotKey(g_messageWnd, kHotkeyId);
    NOTIFYICONDATAW nid = TrayData();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
