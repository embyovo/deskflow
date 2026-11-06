# Pause switching while an application has focus

This change lets the server pause automatic screen switching and input
forwarding while the user works in selected applications, for example a game
that keeps grabbing the mouse or a remote desktop client that uses the screen
edges itself.

The pause starts as soon as one of the listed applications gets the keyboard
focus on the server computer and ends as soon as the focus goes somewhere else.
No restart is needed.

## Configuration

The pause list lives in the application settings file (`deskflow.conf`, in the
`[server]` section) and can be edited from the GUI as described below. Editing
the file by hand works while Deskflow is not running:

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

## Editing the list in the GUI

The server settings dialog has a group box called **Pause switching on focus**
on the **Advanced** tab (`src/lib/gui/dialogs/ServerConfigDialog.ui`).

* The combo box lists the executable names of all processes that are currently
  running. It is editable, so the list doubles as a search field: type a few
  letters to filter and pick the entry, or type an executable name that is not
  running yet (`chrome.exe`) and press **Add**.
* **Add** appends the name to the list below the combo box. Choosing an entry
  from the drop down adds it straight away.
* **Remove** deletes the selected entries (multi selection is enabled),
  **Clear** empties the whole list.
* **Only when the window title contains** is the optional title filter and
  accepts a comma separated list, for example `YouTube, Visual Studio`.
* All entries are stored in `server/pauseOnFocusApps` and
  `server/pauseOnFocusTitles` when the dialog is accepted.

The dialog restarts the core process when it is accepted (the same
`m_coreProcess.restart()` path the other server settings use), so the new list
is used right away and no manual restart is needed.

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
* Only **full screen** applications pause switching, borderless full screen
  included. A windowed or maximized application keeps switching normal, so a
  windowed app can never trap the cursor on the primary screen.
* Focus changes are watched with `SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
  WINEVENT_OUTOFCONTEXT)`, installed on the event queue thread next to the
  server. The hook updates a cached boolean, which the mouse path reads once per
  event, so a paused application costs a single boolean read per mouse event.
  When the cursor reaches the edge of the primary screen the application is
  checked again **once per visit**: that keeps switching correct even if the hook
  misses an event (session switch, protected processes) or cannot be installed
  at all, and a cached `true` that is out of date is corrected before the switch
  instead of blocking the first attempt.
* The pause ends as soon as the foreground changes away from the listed
  application (the hook fires immediately, no polling interval), and no later
  than the next edge check.

## Overhead

With a pause list configured, a paused application costs one relaxed atomic
boolean read per mouse event and no Win32 call. A replica of the same call
sequence (Python/ctypes, so an upper bound) measures 0.04–0.05 µs per event,
0.005 % of one core at 1000 Hz and 0.01 % at 2000 Hz, with no handle or private
byte growth over 600 000 and 6 000 000 events. The foreground hook itself is
free while idle: 0.000 s CPU in 20 s and no handle or memory change. An
alternative that called `GetForegroundWindow` on every mouse event measured
45 µs per event, about 4.9 % of one core at 1000 Hz and 9.8 % at 2000 Hz, so it
is not used.

If `SetWinEventHook` cannot be installed the log warns and switching keeps
working: every check falls back to the edge decision, which has the same cost
as the edge-only design and only runs when the cursor reaches an edge.

## The Windows switch path and where a hitch can come from

Moving the cursor onto an edge runs the sequence below. Every step except the
pause check is upstream code; the switch itself was not touched by this change.

1. `mouseLLHook` (`src/lib/platform/MSWindowsHook.cpp:654`) is the
   `WH_MOUSE_LL` callback and runs on Deskflow's hook thread. While the pointer
   is on the primary the hook is in `kHOOK_WATCH_JUMP_ZONE`
   (`src/lib/platform/MSWindowsComputer.cpp:250`), so `mouseHookHandler`
   (`MSWindowsHook.cpp:543`) posts `DESKFLOW_MSG_MOUSE_MOVE` and **eats** the
   events inside the jump zone (`:641-645`). The cursor therefore stays pinned
   at the edge while the server decides. That is by design, not a stall; the
   callback only posts messages and does no Win32 query.
