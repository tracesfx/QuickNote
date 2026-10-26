# QuickNote

Press **Ctrl+Space** anywhere, type a note, press **Enter**. The note is added to a plain-text file for the day, and the box disappears. QuickNote is a single small exe for Windows 10/11 with nothing to install.

- Your last three notes appear above the box, fading out the older they are, with their time (or date, for earlier days).
- The box fades in and settles into place; when you save, your note slides up into the stack of recent notes. The effects are brief, never delay typing, and are skipped when Windows' *Animation effects* setting is off. The very first time QuickNote runs, a hint shows which shortcut to press.
- **Esc**, or clicking away, cancels.
- **Ctrl+Shift+Space** anywhere opens the notes folder.
- Notes go to `Documents\QuickNotes\YYYY-MM-DD.txt` by default, one line per note: `[14:32:07] check the glow on shot 12`
- In the box, **Ctrl+Backspace / Ctrl+Delete** delete a word at a time. **Ctrl+Z** undoes (a word at a time), and **Ctrl+Y** or **Ctrl+Shift+Z** redoes. Ctrl+A, Ctrl+C, Ctrl+X and Ctrl+V work as usual.
- The app lives in the system tray. Right-click its icon to open or change the notes folder, change shortcuts, toggle **Start with Windows**, or exit. Double-click the icon to open the notes folder.

## Changing shortcuts
Press **Ctrl+K** in the note box, or choose **Change shortcuts…** from the tray menu. Press the new-note shortcut, then the open-folder shortcut. Press Enter at either step to keep the current one, or Esc to cancel. QuickNote rejects combinations that another app already uses, that would override everyday shortcuts (Ctrl+C, Alt+F4, …), or that have no Ctrl, Alt or Win key.

## Settings
Settings are saved in `QuickNote.ini` next to `QuickNote.exe`, so the app can be copied or shared as one folder. Hand out just the exe and the INI is created on first run. If the exe's folder isn't writable (e.g. Program Files), QuickNote uses `%AppData%\QuickNote\QuickNote.ini` instead.

```ini
[QuickNote]
NotesFolder=C:\Users\you\Documents\QuickNotes
Hotkey=Ctrl+Space
FolderHotkey=Ctrl+Shift+Space
StartWithWindows=0
```

Shortcuts are easiest to change with Ctrl+K. If you edit the file by hand, restart QuickNote afterwards.

## Build
Requires Visual Studio 2022 or Build Tools with "Desktop development with C++".

```
build.bat   -> build\QuickNote.exe
```

The build links the C runtime from `ucrtbase.dll`, which is part of Windows 10/11, so no Visual C++ Redistributable is needed.

## Smart App Control
QuickNote is unsigned, so on PCs with Smart App Control turned on, Windows may block it. The verdict isn't predictable. Each commit message records whether the build was allowed when it was tested on the development PC, but a fresh build of the same source can still get a different verdict: one source version was allowed on its first build and blocked on its second. Once a particular exe has been allowed on a PC, that file, and copies of it, keep running there. Other PCs with Smart App Control on decide for themselves. PCs where it is off or in evaluation mode run QuickNote normally.
