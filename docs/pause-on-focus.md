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
| `src/lib/server/Server.h`, `.cpp` | Polls the focused application, blocks switching and delayed switches while paused. |
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
| `.github/workflows/keepalive.yml`, `.github/keepalive.md` | Weekly timestamp commit so GitHub does not disable the scheduled workflow. |
| `src/unittests/server/FocusPauseTests.*`, `CMakeLists.txt` | Unit tests for the matching and resume logic. |
| `docs/pause-on-focus.md` | This document. |