2. `Server::onMouseMovePrimary` (`src/lib/server/Server.cpp:1709`) runs on the
   event queue thread: clamp, `mapToNeighbor`, `isSwitchOkay` (`:856`).
   `startSwitchWait` (`:1028`) and `startSwitchTwoTap` (`:954`) only arm a timer
   when `switchWaitDelay` / `switchTwoTapDelay` are non zero; both default to
   `0.0` (`Server.h:402`, `:406`), so the default path switches immediately.
3. `Server::switchComputer` (`:459`) calls `m_active->leave()` (`:525`):
   * `Computer::leave` (`src/lib/deskflow/Computer.cpp:160`) first runs
     `leavePrimary()` → `updateKeyState()` (`:451-457`), which is a
     `PostThreadMessage` plus `waitForDesk` round trip of its own.
   * `MSWindowsComputer::leave` (`src/lib/platform/MSWindowsComputer.cpp:286`)
     calls `m_desks->leave()` (`:297`) and then warps the cursor to the centre
     of the primary (`:309`).
   * `MSWindowsDesks::leave` (`MSWindowsDesks.cpp:184`) is `PostThreadMessage`
     plus `waitForDesk()` (`:359`, `:932`): the server thread blocks until the
     desk thread has handled the message.
   * `deskLeave` (`:589`) hides the cursor, raises the hider window, and with
     low level hooks it **hands the foreground to that hidden window**:
     `getForegroundWindow()` (`:640`), `EnableWindow`, `SetActiveWindow`,
     `AttachThreadInput` with the foreground thread, `SetForegroundWindow`
     (`:659`), `AttachThreadInput(..., FALSE)` (`:661`).
   * still inside `switchComputer`, when clipboard sharing is enabled and the
     primary owns the clipboard, `onClipboardChanged` (`Server.cpp:534-541`)
     reads and marshals every clipboard format
     (`MSWindowsComputer::getClipboard`, `:439`) before the switch happens.
4. `m_active->enter()` (`Server.cpp:563`). For a client this is only
   `ProtocolUtil::writef(kMsgCEnter)` (`ClientProxy1_0.cpp:226`); `leave()` is
   the same (`:232`), so the server never waits for an answer.
5. Coming back to the primary: `PrimaryClient::enter` (`PrimaryClient.cpp:102`)
   warps the cursor, then `deskEnter` (`MSWindowsDesks.cpp:550`) restores the
   foreground window with `AttachThreadInput` + `SetForegroundWindow`
   (`:577-581`).

Per call costs, measured with a ctypes probe on one 1920x1080 display, 2000
samples (median / p95):

| Call | median | p95 |
| --- | --- | --- |
| `GetForegroundWindow` | 1.4 µs | 6.5 µs |
| `GetWindowThreadProcessId` | 0.8 µs | 1.0 µs |
| `GetWindowLongPtrW(GWL_STYLE)` | 0.7 µs | 0.8 µs |
| `GetWindowRect` | 0.8 µs | 1.1 µs |
| `MonitorFromWindow` + `GetMonitorInfoW` | 1.6 µs | 1.7 µs |
| `AttachThreadInput` attach + detach | 4.5 µs | 12.2 µs |
| `ShowCursor(FALSE)` + `ShowCursor(TRUE)` | 3.4 µs | 3.8 µs |
| `GetCursorPos` + `SetCursorPos` | 5.9 µs | 21.0 µs |
| `SetWindowPos` (hidden window) | 14.3 µs | 35.7 µs |
| `PostThreadMessage` + `GetMessage` | 6.4 µs | 26.5 µs |
| `OpenClipboard` + `CloseClipboard` | 3.1 µs | 4.6 µs |
| clipboard text read (44 byte payload) | 8.6 µs | 21.4 µs |
| `GetClipboardSequenceNumber` | 1.2 µs | 1.4 µs |

The whole local part of a switch adds up to well under 0.1 ms. The pause check
itself measured 42.6-46.3 µs with and without a full screen application running
in the background, so it cannot explain a visible hitch. Three steps on this
path are not bounded by our code:

* the foreground hand-over in `deskLeave` / `deskEnter`. `SetForegroundWindow`
  and `AttachThreadInput` involve the foreground application's thread and the
  window manager; when a full screen window exists the compositor may also
  have to leave its direct flip / overlay path. The calls are microseconds when
  the system is idle (measured above) and can take much longer when the
  foreground thread is busy. This is the only step whose cost depends on what
  else is on screen.
