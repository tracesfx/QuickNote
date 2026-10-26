# QuickNote

Press **Ctrl+Space** anywhere, type a note, press **Enter**. The note is added to a plain-text file for the day, and the box disappears. QuickNote is a single small exe for Windows 10/11 with nothing to install.

- **Esc**, or clicking away, cancels.
- Notes go to `Documents\QuickNotes\YYYY-MM-DD.txt` by default, one line per note: `[14:32:07] check the glow on shot 12`
- In the box, **Ctrl+Backspace / Ctrl+Delete** delete a word at a time. **Ctrl+Z** undoes (a word at a time), and **Ctrl+Y** or **Ctrl+Shift+Z** redoes. Ctrl+A, Ctrl+C, Ctrl+X and Ctrl+V work as usual.
- The app lives in the system tray. Right-click its icon to open or change the notes folder, toggle **Start with Windows**, or exit. Double-click the icon to open the notes folder.

## Settings
`%AppData%\QuickNote\settings.ini`:

```ini
[QuickNote]
NotesFolder=C:\Users\you\Documents\QuickNotes
Hotkey=Ctrl+Space
StartWithWindows=0
```

`Hotkey` accepts combinations like `Ctrl+Alt+N` or `Win+F9`. Restart QuickNote after changing it.

## Build
Requires Visual Studio 2022 or Build Tools with "Desktop development with C++".

```
build.bat   -> build\QuickNote.exe
```

The build links the C runtime from `ucrtbase.dll`, which is part of Windows 10/11, so no Visual C++ Redistributable is needed.

## Smart App Control
QuickNote is unsigned. Smart App Control on Windows 11 judges each build by its content, not its build timestamp, so rebuilding the same source gets the same verdict. Features are added one or two at a time, and each commit message records whether Smart App Control allowed that build. To go back to a version that runs, check out a commit marked `SAC: allowed` and rebuild.
