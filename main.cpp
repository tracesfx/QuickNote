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
const wchar_t kPlaceholder[] = L"Quick note…   Enter to save · Esc to cancel · Ctrl+K shortcuts";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const UINT WM_TRAY = WM_APP + 1;
const int kNoteHotkeyId = 1, kFolderHotkeyId = 2, kTestHotkeyId = 3;
const int kIconId = 1;  // see quicknote.rc
const UINT_PTR kSavedTimerId = 1;
const int kMaxNoteLength = 4000;
const int kHistoryDepth = 100;
enum MenuId : UINT { IDM_OPEN = 1, IDM_CHANGE, IDM_SHORTCUTS, IDM_STARTUP, IDM_EXIT };

// Popup size and font, in 96-DPI pixels; scaled to the monitor's DPI when shown.
const int kPopupWidth = 520, kPopupHeight = 48, kPadding = 14, kFontSize = 16;
const COLORREF kBackColor = RGB(0x20, 0x21, 0x24);
const COLORREF kTextColor = RGB(0xE8, 0xEA, 0xED);
const COLORREF kBorderColor = RGB(0x3C, 0x40, 0x43);
const COLORREF kMutedColor = RGB(0x9A, 0xA0, 0xA6);

// Recent notes above the box: row height and gap in 96-DPI pixels, and each row's opacity, newest first.
const int kRecentCount = 3;
const int kRecentHeight = 34, kRecentGap = 6;
const BYTE kRecentAlpha[kRecentCount] = {210, 140, 75};

// Animations, in milliseconds (distances in 96-DPI pixels). Skipped when Windows' animation effects are off.
const UINT_PTR kAnimationTimerId = 1;  // on the message window
const int kDrift = 8;                  // the box and notes settle downwards by this much when opening
const double kOpenTime = 120, kRowDelay = 40, kRowStagger = 30;
const double kCloseTime = 90;
const double kSaveShiftTime = 160, kSaveHoldTime = 40, kSaveBoxFadeTime = 150;
const double kHintFadeInTime = 200, kHintHoldTime = 3000, kHintFadeOutTime = 300;
const BYTE kHintAlpha = 235;
const int kHintHeight = 40;

// Windows 11 DWM attributes, spelled out so older SDKs still build.
const DWORD kDwmCornerPreference = 33, kDwmBorderColor = 34;
const int kDwmCornerRound = 2;

struct Hotkey {
    UINT modifiers;  // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
    UINT vk;
};
const Hotkey kDefaultNoteHotkey = {MOD_CONTROL, VK_SPACE};
const Hotkey kDefaultFolderHotkey = {MOD_CONTROL | MOD_SHIFT, VK_SPACE};

struct Settings {
    wchar_t file[MAX_PATH];
    wchar_t notesFolder[MAX_PATH];
    Hotkey noteHotkey;
    Hotkey folderHotkey;
    bool startWithWindows;
};

// What the popup is doing: taking a note, or recording new shortcuts (Ctrl+K).
enum class Mode { Note, RecordNoteHotkey, RecordFolderHotkey, Saved };

Settings g_settings;
HWND g_messageWnd, g_popup, g_edit;
HFONT g_font;
UINT g_fontDpi;
HBRUSH g_backBrush;
HICON g_icon;
UINT g_taskbarCreated;
Mode g_mode = Mode::Note;
Hotkey g_pendingNoteHotkey;
wchar_t* g_noteBeforeRecording;  // restored if shortcut recording is cancelled
bool g_showingProblem;           // a rejected shortcut's message stays up until the next key press

// The last few notes, shown above the box, newest (and most opaque) first. The extra row holds a
// just-saved note while it slides into the stack.
struct RecentNote {
    wchar_t text[kMaxNoteLength + 1];
    wchar_t when[32];  // "14:32" for today's notes, otherwise the date
};
const int kSavedRow = kRecentCount;
RecentNote g_recent[kRecentCount + 1];
int g_recentCount;
HWND g_recentWnd[kRecentCount + 1];

// Where ShowPopup placed the box and the note rows above it.
struct Layout {
    int x, y, width, rowHeight, gap, top;
};
Layout g_layout;

// Hidden -> Shown (taking input) -> Closing (fading out, focus already handed back) -> Hidden.
enum class PopupState { Hidden, Shown, Closing };
PopupState g_popupState;
HWND g_previousWindow;  // had the focus before the box opened

bool g_firstRun;  // the settings file was just created
HWND g_hint;
HICON g_hintIcon;
wchar_t g_hintText[96];

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

// ---- Hotkey names ("Ctrl+Space", "Ctrl+Alt+N", ...) ----

const struct { const wchar_t* name; UINT vk; } kKeyNames[] = {
    {L"Space", VK_SPACE}, {L"Enter", VK_RETURN}, {L"Tab", VK_TAB}, {L"Insert", VK_INSERT},
    {L"Delete", VK_DELETE}, {L"Home", VK_HOME}, {L"End", VK_END}, {L"PageUp", VK_PRIOR},
    {L"PageDown", VK_NEXT}, {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"Left", VK_LEFT},
    {L"Right", VK_RIGHT}, {L"Pause", VK_PAUSE}, {L"Backquote", VK_OEM_3}, {L"Minus", VK_OEM_MINUS},
    {L"Equals", VK_OEM_PLUS}, {L"Comma", VK_OEM_COMMA}, {L"Period", VK_OEM_PERIOD}, {L"Slash", VK_OEM_2},
    {L"Semicolon", VK_OEM_1}, {L"Quote", VK_OEM_7}, {L"LeftBracket", VK_OEM_4},
    {L"RightBracket", VK_OEM_6}, {L"Backslash", VK_OEM_5},
};