* `ARCH->sleep(0.03)` in `MSWindowsDesks.cpp:683`, a hard 30 ms delay on every
  leave from a **secondary** computer, added upstream to stop the cursor
  flicker at the centre of the client.
* the clipboard copy in step 3, whose cost follows the clipboard contents.

Deskflow never calls `ChangeDisplaySettings`, `SetDisplayConfig` or
`ClipCursor` (no occurrence in `src/`), so it never changes a display mode and
never clips the cursor itself. An exclusive full screen application does that
on its own; Deskflow does not activate it, so its mode is not renegotiated by
the switch. A background application that clipped the cursor and never
released it can still confine the pointer, and that is visible in
`GetClipCursor`.

Also note that the primary side of the switch uses `SetCursorPos`, not
`SendInput`: `SendInput` is only used on the secondary (`deskMouseMove`,
`deskMouseRelativeMove`) and for fake keys, so DPI or resolution conversion in
`SendInput` cannot slow down the edge on the primary side.

### What this change adds

Two reads, and nothing else:

* one relaxed atomic read per mouse event (`Server.cpp:1744`);
* one `isPauseActive()` per visit to the jump zone (`:1799-1801`), which is
  `GetForegroundWindow` + `GetWindowThreadProcessId` + `GetWindowTextW` +
  `QueryFullProcessImageNameW` + the full screen test, measured at 42.6-46.3 µs.

`git diff v1.27.0 HEAD` touches `Server.cpp` (+101), `Server.h` (+21),
`FocusPause.*` (+187), `Computer.cpp` (+5, one pass through) and
`MSWindowsComputer.cpp` (+90, `getActiveApp` / `isFullscreenWindow`, read only).
`MSWindowsDesks.cpp` and the switch functions are not in the diff, so no
blocking call, wait, timer or extra round trip was added to the leave or enter
path.

### Measuring a hitch on your own machine

Run the server with verbose logging, push the cursor against the edge once with
the full screen application in the background and once with it closed, and
compare the timestamps of these three lines:

```
switch from "..." to "..." at ...,...
leaving computer
entering computer
```

The gap from `leaving computer` to `entering computer` is the switch itself. If
the visible hitch is inside that gap, the foreground hand-over or the clipboard
is the suspect; if the gap is a few microseconds and the hitch is still
visible, it happens outside the switch and is the window manager reacting to
the foreground change.

Two more checks that need no code:

* `GetClipCursor` while the full screen application is in the background. A
  rectangle that is not the whole virtual screen means the application still
  clips the cursor and Deskflow cannot move it to another computer.
* `server/win32KeepForeground=true` in `deskflow.conf`. It makes
  `getForegroundWindow()` return `nullptr` (`MSWindowsDesks.cpp:989-992`), so
  `deskLeave` skips the foreground hand-over completely. The trade-off, noted
  upstream, is that the keyboard layout and shifted characters can be wrong on
  a client.

### Options, and what is verified

* `win32KeepForeground` (above): removes the only step that depends on what is
  on screen. Verified by reading the code path, not measured here.
* Cache the clipboard by `GetClipboardSequenceNumber()` (1.2 µs) and skip the
  full copy when the sequence number did not change. Verified numbers, small
  change, no effect on small clipboards, real saving for large ones.
* Defer the foreground hand-over until the first key has to be translated:
  suggested, touches the desk thread and keyboard handling, medium risk.
* Remove or make configurable the 30 ms `sleep` at `MSWindowsDesks.cpp:683`:
  the cost is verified to be exactly 30 ms, but it is there to stop a visible
  cursor flicker, so it should not be removed without a replacement.
* System side, not verified here: disable full screen optimizations for the
  background application, run it borderless, or use one refresh rate on both
  displays.

What could not be measured here: this machine has a single display and no full
screen application, so the difference between exclusive and borderless full
screen, and any compositor or display mode transition, could not be reproduced;
the statements about those are bounded by the code facts above, not by numbers.

### A one second stall, two cursors and a frozen pointer

