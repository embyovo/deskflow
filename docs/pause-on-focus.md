# Pause switching while an application has focus

This change lets the server pause automatic screen switching and input
forwarding while the user works in selected applications, for example a game
that keeps grabbing the mouse or a remote desktop client that uses the screen
edges itself.

The pause starts as soon as one of the listed applications gets the keyboard
focus on the server computer and ends as soon as the focus goes somewhere else.
No restart is needed.

## Configuration

The pause list lives in the application settings file and can be edited with any
text editor while Deskflow is not running (or from the GUI, if a GUI field for it
is added later). The file is `deskflow.conf` next to the other Deskflow
settings, in the `[server]` section:

```ini
[server]
pauseOnFocusApps=chrome.exe,notepad.exe
pauseOnFocusTitles=YouTube
```

Keys:

| Key | Meaning |
| --- | --- |
| `pauseOnFocusApps` | Comma separated executable file names, e.g. `chrome.exe`. Matched case insensitively. A full path is accepted, only the file name is used. |
| `pauseOnFocusTitles` | Optional, comma separated window title fragments. When set, the focused window title must also contain one of them, which allows pausing only for a specific document or tab. |

The pause is off while `pauseOnFocusApps` is empty. If both keys contain
entries, an application must match the process name **and** one of the title
fragments.

Windows process names are the executable file names as shown in Task Manager,
for example `chrome.exe`, `firefox.exe`, `notepad.exe` or `game.exe`. The
well known system names are `explorer.exe` for the desktop and `ApplicationFrameHost.exe`
for some Windows apps.

## Behaviour

* While paused, moving the mouse into the edge of the primary screen does not
  switch computers. The cursor simply stays at the edge, so the pointer never
  jumps back and forth between screens.
* Keyboard and mouse events are not forwarded to secondary computers. While the
  pause is active the mouse is on the primary screen anyway, so the local
  application keeps receiving its input as usual.
* Clipboard sharing is **not** paused. Clipboard transfers are independent of
  the pointer, so copying inside the paused application and pasting on a client
  still works. File drag and drop does need a screen switch to start, so a drag
  cannot be initiated while paused; a drag that was already running is not
  interrupted.
* The pause only applies while the primary computer is the active computer. If
  the pointer is already on a secondary computer, input keeps working; otherwise
  the user could get stuck there.
* Explicit switch hotkeys and "switch to computer" actions still work, so the
  user always has a way out of the paused application.
* Focus changes are polled every 200 ms, which is the worst case delay before
  switching is paused or restored.

## Platform support

Only Windows is implemented. The foreground window is read with
`GetForegroundWindow` + `GetWindowThreadProcessId`, the process image name with
`QueryFullProcessImageNameW` and the window title with `GetWindowTextW`, all in
`src/lib/platform/MSWindowsComputer.cpp`.

Other platforms inherit the default `IPlatformComputer::getActiveApp()` which
returns an empty result, so nothing is ever paused there. Adding macOS or X11
support only requires overriding that one method.

Windows limitation: applications that run elevated (as administrator) can not be
queried by a non elevated Deskflow process; their process name is empty and they
never match the pause list. Run Deskflow elevated as well if you need that.

## How to verify

1. Start the server on the primary computer and a client on a second computer,
   with a layout where moving the mouse to the right edge switches to the client.
2. Edit `deskflow.conf`, set `pauseOnFocusApps=notepad.exe`, start Deskflow.
   The log shows `pausing switching while these applications have focus: notepad.exe`.
3. Open Notepad on the primary computer and give it the focus. Push the mouse
   against the right edge: the cursor stays on the primary screen and the log
   shows `paused switching, "Untitled - Notepad" (notepad.exe) has focus`.
4. Click on the desktop (or any other application). Without restarting anything,
   push the mouse against the right edge again: the cursor moves to the client
   computer and the log shows `resumed switching`.
5. Optional: set `pauseOnFocusTitles=YouTube`, open Chrome with a YouTube tab
   and confirm that Chrome only pauses on that tab, not on other tabs.

The matching rules are covered by `src/unittests/server/FocusPauseTests.cpp`.

## Changed files

| File | Change |
| --- | --- |
| `src/lib/server/FocusPause.h`, `.cpp` | New, pause list matching and settings rules. |
| `src/lib/server/Server.h`, `.cpp` | Polls the focused application, blocks switching and delayed switches while paused. |
| `src/lib/deskflow/IPlatformComputer.h` | New `ActiveAppInfo` and `getActiveApp()` platform hook with a safe default. |
| `src/lib/deskflow/Computer.h`, `.cpp` | Pass through `getActiveApp()`. |
| `src/lib/platform/MSWindowsComputer.h`, `.cpp` | Windows implementation of `getActiveApp()`. |
| `src/lib/common/Settings.h`, `.cpp` | New `server/pauseOnFocusApps` and `server/pauseOnFocusTitles` settings. |
| `src/lib/server/CMakeLists.txt` | Builds the new source file. |
| `src/unittests/server/FocusPauseTests.*`, `CMakeLists.txt` | Unit tests for the matching and resume logic. |
| `docs/pause-on-focus.md` | This document. |