bool SameHotkey(Hotkey a, Hotkey b) {
    return a.modifiers == b.modifiers && a.vk == b.vk;
}

bool IsLetterOrDigit(UINT vk) {
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9');
}

UINT KeyFromName(const wchar_t* name) {
    if (name[0] && !name[1]) {
        UINT c = towupper(name[0]);
        if (IsLetterOrDigit(c))
            return c;
    }
    if (!_wcsnicmp(name, L"Numpad", 6) && iswdigit(name[6]) && !name[7])
        return VK_NUMPAD0 + (name[6] - L'0');
    if ((name[0] == L'F' || name[0] == L'f') && iswdigit(name[1])) {
        int n = _wtoi(name + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + n - 1;
    }
    for (const auto& key : kKeyNames)
        if (!_wcsicmp(name, key.name))
            return key.vk;
    return 0;
}

// Writes the key's name, or returns false if the key can't be used in a hotkey.
bool KeyName(UINT vk, wchar_t* out, size_t count) {
    if (IsLetterOrDigit(vk)) {
        swprintf_s(out, count, L"%c", vk);
        return true;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        swprintf_s(out, count, L"Numpad%u", vk - VK_NUMPAD0);
        return true;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf_s(out, count, L"F%u", vk - VK_F1 + 1);
        return true;
    }
    for (const auto& key : kKeyNames) {
        if (key.vk == vk) {
            wcscpy_s(out, count, key.name);
            return true;
        }
    }
    return false;
}

// "Ctrl+Shift+Space"; with vk == 0, just the modifiers ("Ctrl+Shift+").
void FormatHotkey(Hotkey hotkey, wchar_t* out, size_t count) {
    out[0] = 0;
    if (hotkey.modifiers & MOD_CONTROL) wcscat_s(out, count, L"Ctrl+");
    if (hotkey.modifiers & MOD_ALT) wcscat_s(out, count, L"Alt+");
    if (hotkey.modifiers & MOD_SHIFT) wcscat_s(out, count, L"Shift+");
    if (hotkey.modifiers & MOD_WIN) wcscat_s(out, count, L"Win+");
    wchar_t key[16];
    if (hotkey.vk && KeyName(hotkey.vk, key, ARRAYSIZE(key)))
        wcscat_s(out, count, key);
}

bool ParseHotkey(const wchar_t* text, Hotkey* hotkey) {
    *hotkey = {};
    wchar_t buffer[64];
    wcsncpy_s(buffer, text, _TRUNCATE);
    wchar_t* context = nullptr;
    for (wchar_t* part = wcstok_s(buffer, L"+", &context); part; part = wcstok_s(nullptr, L"+", &context)) {
        while (*part == L' ')
            ++part;
        size_t len = wcslen(part);
        while (len && part[len - 1] == L' ')
            part[--len] = 0;

        if (!_wcsicmp(part, L"Ctrl") || !_wcsicmp(part, L"Control")) hotkey->modifiers |= MOD_CONTROL;
        else if (!_wcsicmp(part, L"Alt")) hotkey->modifiers |= MOD_ALT;
        else if (!_wcsicmp(part, L"Shift")) hotkey->modifiers |= MOD_SHIFT;
        else if (!_wcsicmp(part, L"Win")) hotkey->modifiers |= MOD_WIN;
        else if (hotkey->vk || !(hotkey->vk = KeyFromName(part)))
            return false;
    }
    return hotkey->vk != 0;
}

// ---- Settings (QuickNote.ini next to the exe) ----

void SaveSettings() {
    const Settings& s = g_settings;
    wchar_t hotkey[64];
    WritePrivateProfileStringW(kAppName, L"NotesFolder", s.notesFolder, s.file);
    FormatHotkey(s.noteHotkey, hotkey, ARRAYSIZE(hotkey));
    WritePrivateProfileStringW(kAppName, L"Hotkey", hotkey, s.file);
    FormatHotkey(s.folderHotkey, hotkey, ARRAYSIZE(hotkey));
    WritePrivateProfileStringW(kAppName, L"FolderHotkey", hotkey, s.file);
    WritePrivateProfileStringW(kAppName, L"StartWithWindows", s.startWithWindows ? L"1" : L"0", s.file);
}

// A new settings file starts with a UTF-16 BOM so Windows writes the INI as Unicode, not ANSI.
bool CreateSettingsFile(const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD written;
    WriteFile(f, "\xFF\xFE", 2, &written, nullptr);
    CloseHandle(f);
    return true;
}

void LoadSettings() {
    Settings& s = g_settings;
    // Settings live next to the exe, so QuickNote can be copied around as one folder.
    GetModuleFileNameW(nullptr, s.file, MAX_PATH);
    wchar_t* name = wcsrchr(s.file, L'\\') + 1;
    wcscpy_s(name, MAX_PATH - (name - s.file), L"QuickNote.ini");

    if (GetFileAttributesW(s.file) == INVALID_FILE_ATTRIBUTES && !(g_firstRun = CreateSettingsFile(s.file))) {
        // The exe's folder isn't writable (e.g. Program Files), so use %AppData%\QuickNote instead.
        wchar_t folder[MAX_PATH];
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, folder);
        wcscat_s(folder, L"\\QuickNote");
        CreateDirectoryW(folder, nullptr);
        swprintf_s(s.file, L"%ls\\QuickNote.ini", folder);
        if (GetFileAttributesW(s.file) == INVALID_FILE_ATTRIBUTES)
            g_firstRun = CreateSettingsFile(s.file);
    }

    wchar_t defaultFolder[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, defaultFolder);
    wcscat_s(defaultFolder, L"\\QuickNotes");
    GetPrivateProfileStringW(kAppName, L"NotesFolder", defaultFolder, s.notesFolder, MAX_PATH, s.file);
    wchar_t hotkey[64];
    GetPrivateProfileStringW(kAppName, L"Hotkey", L"", hotkey, ARRAYSIZE(hotkey), s.file);
    if (!ParseHotkey(hotkey, &s.noteHotkey))
        s.noteHotkey = kDefaultNoteHotkey;
    GetPrivateProfileStringW(kAppName, L"FolderHotkey", L"", hotkey, ARRAYSIZE(hotkey), s.file);
    if (!ParseHotkey(hotkey, &s.folderHotkey))
        s.folderHotkey = kDefaultFolderHotkey;
    s.startWithWindows = GetPrivateProfileIntW(kAppName, L"StartWithWindows", 0, s.file) != 0;
    // Also fills in keys added since the file was written (FolderHotkey).
    SaveSettings();
}

