/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FocusPauseTests.h"

#include "server/FocusPause.h"

using namespace deskflow::server;

namespace {
//! Most tests describe a full screen application, the only kind that pauses.
ActiveAppInfo activeApp(std::string process, std::string title, bool fullscreen = true)
{
  return {std::move(process), std::move(title), fullscreen};
}
} // namespace

void FocusPauseTests::emptyListNeverPauses()
{
  FocusPause pause;
  QVERIFY(pause.isEmpty());
  QVERIFY(!pause.shouldPause(activeApp("chrome.exe", "Google Chrome")));
}

void FocusPauseTests::matchesProcessName()
{
  FocusPause pause({"chrome.exe"}, {});
  QVERIFY(!pause.isEmpty());

  // process names are compared without case
  QVERIFY(pause.shouldPause(activeApp("Chrome.exe", "Google Chrome")));
  QVERIFY(!pause.shouldPause(activeApp("notepad.exe", "Untitled - Notepad")));
}

void FocusPauseTests::processNameFromPath()
{
  QCOMPARE(FocusPause::processNameFromPath("C:\\Program Files\\Google\\Chrome\\chrome.exe"), "chrome.exe");
  QCOMPARE(FocusPause::processNameFromPath("/usr/bin/firefox"), "firefox");
  QCOMPARE(FocusPause::processNameFromPath("CHROME.EXE"), "chrome.exe");
}

void FocusPauseTests::ignoresBlankRules()
{
  FocusPause pause({"", "  ", "notepad.exe"}, {"", "  "});
  QVERIFY(!pause.isEmpty());
  QVERIFY(!pause.shouldPause(activeApp("chrome.exe", "Google Chrome")));
  QVERIFY(pause.shouldPause(activeApp("notepad.exe", "Untitled - Notepad")));
}

void FocusPauseTests::titleNarrowsMatch()
{
  FocusPause pause({"chrome.exe"}, {"youtube"});
  QVERIFY(pause.shouldPause(activeApp("chrome.exe", "YouTube - Google Chrome")));
  QVERIFY(!pause.shouldPause(activeApp("chrome.exe", "Deskflow - Google Chrome")));

  // any configured title fragment is enough
  FocusPause several({"chrome.exe"}, {"youtube", "netflix"});
  QVERIFY(several.shouldPause(activeApp("chrome.exe", "Netflix - Google Chrome")));
}

void FocusPauseTests::titleWithoutProcessNeverPauses()
{
  FocusPause pause({}, {"youtube"});
  QVERIFY(pause.isEmpty());
  QVERIFY(!pause.shouldPause(activeApp("chrome.exe", "YouTube - Google Chrome")));
}

void FocusPauseTests::unknownActiveAppNeverPauses()
{
  // e.g. the desktop has focus or the platform does not report the app
  FocusPause pause({"chrome.exe"}, {});
  QVERIFY(!pause.shouldPause({}));
  QVERIFY(!pause.shouldPause({"", "", true}));
}

void FocusPauseTests::windowedAppNeverPauses()
{
  FocusPause pause({"chrome.exe"}, {});
  QVERIFY(!pause.shouldPause(activeApp("chrome.exe", "Google Chrome", false)));

  // a title match does not change that
  FocusPause titled({"chrome.exe"}, {"youtube"});
  QVERIFY(!titled.shouldPause(activeApp("chrome.exe", "YouTube - Google Chrome", false)));
}

void FocusPauseTests::staysPausedWhenSwitchingBetweenListedApps()
{
  FocusPause pause({"chrome.exe", "notepad.exe"}, {});
  QVERIFY(pause.shouldPause(activeApp("chrome.exe", "Google Chrome")));
  QVERIFY(pause.shouldPause(activeApp("notepad.exe", "Untitled - Notepad")));
}

void FocusPauseTests::resumesAfterFocusLeaves()
{
  FocusPause pause({"notepad.exe"}, {});
  QVERIFY(pause.shouldPause(activeApp("notepad.exe", "Untitled - Notepad")));

  // focus returns to the desktop (explorer) or any other app: pause is over
  QVERIFY(!pause.shouldPause(activeApp("explorer.exe", "Program Manager")));
  QVERIFY(!pause.shouldPause(activeApp("chrome.exe", "Google Chrome")));

  // and comes back as soon as the listed app is focused again
  QVERIFY(pause.shouldPause(activeApp("notepad.exe", "Untitled - Notepad")));
}

QTEST_MAIN(FocusPauseTests)