Reported symptoms: the edge stalls for about a second, a cursor is visible on
both computers at the same time, the control side can show two cursors, and the
pointer does not move during that second. It only happens with a full screen
application in the background.

What the measurements rule out:

* **the foreground lock.** `HKCU\Control Panel\Desktop\ForegroundLockTimeout`
  is `200000` ms on this machine and `SPI_GETFOREGROUNDLOCKTIMEOUT` reports `0`
  for a process that is allowed to take the foreground, so there is no one
  second lock to wait for. `deskLeave` and `deskEnter` also call
  `AttachThreadInput` before `SetForegroundWindow`, which is the documented way
  around the lock. Changing `SPI_SETFOREGROUNDLOCKTIMEOUT` would not help.
* **a hard coded delay in our code.** The only constant delay on the path is the
  30 ms `ARCH->sleep(0.03)` in `MSWindowsDesks.cpp:683`, and only when leaving a
  secondary computer.
* **the pause check.** 42.6-46.3 µs, measured with and without a full screen
  application.

What the code says about the symptoms:

* **two cursors.** `deskLeave` hides the cursor at its first step
  (`MSWindowsDesks.cpp:595`) and `deskEnter` shows it again at its first step
  (`:561`), before restoring the foreground at `:579`. The client only hides
  its own cursor when it handles its `leave` message, which the server sends
  after the primary has finished `leave()`. Any block between those points
  keeps both cursors on screen, so the duration of the overlap is exactly the
  duration of the stall. A second cursor drawn by the full screen application
  itself is a software cursor: `ShowCursor` cannot hide it and Deskflow has no
  handle on it.
* **a frozen pointer.** While leaving, the hook stays in
  `kHOOK_WATCH_JUMP_ZONE` and is only switched to `kHOOK_RELAY_EVENTS` at the
  end of `MSWindowsComputer::leave` (`:322`), after `waitForDesk()` returned.
  Every move in the mean time is eaten by the hook and posted to an event
  thread that is blocked in `waitForDesk()`, so nothing consumes it. The counter
  `MSWindowsHook::eatenMoveCount()` makes this visible in the trace.
* **the one second.** `waitForDesk()` has no timeout
  (`MSWindowsDesks.cpp:932-941`), so the event thread waits for exactly as long
  as the desk thread needs. The desk thread's only unbounded steps are
  `AttachThreadInput` and `SetForegroundWindow` against the application that
  owned the foreground (`:657-661` and `:577-581`). When that application is
  busy resetting a full screen device it stops pumping messages, and
  `AttachThreadInput` waits for its input queue, which turns a system side mode
  switch into a Deskflow side block. The trace below separates that from a
  block on the client or in the clipboard.

### Switch trace

The instrumented lines are `LOG_DEBUG`, so they cost nothing at the default log
level and appear with `--log-level debug` (or the Debug level in the GUI). Every
line already carries a millisecond timestamp from the logger. Filter the log for
`SWITCHTRACE`.

```
SWITCHTRACE post msg=... thread=...
SWITCHTRACE desk thread handled msg=... thread=...
SWITCHTRACE processed msg=... thread=... relayed=... eaten=...
SWITCHTRACE deskLeave begin thread=... primary=...
SWITCHTRACE deskLeave cursor hidden thread=... visible=0|1
SWITCHTRACE deskLeave fg=... pid=... class="..." hider=... thread=...
SWITCHTRACE deskLeave attached thread=...
SWITCHTRACE deskLeave foreground taken thread=...
SWITCHTRACE deskLeave done thread=...
SWITCHTRACE leave desk returned thread=... relayed=... eaten=... last-move-ago=...ms
SWITCHTRACE leave warp done thread=...
SWITCHTRACE leave hook=relay thread=...
SWITCHTRACE leave done thread=...
SWITCHTRACE switchComputer begin active="..." dst="..."
SWITCHTRACE switchComputer leave returned
SWITCHTRACE switchComputer clipboard done
SWITCHTRACE primary enter begin / primary warp done / primary enter done
SWITCHTRACE enter begin / enter desk returned / enter done
SWITCHTRACE deskEnter begin / cursor visible / restore fg=... / attached / foreground restored / done
SWITCHTRACE switchComputer enter returned / done
```

How to read them:

* `deskLeave fg=...` → `deskLeave foreground taken`: time inside the foreground
  hand over. The `pid` and `class` tell which application owned the foreground:
  a class such as `UnityWndClass`, `UnrealWindow`, `SDL_app` or
  `Chrome_WidgetWin_1` identifies the full screen application.
* `processed msg=...` → `leave done`: the rest of the leave path.
* `leave done` → `switchComputer clipboard done`: the clipboard copy and the
  routing, still on the event thread.
* `deskEnter cursor visible` → `deskEnter foreground restored`: the restore
  hand over, which is the step that runs while both cursors can be visible.
* `relayed` / `eaten` growth and `last-move-ago`: whether the hook was eating
  moves during the block and how long ago the last one arrived.
* `visible=1` right after `cursor hidden` means `ShowCursor` did not take
  effect, which would explain a duplicate system cursor.

### Fixes, from lowest to highest risk

1. `server/win32KeepForeground=true`. `getForegroundWindow()` returns `nullptr`
   (`MSWindowsDesks.cpp:989-992`), so `deskLeave` never calls
   `AttachThreadInput` / `SetForegroundWindow`, and `deskEnter` has nothing to
   restore. This removes the only unbounded step from the path and needs no
   code change. The trade-off, documented upstream, is that the keyboard layout
   and shifted characters can be wrong on a client.
2. Do not hand the foreground over until it is needed. It exists only so the
   disabled foreground window cannot turn injected keys into unshifted
   characters, so it could run on the first key event instead of on every
   switch. Suggested, touches the desk thread and keyboard translation, medium
   risk.
3. Add a watchdog around the hand over: do the `AttachThreadInput` +
   `SetForegroundWindow` work on a short lived thread and let the switch
   continue after a few milliseconds. Suggested, medium risk: `deskEnter` has to
   restore the same window later, so the two sides must not race.
4. For the duplicate cursor, check `visible=` in the trace first. If the system
   cursor really is still visible after `cursor hidden`, repeat the
   `ShowCursor(FALSE)` loop until `GetCursorInfo` reports it hidden (a few extra
   iterations, no other change). If the second cursor belongs to an application
   that draws its own, only that application can stop drawing it.
5. Do not make the hook pass moves through during a blocked switch: the cursor
   would slide past the edge while the server has not decided yet, and the
   pointer would have to be warped back anyway. The correct fix is to shorten
   the block (1 and 2), not to un-eat the events.

## Full screen detection

The foreground window is full screen when all of these hold
(`isFullscreenWindow()` in `src/lib/platform/MSWindowsComputer.cpp`):

* `GetWindowRect` is within **8 pixels** of the monitor rectangle returned by
  `GetMonitorInfo` for `MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)`. The
  tolerance covers DPI scaling and the invisible resize border, which leave a
  few pixels uncovered even for a real full screen window.
* The window does **not** have both `WS_CAPTION` and `WS_THICKFRAME`. A normal
  maximized window keeps its caption and frame (and stops above the task bar,
  because the maximized area is the monitor work area), so it is treated as
  windowed. Borderless full screen games use `WS_POPUP` and pass.
* The window is not the desktop (`Progman`, `WorkerW`), not the task bar
  (`Shell_TrayWnd`, `Shell_SecondaryTrayWnd`) and not a Deskflow window
  (same process id as the server).

A maximized window that is only partly covered, for example because an
always-on-top app bar takes space, is smaller than `rcMonitor` and therefore
windowed.

## Platform support

Only Windows is implemented. The foreground window is read with
`GetForegroundWindow` + `GetWindowThreadProcessId`, the process image name with
`QueryFullProcessImageNameW`, the window title with `GetWindowTextW` and the
full screen state with `GetWindowRect` + `MonitorFromWindow` + `GetMonitorInfo`,
all in `src/lib/platform/MSWindowsComputer.cpp`.

Other platforms inherit the default `IPlatformComputer::getActiveApp()` which
returns an empty result, so nothing is ever paused there. Adding macOS or X11
support only requires overriding that one method.

Windows limitation: applications that run elevated (as administrator) can not be
queried by a non elevated Deskflow process; their process name is empty and they
never match the pause list. Run Deskflow elevated as well if you need that.

## How to verify