// ---- Tray icon ----

NOTIFYICONDATAW TrayData() {
    NOTIFYICONDATAW nid = {sizeof nid};
    nid.hWnd = g_messageWnd;
    nid.uID = 1;
    return nid;
}

void SetTrayTip(NOTIFYICONDATAW* nid) {
    wchar_t hotkey[64];
    FormatHotkey(g_settings.noteHotkey, hotkey, ARRAYSIZE(hotkey));
    nid->uFlags |= NIF_TIP | NIF_SHOWTIP;
    swprintf_s(nid->szTip, L"QuickNote (%ls)", hotkey);
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_ICON | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    SetTrayTip(&nid);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void UpdateTrayTip() {
    NOTIFYICONDATAW nid = TrayData();
    SetTrayTip(&nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowError(const wchar_t* message) {
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, kAppName);
    wcsncpy_s(nid.szInfo, message, _TRUNCATE);
    nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---- Global hotkeys ----

void UnregisterHotkeys() {
    UnregisterHotKey(g_messageWnd, kNoteHotkeyId);
    UnregisterHotKey(g_messageWnd, kFolderHotkeyId);
}

// Returns whether the new-note shortcut was registered.
bool RegisterHotkeys() {
    UnregisterHotkeys();
    const Hotkey hotkeys[] = {g_settings.noteHotkey, g_settings.folderHotkey};
    const int ids[] = {kNoteHotkeyId, kFolderHotkeyId};
    bool noteRegistered = true;
    for (int i = 0; i < 2; ++i) {
        if (!RegisterHotKey(g_messageWnd, ids[i], hotkeys[i].modifiers | MOD_NOREPEAT, hotkeys[i].vk)) {
            wchar_t name[64], message[256];
            FormatHotkey(hotkeys[i], name, ARRAYSIZE(name));
            swprintf_s(message, L"%ls is already used by another app. Pick another from the tray menu: "
                                L"Change shortcuts…", name);
            ShowError(message);
            noteRegistered = noteRegistered && i != 0;
        }
    }
    return noteRegistered;
}

// True if no other app has taken this hotkey. Only meaningful while ours are unregistered.
bool HotkeyAvailable(Hotkey hotkey) {
    if (!RegisterHotKey(g_messageWnd, kTestHotkeyId, hotkey.modifiers | MOD_NOREPEAT, hotkey.vk))
        return false;
    UnregisterHotKey(g_messageWnd, kTestHotkeyId);
    return true;
}

// ---- Tray menu actions ----

void ShowPopup();
void StartShortcutRecording();

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
    AppendMenuW(menu, MF_STRING, IDM_SHORTCUTS, L"Change shortcuts…");
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
    case IDM_SHORTCUTS:
        ShowPopup();
        StartShortcutRecording();
        break;
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

// True for "YYYY-MM-DD.txt".
bool IsNoteFileName(const wchar_t* name) {
    if (wcslen(name) != 14 || _wcsicmp(name + 10, L".txt"))
        return false;
    for (int i = 0; i < 10; ++i)
        if (i == 4 || i == 7 ? name[i] != L'-' : !iswdigit(name[i]))
            return false;
    return true;
}

// Replaces `name` with the newest note file that sorts before it. Dates sort by name.
bool PreviousNoteFile(wchar_t* name) {
    wchar_t before[15];
    wcscpy_s(before, name);
    wchar_t pattern[MAX_PATH + 16];
    swprintf_s(pattern, L"%ls\\*.txt", g_settings.notesFolder);
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE)
        return false;
    name[0] = 0;
    do {
        if (IsNoteFileName(found.cFileName) && _wcsicmp(found.cFileName, before) < 0 &&
            _wcsicmp(found.cFileName, name) > 0)
            wcscpy_s(name, 15, found.cFileName);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return name[0] != 0;
}

// Adds the last notes of one day's file to g_recent, newest first, until it's full.
void ReadRecentNotes(const wchar_t* fileName, bool today) {
    wchar_t path[MAX_PATH + 16];
    swprintf_s(path, L"%ls\\%ls", g_settings.notesFolder, fileName);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    // The tail is enough for the last few notes: each line is at most kMaxNoteLength characters.
    static char utf8[kRecentCount * (kMaxNoteLength * 3 + 32)];
    static wchar_t text[ARRAYSIZE(utf8) + 1];
    LARGE_INTEGER size = {};
    GetFileSizeEx(file, &size);
    LARGE_INTEGER start;
    start.QuadPart = size.QuadPart > static_cast<LONGLONG>(sizeof utf8) ? size.QuadPart - sizeof utf8 : 0;
    SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
    DWORD bytes = 0;
    ReadFile(file, utf8, sizeof utf8, &bytes, nullptr);
    CloseHandle(file);
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8, bytes, text, ARRAYSIZE(text) - 1);
    text[length] = 0;

    // Older days show their date instead of the time.
    wchar_t date[32] = L"";
    if (!today) {
        SYSTEMTIME day = {};
        day.wYear = static_cast<WORD>(_wtoi(fileName));
        day.wMonth = static_cast<WORD>(_wtoi(fileName + 5));
        day.wDay = static_cast<WORD>(_wtoi(fileName + 8));
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &day, L"MMM d", date, ARRAYSIZE(date), nullptr);
    }

    // Walk the lines from the end. If the read started mid-file, the first line may be cut off, so skip it.
    wchar_t* first = text;
    if (start.QuadPart > 0)
        while (*first && *first != L'\n')
            ++first;
    wchar_t* end = text + length;
    while (end > first && g_recentCount < kRecentCount) {
        wchar_t* line = end;
        while (line > first && line[-1] != L'\n')
            --line;
        wchar_t* next = line > first ? line - 1 : first;
        while (end > line && (end[-1] == L'\r' || end[-1] == L'\n' || end[-1] == L' '))
            --end;
        *end = 0;
        if (*line) {
            RecentNote& note = g_recent[g_recentCount++];
            // Lines look like "[HH:MM:SS] text"; anything else is shown whole.
            bool stamped = end - line > 11 && line[0] == L'[' && line[9] == L']' && line[10] == L' ';
            wcsncpy_s(note.text, stamped ? line + 11 : line, _TRUNCATE);
            if (!today)
                wcscpy_s(note.when, date);
            else if (stamped)
                swprintf_s(note.when, L"%.5ls", line + 1);
            else
                note.when[0] = 0;
        }
        end = next;
    }
}

// Loads the last few notes, newest first, looking back through earlier days if today has fewer.
void LoadRecentNotes() {
    g_recentCount = 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t today[16];
    swprintf_s(today, L"%04u-%02u-%02u.txt", t.wYear, t.wMonth, t.wDay);
    wchar_t name[15] = L"A";  // sorts after every date
    // A few files at most, in case some are empty.
    for (int files = 0; files < 5 && g_recentCount < kRecentCount && PreviousNoteFile(name); ++files)
        ReadRecentNotes(name, !_wcsicmp(name, today));
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

// Sets the box's text without recording it as an edit, and puts the caret at the end.
void SetBoxText(const wchar_t* text) {
    g_restoringText = true;
    SetWindowTextW(g_edit, text);
    g_restoringText = false;
    int length = GetWindowTextLengthW(g_edit);
    SendMessageW(g_edit, EM_SETSEL, length, length);
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
    if (g_restoringText || g_mode != Mode::Note)
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

// ---- Shortcut recording (Ctrl+K in the box, or "Change shortcuts…" in the tray menu) ----

bool IsModifierKey(UINT vk) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_LWIN: case VK_RWIN:
        return true;
    }
    return false;
}

UINT HeldModifiers() {
    UINT modifiers = 0;
    if (GetKeyState(VK_CONTROL) < 0) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) modifiers |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) modifiers |= MOD_WIN;
    return modifiers;
}