1. Start the server on the primary computer and a client on a second computer,
   with a layout where moving the mouse to the right edge switches to the client.
2. Edit `deskflow.conf`, set `pauseOnFocusApps=chrome.exe`, start Deskflow.
   The log shows `pausing switching while these applications have focus: chrome.exe`.
3. Open a full screen application whose process is listed, for example a game,
   or put a browser into full screen with F11. Push the mouse against the right
   edge: the cursor stays on the primary screen and the log shows `focused
   application paused switching`.
4. Click on the desktop (or any other application). Without restarting anything,
   push the mouse against the right edge again: the cursor moves to the client
   computer. A windowed or maximized window of the same application does **not**
   pause, only a window that covers its whole monitor does.
5. Optional: set `pauseOnFocusTitles=YouTube`, open Chrome with a YouTube tab
   and confirm that Chrome only pauses on that tab, not on other tabs.

The matching rules are covered by `src/unittests/server/FocusPauseTests.cpp`.

## Building on Windows

The official build needs Qt 6.7+ (MSVC kit), CMake 3.24+, Visual Studio 2022
with the C++ workload and OpenSSL 3 (vcpkg recommended, or `OPENSSL_ROOT_DIR`):

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH=C:\Qt\6.9.0\msvc2022_64 ^
  -DOPENSSL_ROOT_DIR=C:\vcpkg\installed\x64-windows
cmake --build build --config Release --target deskflow-core
```

Run the tests with `ctest --test-dir build -R FocusPauseTests`.

This change was compiled on Windows with Qt 6.9.0 (MinGW 13.1 kit, CMake 4.4):
the unit test executable builds and passes all cases, and `Server.cpp`,
`Settings.cpp`, `Computer.cpp` and `MSWindowsComputer.cpp` compile cleanly.  A
full application build could not be finished in this environment because no
MSVC toolchain and no OpenSSL 3 development libraries were available (CMake
configuration stops at `find_package(OpenSSL 3 REQUIRED)`).

## Syncing with upstream

`.github/scripts/sync-upstream.ps1` merges the latest commits of the official
Deskflow repository into the current branch. It never pushes and never resolves
conflicts on its own. The repository ignores its top level `scripts` folder, so
the script lives under `.github/scripts`:

```powershell
# from the repository root, on feature/pause-on-focus
.\.github\scripts\sync-upstream.ps1

# explicit branch, or rebase instead of merge
.\.github\scripts\sync-upstream.ps1 -Branch feature/pause-on-focus -Rebase
```

What it does:

1. Checks that it runs inside a work tree and refuses to start if there are
   uncommitted changes.
2. Adds the `upstream` remote (`https://github.com/deskflow/deskflow.git`) if it
   is missing, and fails with a hint if it points somewhere else.
3. Detects the upstream default branch (`git remote set-head upstream --auto`,
   `master` as fallback) and fetches with `--prune`.
4. Switches to the feature branch and exits cleanly if it is already up to date.
5. Merges `upstream/<default>` into the branch. A **merge** is used by default
   and not a rebase, because this branch is published in a fork: a rebase
   rewrites commits that are already pushed and would need a force-push, which
   this workflow must avoid. `-Rebase` is available for a private/local branch.
6. On conflicts it stops, lists the conflicted files and prints the commands to
   continue (`git add` + `git merge --continue`) or to give up
   (`git merge --abort`). It exits with code 1 and pushes nothing.

The script is safe to run repeatedly: without new upstream commits it just
reports that there is nothing to do.

## Automatic upstream sync and release

`.github/workflows/upstream-sync.yml` does the same merge unattended and only
publishes when the result is verified.

* Trigger: every day at 03:00 UTC, plus `workflow_dispatch` with an optional
  `upstream_tag` input.
* It reads the latest **release** tag of `deskflow/deskflow` with the GitHub API
  (nightly and pre-release tags are ignored, and the API is used because GitHub
  has no ordering on tags).
* If that tag is already an ancestor of `master` (`git merge-base --is-ancestor`)
  the run stops. No state file is needed, so runs are idempotent.
* Otherwise it merges with `--no-ff`. History is never rewritten and nothing is
  ever force pushed. On a conflict the merge is aborted, `master` stays where it
  is, the job fails and an issue is opened automatically.