// Taking these over system-wide would break typing or everyday app shortcuts.
bool IsCommonShortcut(Hotkey hotkey) {
    if (hotkey.vk == VK_F4 && (hotkey.modifiers == MOD_ALT || hotkey.modifiers == MOD_CONTROL))
        return true;  // close window / tab
    return (hotkey.modifiers == MOD_CONTROL || hotkey.modifiers == MOD_ALT) && IsLetterOrDigit(hotkey.vk);
}

// Shift alone doesn't count: Shift+key just types. F13-F24 are free to use on their own.
bool HasRequiredModifier(Hotkey hotkey) {
    return (hotkey.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)) || (hotkey.vk >= VK_F13 && hotkey.vk <= VK_F24);
}

void ShowRecordingPrompt(UINT heldModifiers) {
    bool noteStep = g_mode == Mode::RecordNoteHotkey;
    const wchar_t* label = noteStep ? L"New note shortcut" : L"Open folder shortcut";
    wchar_t keys[64], text[160];
    if (heldModifiers) {
        FormatHotkey({heldModifiers, 0}, keys, ARRAYSIZE(keys));
        swprintf_s(text, L"%ls: %ls…", label, keys);
    } else {
        FormatHotkey(noteStep ? g_settings.noteHotkey : g_settings.folderHotkey, keys, ARRAYSIZE(keys));
        swprintf_s(text, L"%ls: press keys · Enter keeps %ls", label, keys);
    }
    SetBoxText(text);
    g_showingProblem = false;
}

void StartShortcutRecording() {
    if (g_mode != Mode::Note)
        return;
    UnregisterHotkeys();  // so pressing the current shortcuts reaches the box
    FreeText(g_noteBeforeRecording);
    g_noteBeforeRecording = GetBoxText();
    g_mode = Mode::RecordNoteHotkey;
    SendMessageW(g_edit, EM_SETREADONLY, TRUE, 0);
    ShowRecordingPrompt(0);
}

// Returns to the note box, with whatever was typed before Ctrl+K.
void StopShortcutRecording() {
    KillTimer(g_popup, kSavedTimerId);
    if (g_mode != Mode::Saved)
        RegisterHotkeys();
    g_mode = Mode::Note;
    SendMessageW(g_edit, EM_SETREADONLY, FALSE, 0);
    SetBoxText(g_noteBeforeRecording ? g_noteBeforeRecording : L"");
}

void HidePopup(bool saved = false);

void HandleRecordingKey(UINT vk, bool down, bool repeat) {
    if (repeat)
        return;
    if (g_mode == Mode::Saved) {
        if (down && !IsModifierKey(vk))
            HidePopup();
        return;
    }
    UINT modifiers = HeldModifiers();
    if (IsModifierKey(vk)) {
        // Pressing a modifier starts a new attempt; releasing one leaves a rejection message readable.
        if (down || !g_showingProblem)
            ShowRecordingPrompt(modifiers);
        return;
    }
    if (!down)
        return;
    if (vk == VK_ESCAPE && !modifiers) {
        StopShortcutRecording();
        return;
    }

    bool noteStep = g_mode == Mode::RecordNoteHotkey;
    Hotkey current = noteStep ? g_settings.noteHotkey : g_settings.folderHotkey;
    Hotkey hotkey = (vk == VK_RETURN && !modifiers) ? current : Hotkey{modifiers, vk};
    wchar_t name[64], problem[160];
    FormatHotkey(hotkey, name, ARRAYSIZE(name));
    if (!KeyName(hotkey.vk, problem, ARRAYSIZE(problem)))
        wcscpy_s(problem, L"That key can't be used — try another");
    else if (!HasRequiredModifier(hotkey))
        wcscpy_s(problem, L"Add Ctrl, Alt or Win — try again");
    else if (IsCommonShortcut(hotkey))
        swprintf_s(problem, L"%ls is a common app shortcut — try another", name);
    else if (!noteStep && SameHotkey(hotkey, g_pendingNoteHotkey))
        swprintf_s(problem, L"%ls is the new note shortcut — try another", name);
    else if (!HotkeyAvailable(hotkey))
        swprintf_s(problem, L"%ls is used by another app — try another", name);
    else
        problem[0] = 0;
    if (problem[0]) {
        SetBoxText(problem);
        g_showingProblem = true;
        return;
    }

    if (noteStep) {
        g_pendingNoteHotkey = hotkey;
        g_mode = Mode::RecordFolderHotkey;
        ShowRecordingPrompt(0);
        return;
    }
    g_settings.noteHotkey = g_pendingNoteHotkey;
    g_settings.folderHotkey = hotkey;
    SaveSettings();
    RegisterHotkeys();
    UpdateTrayTip();
    g_mode = Mode::Saved;

    wchar_t noteName[64], text[200];
    FormatHotkey(g_settings.noteHotkey, noteName, ARRAYSIZE(noteName));
    swprintf_s(text, L"Saved · New note: %ls · Open folder: %ls", noteName, name);
    SetBoxText(text);
    SetTimer(g_popup, kSavedTimerId, 2500, nullptr);
}

// ---- Animation ----
// Each window fades (layered alpha) and slides on its own track; one timer drives them all.

enum class Then { Stay, Hide, FadeOut };
struct Track {
    HWND hwnd;
    int x;
    int fromAlpha, toAlpha, fromY, toY;
    double start, duration;
    Then then;
    double hold, fadeOut;  // for Then::FadeOut: the pause before fading out, and the fade's length
    bool active;
    int alpha;  // as last applied
};
enum TrackId { kBoxTrack, kFirstRowTrack, kHintTrack = kFirstRowTrack + kRecentCount + 1, kTrackCount };
Track g_tracks[kTrackCount];
bool g_animate = true;  // Windows' "Animation effects" setting

double NowMs() {
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return count.QuadPart * 1000.0 / frequency.QuadPart;
}