* It then builds and runs the unit tests on the merged tree with the same steps
  as `windows-build.yml` (`install-dependencies`, `run-tests`).
* **Only after a green build and test run** does it push `master`, create the
  release tag and start `release-windows.yml`. A failed build or test therefore
  never produces a tag or a release.
* `GITHUB_TOKEN` pushes do not start other workflows, so the release is started
  explicitly with `gh workflow run release-windows.yml --ref <tag>`
  (`workflow_dispatch` is exempt from that restriction).

### Version policy

| Source | Tag | Package version |
| --- | --- | --- |
| Upstream release `v1.27.0`, first sync | `v1.27.0-deskflow.1` | `1.27.0-deskflow.1` |
| Same upstream tag again (manual re-run) | `v1.27.0-deskflow.2` | `1.27.0-deskflow.2` |
| Manual fork release | `v1.1.2-focus-test` | `1.1.2-focus-test` |

The `-deskflow.N` suffix keeps the fork tag distinct from every upstream tag, so
the two remotes can be fetched into the same repository without a collision, and
the upstream version number stays visible in the package name. The suffix
counter is chosen by probing the existing refs, so a repeated sync of the same
upstream tag never overwrites an older release.

## Repository keepalive

GitHub automatically disables **schedule**-triggered workflows after 60 days
without repository activity. Runs of a scheduled workflow do not count as
activity, only commits and pushes do. `.github/workflows/keepalive.yml` runs
every Monday at 04:17 UTC and rewrites `.github/keepalive.md` with a timestamp,
then commits it back to the default branch:

* the commit is the activity GitHub looks for, so the schedule keeps running;
* the push is done with the built in `GITHUB_TOKEN`, and the commit message ends
  with `[skip ci]`, so neither this nor any other workflow is started by it;
* the job takes well under a minute of runner time per week and touches no build
  output, so it cannot interfere with `windows-build.yml` or
  `release-windows.yml`.

If the repository is idle for more than 60 days anyway, re-enable the workflow
from the Actions page: **Actions → Repository keepalive → Enable workflow**, or
run `gh workflow enable keepalive.yml`. The same applies to any workflow GitHub
disabled, which is always listed at the top of the Actions page.

## Changed files

| File | Change |
| --- | --- |
| `src/lib/server/FocusPause.h`, `.cpp` | New, pause list matching and settings rules. |
| `src/lib/server/Server.h`, `.cpp` | Caches the pause state from a foreground hook, reads it once per mouse event, blocks switching and delayed switches while paused. |
| `src/lib/deskflow/ActiveAppInfo.h` | New, describes the focused application. |
| `src/lib/deskflow/IPlatformComputer.h` | New `getActiveApp()` platform hook with a safe default. |
| `src/lib/deskflow/Computer.h`, `.cpp` | Pass through `getActiveApp()`. |
| `src/lib/platform/MSWindowsComputer.h`, `.cpp` | Windows implementation of `getActiveApp()`. |
| `src/lib/common/Settings.h`, `.cpp` | New `server/pauseOnFocusApps` and `server/pauseOnFocusTitles` settings. |
| `src/lib/gui/dialogs/ServerConfigDialog.ui`, `.h`, `.cpp` | New "Pause switching on focus" group box: pick a running executable, type one, add/remove/clear, optional title filter. |
| `src/lib/server/CMakeLists.txt` | Builds the new source file. |
| `.github/workflows/windows-build.yml` | Windows build job (MSVC 2022, Qt, OpenSSL 3 from vcpkg) with installer artifacts. |
| `.github/workflows/release-windows.yml` | Tag driven Windows build that publishes a GitHub release. |
| `.github/scripts/sync-upstream.ps1` | Merges upstream into the current branch and stops on conflicts. |
| `.github/workflows/upstream-sync.yml` | Daily merge of the latest upstream release, build + tests, then tag and release. |
| `.github/workflows/keepalive.yml`, `.github/keepalive.md` | Weekly timestamp commit so GitHub does not disable the scheduled workflow. |
| `src/unittests/server/FocusPauseTests.*`, `CMakeLists.txt` | Unit tests for the matching and resume logic. |
| `docs/pause-on-focus.md` | This document. |