void ApplyTrack(Track& t, double now) {
    double p = t.duration > 0 ? (now - t.start) / t.duration : 1;
    p = p < 0 ? 0 : p > 1 ? 1 : p;
    double eased = 1 - (1 - p) * (1 - p) * (1 - p);  // ease-out cubic: quick start, gentle landing
    int alpha = t.fromAlpha + static_cast<int>((t.toAlpha - t.fromAlpha) * eased);
    if (alpha != t.alpha) {
        SetLayeredWindowAttributes(t.hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
        t.alpha = alpha;
    }
    if (t.fromY != t.toY)
        SetWindowPos(t.hwnd, nullptr, t.x, t.fromY + static_cast<int>((t.toY - t.fromY) * eased), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (p < 1)
        return;
    if (t.then == Then::FadeOut) {
        t.start += t.duration + t.hold;
        t.duration = t.fadeOut;
        t.fromAlpha = t.alpha;
        t.toAlpha = 0;
        t.fromY = t.toY;
        t.then = Then::Hide;
        return;
    }
    if (t.then == Then::Hide)
        ShowWindow(t.hwnd, SW_HIDE);
    t.active = false;
}

// Starts moving a window from one opacity and height to another. The first frame is applied at once.
void Animate(int id, HWND hwnd, int x, int fromAlpha, int toAlpha, int fromY, int toY, double duration,
             double delay = 0, Then then = Then::Stay, double hold = 0, double fadeOut = 0) {
    if (!g_animate)
        duration = delay = fadeOut = 0;
    Track& t = g_tracks[id];
    t = {hwnd, x, fromAlpha, toAlpha, fromY, toY, NowMs() + delay, duration, then, hold, fadeOut, true, -1};
    ApplyTrack(t, NowMs());
    SetTimer(g_messageWnd, kAnimationTimerId, USER_TIMER_MINIMUM, nullptr);
}

bool TracksRunning(int first, int last) {
    for (int id = first; id <= last; ++id)
        if (g_tracks[id].active)
            return true;
    return false;
}

void FinishClosing();

void OnAnimationFrame() {
    double now = NowMs();
    for (Track& t : g_tracks)
        if (t.active)
            ApplyTrack(t, now);
    if (g_popupState == PopupState::Closing && !TracksRunning(kBoxTrack, kHintTrack - 1))
        FinishClosing();
    if (!TracksRunning(0, kTrackCount - 1))
        KillTimer(g_messageWnd, kAnimationTimerId);
}

// ---- Popup ----

int RowY(int slot) {  // slot -1 is the box itself
    return g_layout.y - (slot + 1) * (g_layout.rowHeight + g_layout.gap);
}

void SetClickThrough(HWND hwnd, bool clickThrough) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, clickThrough ? style | WS_EX_TRANSPARENT : style & ~WS_EX_TRANSPARENT);
}

void FinishClosing() {
    for (int id = kBoxTrack; id < kHintTrack; ++id)
        g_tracks[id].active = false;
    for (HWND row : g_recentWnd)
        ShowWindow(row, SW_HIDE);
    ShowWindow(g_popup, SW_HIDE);
    SetClickThrough(g_popup, false);
    SetBoxText(L"");
    ResetHistory();
    g_popupState = PopupState::Hidden;
}

// Hands the focus straight back, then fades out. After a save, the new note first slides up
// into the stack of recent notes, pushing the oldest one out.
void HidePopup(bool saved) {
    if (g_popupState != PopupState::Shown)
        return;
    g_popupState = PopupState::Closing;
    if (g_mode != Mode::Note)
        StopShortcutRecording();
    if (saved)
        SetBoxText(L"");
    SetClickThrough(g_popup, true);
    if (GetForegroundWindow() == g_popup && IsWindow(g_previousWindow))
        SetForegroundWindow(g_previousWindow);

    const Layout& l = g_layout;
    if (!saved) {
        Animate(kBoxTrack, g_popup, l.x, g_tracks[kBoxTrack].alpha, 0, l.y, l.y, kCloseTime, 0, Then::Hide);
        for (int i = 0; i < kRecentCount; ++i)
            if (IsWindowVisible(g_recentWnd[i]))
                Animate(kFirstRowTrack + i, g_recentWnd[i], l.x, g_tracks[kFirstRowTrack + i].alpha, 0, RowY(i),
                        RowY(i), kCloseTime, 0, Then::Hide);
        return;
    }

    Animate(kBoxTrack, g_popup, l.x, g_tracks[kBoxTrack].alpha, 0, l.y, l.y, kSaveBoxFadeTime, 0, Then::Hide);
    for (int i = 0; i < kRecentCount; ++i) {
        if (!IsWindowVisible(g_recentWnd[i]))
            continue;
        bool staysInStack = i + 1 < kRecentCount && RowY(i + 1) >= l.top;
        Animate(kFirstRowTrack + i, g_recentWnd[i], l.x, g_tracks[kFirstRowTrack + i].alpha,
                staysInStack ? kRecentAlpha[i + 1] : 0, RowY(i), RowY(i + 1), kSaveShiftTime, 0,
                staysInStack ? Then::FadeOut : Then::Hide, kSaveHoldTime, kCloseTime);
    }
    if (RowY(0) >= l.top) {
        HWND row = g_recentWnd[kSavedRow];
        InvalidateRect(row, nullptr, TRUE);
        Animate(kFirstRowTrack + kSavedRow, row, l.x, 0, kRecentAlpha[0], RowY(-1), RowY(0), kSaveShiftTime, 0,
                Then::FadeOut, kSaveHoldTime, kCloseTime);
        SetWindowPos(row, HWND_TOPMOST, l.x, RowY(-1), l.width, l.rowHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// ---- First-run hint ----

void HideHint() {
    g_tracks[kHintTrack].active = false;
    ShowWindow(g_hint, SW_HIDE);
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
        SYSTEMTIME t;
        GetLocalTime(&t);
        RecentNote& note = g_recent[kSavedRow];
        wcscpy_s(note.text, start);
        swprintf_s(note.when, L"%02u:%02u", t.wHour, t.wMinute);
        HidePopup(true);
        return;
    }
    HidePopup();
}

// Keyboard handling for the note box that the standard edit control lacks.
// Returns true if the message was handled and shouldn't reach the control.
bool HandleBoxMessage(const MSG& msg) {
    bool keyDown = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
    bool keyUp = msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP;
    if (g_mode != Mode::Note) {
        if (keyDown || keyUp)
            HandleRecordingKey(static_cast<UINT>(msg.wParam), keyDown, keyDown && (msg.lParam & (1 << 30)));
        return keyDown || keyUp || msg.message == WM_CHAR || msg.message == WM_SYSCHAR;
    }
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
    case 'K': if (ctrl) { StartShortcutRecording(); return true; } break;
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

// The work area and DPI of the monitor the cursor is on. Also sizes the font for that DPI.
UINT CursorMonitor(RECT* work) {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof info};
    GetMonitorInfoW(monitor, &info);
    *work = info.rcWork;
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
    return dpi;
}

// Reads Windows' "Animation effects" setting (Settings > Accessibility > Visual effects).
void ReadAnimationSetting() {
    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    g_animate = animate != FALSE;
}

// Centers the box horizontally, a quarter of the way down the monitor the cursor is on.
// It fades in while settling into place, then the recent notes follow one after another.
void ShowPopup() {
    if (g_popupState == PopupState::Closing)
        FinishClosing();
    HideHint();
    ReadAnimationSetting();
    RECT work;
    UINT dpi = CursorMonitor(&work);

    HDC dc = GetDC(g_edit);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    TEXTMETRICW metrics;
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, oldFont);
    ReleaseDC(g_edit, dc);

    int width = MulDiv(kPopupWidth, dpi, 96), height = MulDiv(kPopupHeight, dpi, 96);
    int padding = MulDiv(kPadding, dpi, 96), lineHeight = metrics.tmHeight + 2;
    int drift = MulDiv(kDrift, dpi, 96);
    Layout& l = g_layout;
    l = {work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top) / 4, width,
         MulDiv(kRecentHeight, dpi, 96), MulDiv(kRecentGap, dpi, 96), static_cast<int>(work.top)};

    // Starts transparent: the window is shown at once (so typing goes straight in) and fades in.
    Animate(kBoxTrack, g_popup, l.x, 0, 255, l.y - drift, l.y, kOpenTime);
    SetWindowPos(g_popup, HWND_TOPMOST, l.x, l.y - drift, width, height, SWP_NOACTIVATE);
    SetWindowPos(g_edit, nullptr, padding, (height - lineHeight) / 2, width - 2 * padding, lineHeight, SWP_NOZORDER);

    // The last notes stack upwards from the box, fading out the older they are.
    LoadRecentNotes();
    for (int i = 0; i < kRecentCount; ++i) {
        if (i >= g_recentCount || RowY(i) < l.top)
            continue;
        HWND row = g_recentWnd[i];
        InvalidateRect(row, nullptr, TRUE);
        Animate(kFirstRowTrack + i, row, l.x, 0, kRecentAlpha[i], RowY(i) - drift, RowY(i), kOpenTime,
                kRowDelay + i * kRowStagger);
        SetWindowPos(row, HWND_TOPMOST, l.x, RowY(i) - drift, width, l.rowHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    g_previousWindow = GetForegroundWindow();
    g_popupState = PopupState::Shown;
    ShowWindow(g_popup, SW_SHOW);
    ForceForeground(g_popup);
    SetFocus(g_edit);
}

// First run only: "QuickNote is running · press Ctrl+Space" fades in where the box appears,
// then fades out after a few seconds (or as soon as the box opens).
void ShowHint() {
    ReadAnimationSetting();
    RECT work;
    UINT dpi = CursorMonitor(&work);
    wchar_t hotkey[64];
    FormatHotkey(g_settings.noteHotkey, hotkey, ARRAYSIZE(hotkey));
    swprintf_s(g_hintText, L"QuickNote is running · press %ls", hotkey);

    int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (g_hintIcon)
        DestroyIcon(g_hintIcon);
    LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kIconId), iconSize, iconSize, &g_hintIcon);
    HDC dc = GetDC(g_hint);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    SIZE text;
    GetTextExtentPoint32W(dc, g_hintText, static_cast<int>(wcslen(g_hintText)), &text);
    SelectObject(dc, oldFont);
    ReleaseDC(g_hint, dc);

    int padding = MulDiv(kPadding, dpi, 96), drift = MulDiv(kDrift, dpi, 96);
    int width = padding + iconSize + padding * 2 / 3 + text.cx + padding, height = MulDiv(kHintHeight, dpi, 96);
    int x = work.left + (work.right - work.left - width) / 2;
    int y = work.top + (work.bottom - work.top) / 4 + (MulDiv(kPopupHeight, dpi, 96) - height) / 2;
    Animate(kHintTrack, g_hint, x, 0, kHintAlpha, y - drift, y, kHintFadeInTime, 0, Then::FadeOut, kHintHoldTime,
            kHintFadeOutTime);
    SetWindowPos(g_hint, HWND_TOPMOST, x, y - drift, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK HintProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);
        int padding = MulDiv(kPadding, g_fontDpi, 96);
        int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, g_fontDpi);
        DrawIconEx(dc, padding, (rect.bottom - iconSize) / 2, g_hintIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
        rect.left = padding + iconSize + padding * 2 / 3;
        HGDIOBJ oldFont = SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kTextColor);
        DrawTextW(dc, g_hintText, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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
    case WM_CTLCOLORSTATIC:  // the box is read-only while recording shortcuts
        SetTextColor(reinterpret_cast<HDC>(wParam), kTextColor);
        SetBkColor(reinterpret_cast<HDC>(wParam), kBackColor);
        return reinterpret_cast<LRESULT>(g_backBrush);
    case WM_TIMER:
        if (wParam == kSavedTimerId)
            HidePopup();
        return 0;
    case WM_CLOSE:  // Alt+F4 only hides; the window is reused for the app's lifetime
        HidePopup();
        return 0;
    case WM_DPICHANGED:  // ShowPopup already sizes for the target monitor
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// One recent note above the box: the text on the left, its time or date on the right.
LRESULT CALLBACK RecentProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        int i = static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (i < g_recentCount || i == kSavedRow) {
            const RecentNote& note = g_recent[i];
            RECT rect;
            GetClientRect(hwnd, &rect);
            int padding = MulDiv(kPadding, g_fontDpi, 96);
            InflateRect(&rect, -padding, 0);
            HGDIOBJ oldFont = SelectObject(dc, g_font);
            SetBkMode(dc, TRANSPARENT);
            const UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
            RECT when = rect;
            DrawTextW(dc, note.when, -1, &when, flags | DT_CALCRECT);
            int whenWidth = when.right - when.left;
            when = rect;
            SetTextColor(dc, kMutedColor);
            DrawTextW(dc, note.when, -1, &when, flags | DT_RIGHT);
            rect.right -= whenWidth ? whenWidth + padding : 0;
            SetTextColor(dc, kTextColor);
            DrawTextW(dc, note.text, -1, &rect, flags | DT_END_ELLIPSIS);
            SelectObject(dc, oldFont);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Hidden window that receives the hotkey and tray icon messages.
LRESULT CALLBACK MessageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HOTKEY:
        if (wParam == kFolderHotkeyId)
            OpenNotesFolder();
        else if (g_popupState == PopupState::Shown)
            HidePopup();
        else
            ShowPopup();
        return 0;
    case WM_TIMER:
        if (wParam == kAnimationTimerId)
            OnAnimationFrame();
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
    // Layered so it can fade in and out.
    g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, popupClass.lpszClassName, kAppName,
                              WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(g_popup, 0, 0, LWA_ALPHA);
    DwmSetWindowAttribute(g_popup, kDwmCornerPreference, &kDwmCornerRound, sizeof kDwmCornerRound);
    DwmSetWindowAttribute(g_popup, kDwmBorderColor, &kBorderColor, sizeof kBorderColor);

    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             0, 0, 0, 0, g_popup, nullptr, instance, nullptr);
    SendMessageW(g_edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(kPlaceholder));
    SendMessageW(g_edit, EM_SETLIMITTEXT, kMaxNoteLength, 0);

    WNDCLASSEXW recentClass = {sizeof recentClass};
    recentClass.lpfnWndProc = RecentProc;
    recentClass.hInstance = instance;
    recentClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    recentClass.hbrBackground = g_backBrush;
    recentClass.lpszClassName = L"QuickNoteRecent";
    RegisterClassExW(&recentClass);

    // Layered for the fade, click-through, and never activated, so the box keeps the focus.
    const DWORD overlayStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    for (int i = 0; i <= kSavedRow; ++i) {
        HWND row = CreateWindowExW(overlayStyle, recentClass.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, g_popup,
                                   nullptr, instance, nullptr);
        SetWindowLongPtrW(row, GWLP_USERDATA, i);
        SetLayeredWindowAttributes(row, 0, 0, LWA_ALPHA);
        DwmSetWindowAttribute(row, kDwmCornerPreference, &kDwmCornerRound, sizeof kDwmCornerRound);
        DwmSetWindowAttribute(row, kDwmBorderColor, &kBorderColor, sizeof kBorderColor);
        g_recentWnd[i] = row;
    }

    WNDCLASSEXW hintClass = recentClass;
    hintClass.lpfnWndProc = HintProc;
    hintClass.lpszClassName = L"QuickNoteHint";
    RegisterClassExW(&hintClass);
    g_hint = CreateWindowExW(overlayStyle, hintClass.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                             instance, nullptr);
    SetLayeredWindowAttributes(g_hint, 0, 0, LWA_ALPHA);
    DwmSetWindowAttribute(g_hint, kDwmCornerPreference, &kDwmCornerRound, sizeof kDwmCornerRound);
    DwmSetWindowAttribute(g_hint, kDwmBorderColor, &kBorderColor, sizeof kBorderColor);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Only one copy may run, otherwise the second one can't register the hotkeys.
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

    if (RegisterHotkeys() && g_firstRun)
        ShowHint();

    ResetHistory();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == g_edit && HandleBoxMessage(msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        g_typing = false;
    }

    UnregisterHotkeys();
    NOTIFYICONDATAW nid = TrayData();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
