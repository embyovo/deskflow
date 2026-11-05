/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfigDialog.h"
#include "ui_ServerConfigDialog.h"

#include "common/Constants.h"
#include "common/NetworkProtocol.h"
#include "common/PlatformInfo.h"
#include "common/Settings.h"
#include "dialogs/ActionDialog.h"
#include "dialogs/ComputerSettingsDialog.h"
#include "dialogs/HotkeyDialog.h"
#include "dialogs/PauseAppChoices.h"
#include "gui/widgets/SettingsDialogButtonBox.h"

#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QSet>
#include <QStandardItemModel>

#include <algorithm>

#ifdef Q_OS_WIN
// clang-format off
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dwmapi.h>
#include <TlHelp32.h>
#include <winver.h>
// clang-format on
#include <cwchar>
#include <vector>
#endif

using enum ComputerConfig::SwitchCorner;

namespace {
struct RunningProcess
{
  QString m_name;
  QString m_description;
  //! True when the process owns a visible top level window
  bool m_windowed = false;
};

//! Everything the pause application list needs, plus the numbers used by its diagnostic.
struct ProcessEnumeration
{
  QList<RunningProcess> m_processes;
#ifdef Q_OS_WIN
  QSet<QString> m_windowed;
  int m_windows = 0;
  int m_hiddenWindows = 0;
  int m_shellWindows = 0;
  int m_ownWindows = 0;
  int m_cloakedWindows = 0;
  int m_helperWindows = 0;
  int m_unnamedWindows = 0;
  int m_captionlessWindows = 0;
#endif
};

#ifdef Q_OS_WIN

//! Full path of a process, or an empty string when it cannot be queried (protected or gone).
QString processImagePath(DWORD processId)
{
  QString path;
  if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId); process != nullptr) {
    wchar_t buffer[1024];
    DWORD length = 1024;
    if (QueryFullProcessImageNameW(process, 0, buffer, &length) && length > 0)
      path = QString::fromWCharArray(buffer, static_cast<int>(length));
    CloseHandle(process);
  }
  return path;
}

QString executableNameFromPath(const QString &path)
{
  const int separator = qMax(path.lastIndexOf(QLatin1Char('\\')), path.lastIndexOf(QLatin1Char('/')));
  return path.mid(separator + 1);
}

//! Process names by id from one tool help snapshot; independent of OpenProcess rights.
QHash<DWORD, QString> processNamesById()
{
  QHash<DWORD, QString> names;
  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return names;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      const auto name = QString::fromWCharArray(entry.szExeFile);
      if (!name.isEmpty())
        names.insert(entry.th32ProcessID, name);
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return names;
}

QString windowClassName(HWND window)
{
  wchar_t className[64] = {};
  if (GetClassNameW(window, className, static_cast<int>(std::size(className))) <= 0)
    return {};
  return QString::fromWCharArray(className);
}

//! The desktop, the task bar and their helper windows are not applications.
bool isShellWindow(HWND window)
{
  const auto className = windowClassName(window);
  return className.compare(QLatin1String("Progman"), Qt::CaseInsensitive) == 0 ||
         className.compare(QLatin1String("WorkerW"), Qt::CaseInsensitive) == 0 ||
         className.compare(QLatin1String("Shell_TrayWnd"), Qt::CaseInsensitive) == 0 ||
         className.compare(QLatin1String("Shell_SecondaryTrayWnd"), Qt::CaseInsensitive) == 0;
}

//! Suspended UWP windows and windows of another virtual desktop stay visible but are cloaked.
bool isCloakedWindow(HWND window)
{
  constexpr DWORD kDwmwaCloaked = 14;
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(window, kDwmwaCloaked, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

struct WindowScan
{
  QHash<DWORD, QString> m_namesById;
  QSet<QString> m_windowed;
  int m_windows = 0;
  int m_hidden = 0;
  int m_shell = 0;
  int m_own = 0;
  int m_cloaked = 0;
  int m_helper = 0;
  int m_unnamed = 0;
  int m_captionless = 0;
};

BOOL CALLBACK collectWindowedProcess(HWND window, LPARAM param)
{
  auto *scan = reinterpret_cast<WindowScan *>(param);
  scan->m_windows++;
  if (!IsWindowVisible(window)) {
    scan->m_hidden++;
    return TRUE;
  }

  DWORD processId = 0;
  if (GetWindowThreadProcessId(window, &processId) == 0 || processId == 0) {
    scan->m_unnamed++;
    return TRUE;
  }
  if (processId == GetCurrentProcessId()) {
    scan->m_own++;
    return TRUE;
  }
  if (isShellWindow(window)) {
    scan->m_shell++;
    return TRUE;
  }
  if (isCloakedWindow(window)) {
    scan->m_cloaked++;
    return TRUE;
  }

  // Only tool windows without a caption are helper windows. A visible window whose caption cannot
  // be read (elevated or a fragile cross process window) still counts as a window of the application.
  if (GetWindowTextLengthW(window) == 0) {
    scan->m_captionless++;
    if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
      scan->m_helper++;
      return TRUE;
    }
  }

  // The tool help name also covers processes that OpenProcess is not allowed to query.
  auto name = scan->m_namesById.value(processId);
  if (name.isEmpty()) {
    name = executableNameFromPath(processImagePath(processId));
    if (name.isEmpty()) {
      scan->m_unnamed++;
      return TRUE;
    }
  }
  scan->m_windowed.insert(name.toLower());
  return TRUE;
}

//! Executables that own a visible top level window, the task manager's applications group.
WindowScan scanWindows()
{
  WindowScan scan;
  scan.m_namesById = processNamesById();
  EnumWindows(collectWindowedProcess, reinterpret_cast<LPARAM>(&scan));
  return scan;
}

//! The FileDescription shown by Task Manager, for example "Google Chrome".
QString fileDescription(const QString &path)
{
  if (path.isEmpty())
    return {};

  DWORD ignored = 0;
  const auto size = GetFileVersionInfoSizeW(reinterpret_cast<LPCWSTR>(path.utf16()), &ignored);
  if (size == 0)
    return {};

  std::vector<BYTE> versionInfo(size);
  if (!GetFileVersionInfoW(reinterpret_cast<LPCWSTR>(path.utf16()), 0, size, versionInfo.data()))
    return {};

  struct Translation
  {
    WORD m_language;
    WORD m_codePage;
  };
  LPVOID translations = nullptr;
  UINT translationLength = 0;
  if (!VerQueryValueW(versionInfo.data(), L"\\VarFileInfo\\Translation", &translations, &translationLength) ||
      translationLength < sizeof(Translation))
    return {};

  const auto *translation = static_cast<const Translation *>(translations);
  wchar_t key[64];
  std::swprintf(
      key, std::size(key), L"\\StringFileInfo\\%04x%04x\\FileDescription", translation->m_language,
      translation->m_codePage
  );
  wchar_t *text = nullptr;
  UINT textLength = 0;
  if (!VerQueryValueW(versionInfo.data(), key, reinterpret_cast<void **>(&text), &textLength) || textLength == 0)
    return {};

  return QString::fromWCharArray(text).trimmed();
}

ProcessEnumeration runningProcesses()
{
  ProcessEnumeration result;
  const auto scan = scanWindows();

  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return result;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      RunningProcess process;
      process.m_name = QString::fromWCharArray(entry.szExeFile);
      if (process.m_name.isEmpty())
        continue;
      process.m_description = fileDescription(processImagePath(entry.th32ProcessID));
      process.m_windowed = scan.m_windowed.contains(process.m_name.toLower());
      result.m_processes.append(process);
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);

  result.m_windowed = scan.m_windowed;
  result.m_windows = scan.m_windows;
  result.m_hiddenWindows = scan.m_hidden;
  result.m_shellWindows = scan.m_shell;
  result.m_ownWindows = scan.m_own;
  result.m_cloakedWindows = scan.m_cloaked;
  result.m_helperWindows = scan.m_helper;
  result.m_unnamedWindows = scan.m_unnamed;
  result.m_captionlessWindows = scan.m_captionless;
  return result;
}

//! The executable that owns the foreground window.
/*!
 * The GUI runs in its own process and cannot reach the computer object of the
 * server, so the platform hook used by the server is not available here. Keep
 * this Windows only; other platforms simply offer the manual input.
 */
QString foregroundProcessName()
{
  const HWND window = GetForegroundWindow();
  // A task bar menu is not the application the user works in, fall back to the z order instead.
  if (window == nullptr || isShellWindow(window))
    return {};

  DWORD processId = 0;
  if (GetWindowThreadProcessId(window, &processId) == 0 || processId == 0)
    return {};

  const auto name = processNamesById().value(processId);
  return name.isEmpty() ? executableNameFromPath(processImagePath(processId)) : name;
}

//! The application the user was working in before our own dialog took the foreground.
/*!
 * GetForegroundWindow returns our own settings dialog while it is open, so walking the z order is
 * the only way to find the application the user came from. Topmost and no activate windows are
 * overlays or helpers, never that application.
 */
QString recentForegroundProcessName()
{
  const auto namesById = processNamesById();
  HWND window = GetTopWindow(nullptr);
  for (int guard = 0; window != nullptr && guard < 1024; ++guard) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    const auto exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (processId != 0 && processId != GetCurrentProcessId() && IsWindowVisible(window) &&
        GetWindowTextLengthW(window) > 0 && (exStyle & (WS_EX_TOPMOST | WS_EX_NOACTIVATE)) == 0 &&
        !isShellWindow(window) && !isCloakedWindow(window)) {
      if (const auto name = namesById.value(processId); !name.isEmpty())
        return name;
    }
    window = GetWindow(window, GW_HWNDNEXT);
  }
  return {};
}
#else
ProcessEnumeration runningProcesses()
{
  return {};
}

QString foregroundProcessName()
{
  return {};
}

QString recentForegroundProcessName()
{
  return {};
}
#endif
} // namespace

ServerConfigDialog::ServerConfigDialog(QWidget *parent, ServerConfig &config)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
      ui{std::make_unique<Ui::ServerConfigDialog>()},
      m_columns{Settings::value(Settings::Server::GridWidth).toInt()},
      m_rows{Settings::value(Settings::Server::GridHeight).toInt()},
      m_originalServerConfig(config),
      m_originalServerConfigIsExternal(config.useExternalConfig()),
      m_originalServerConfigUsesExternalFile(config.configFile()),
      m_serverConfig(config),
      m_computerSetupModel(m_serverConfig.computers(), m_columns, m_rows),
      m_buttonBox{new SettingsDialogButtonBox(this)}
{
  ui->setupUi(this);
  ui->tabWidget->setCurrentIndex(0);
  layout()->addWidget(m_buttonBox);

  loadFromConfig();

  ui->lblRemoveComputer->setPixmap(QIcon::fromTheme("user-trash").pixmap(QSize(64, 64)));
  ui->lblNewComputer->setEnabled(!model().isFull());
  ui->lblNewComputer->setPixmap(QIcon::fromTheme("video-display").pixmap(QSize(64, 64)));
  ui->btnBrowseConfigFile->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::DocumentOpen));

  if (!deskflow::platform::isWindows())
    ui->cbWin32KeepForeground->setVisible(false);
  ui->cbPauseApp->setEditable(true);
  ui->cbPauseApp->setInsertPolicy(QComboBox::NoInsert);
  ui->cbPauseApp->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  populatePauseAppChoices();
  initConnections();
}

ServerConfigDialog::~ServerConfigDialog() = default;

bool ServerConfigDialog::addClient(const QString &clientName)
{
  return addComputer(clientName, true);
}

void ServerConfigDialog::save()
{
  if (ui->groupExternalConfig->isChecked() && !QFile::exists(ui->lineConfigFile->text())) {

    auto selectedButton = QMessageBox::warning(
        this, "Filename invalid", "Please select a valid configuration file.", QMessageBox::Ok | QMessageBox::Ignore
    );

    if (selectedButton != QMessageBox::Ok || !browseConfigFile()) {
      return;
    }
  }
  // now that the dialog has been accepted, copy the new server config to the
  // original one, which is a reference to the one in MainWindow.
  setOriginalServerConfig(serverConfig());
  if (Settings::value(Settings::Server::EnableClipboard).toBool() != m_enableClipboard) {
    Settings::setValue(Settings::Server::XdpRestoreToken, QString());
    Settings::setValue(Settings::Server::XdpClipboardRetried, false);
  }
  Settings::setValue(Settings::Server::Protocol, networkProtocolToOption(m_protocol));
  Settings::setValue(Settings::Server::EnableClipboard, m_enableClipboard);
  Settings::setValue(Settings::Server::ClipboardSize, m_clipboardSize);
  Settings::setValue(Settings::Server::EnableHeartbeat, m_enableHeartbeat);
  Settings::setValue(Settings::Server::Heartbeat, m_heartbeatRate);
  Settings::setValue(Settings::Server::EnableSwitchDelay, m_enableSwitchDelay);
  Settings::setValue(Settings::Server::SwitchDelay, m_switchDelay);
  Settings::setValue(Settings::Server::DefaultLockToComputerState, m_defaultLockToComputerState);
  Settings::setValue(Settings::Server::DisableLockToComputer, m_disableLockToComputer);
  Settings::setValue(Settings::Server::EnableSwitchDoubleTap, m_enableSwitchDoubleTap);
  Settings::setValue(Settings::Server::SwitchDoubleTap, m_switchDoubleTap);
  Settings::setValue(Settings::Server::RelativeMouseMoves, m_relativeMouseMoves);
  Settings::setValue(Settings::Server::Win32KeepForeground, m_win32keepForeground);
  Settings::setValue(Settings::Server::ExternalConfig, ui->groupExternalConfig->isChecked());
  Settings::setValue(Settings::Server::ExternalConfigFile, ui->lineConfigFile->text());
  Settings::setValue(Settings::Server::PauseOnFocusApps, m_pauseApps);
  Settings::setValue(Settings::Server::PauseOnFocusTitles, pauseTitles());

  QStringList computerNames;
  const auto computerList = m_computerSetupModel.m_computers;
  for (const auto &computer : computerList) {
    const auto &computerName = computer.name();
    if (computerName.isEmpty())
      continue;
    computerNames.append(QStringLiteral("computer_%1").arg(computerName));
    Settings::setValue(Settings::Computer::Aliases.arg(computerName), computer.aliases());
    Settings::setValue(Settings::Computer::Name.arg(computerName), computerName);
  }
  // Write the computer list now too, or it keeps computers whose settings were just removed until the app quits.
  m_serverConfig.commit();
  Settings::removeUnknownComputers(computerNames);
  QDialog::accept();
}

void ServerConfigDialog::cancel()
{
  serverConfig().setUseExternalConfig(m_originalServerConfigIsExternal);
  serverConfig().setConfigFile(m_originalServerConfigUsesExternalFile);
  QDialog::reject();
}

void ServerConfigDialog::addHotkey()
{
  Hotkey hotkey;
  HotkeyDialog dlg(this, hotkey);
  if (dlg.exec() == QDialog::Accepted) {
    serverConfig().hotkeys().append(hotkey);
    ui->listHotkeys->addItem(hotkey.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::editHotkey()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to editing out of bounds hotkey row: " << row;
    return;
  }

  Hotkey &hotkey = serverConfig().hotkeys()[row];
  HotkeyDialog dlg(this, hotkey);
  if (dlg.exec() == QDialog::Accepted) {
    ui->listHotkeys->currentItem()->setText(hotkey.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::removeHotkey()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to remove out of bounds hotkey row: " << row;
    return;
  }

  serverConfig().hotkeys().removeAt(row);
  ui->listActions->clear();
  delete ui->listHotkeys->item(row);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::listHotkeysSelectionChanged(const QItemSelection &selected, const QItemSelection &)
{
  bool itemsSelected = !selected.isEmpty();
  ui->btnEditHotkey->setEnabled(itemsSelected);
  ui->btnRemoveHotkey->setEnabled(itemsSelected);
  ui->btnNewAction->setEnabled(itemsSelected);

  if (itemsSelected && !serverConfig().hotkeys().isEmpty()) {
    ui->listActions->clear();
    const Hotkey &hotkey = serverConfig().hotkeys().at(selected.indexes().first().row());
    for (const Action &action : hotkey.actions())
      ui->listActions->addItem(action.text());
  }
}

void ServerConfigDialog::addAction()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to add action to out of bounds hotkey row: " << row;
    return;
  }

  Hotkey &hotkey = serverConfig().hotkeys()[row];
  Action action;
  ActionDialog dlg(this, serverConfig(), hotkey, action);
  if (dlg.exec() == QDialog::Accepted) {
    hotkey.addAction(action);
    ui->listActions->addItem(action.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::editAction()
{
  int hotkeyRow = ui->listHotkeys->currentRow();
  if (hotkeyRow < 0 || hotkeyRow >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to edit action from out of bounds hotkey row: " << hotkeyRow;
    return;
  }
  Hotkey &hotkey = serverConfig().hotkeys()[hotkeyRow];

  int actionRow = ui->listActions->currentRow();
  if (actionRow < 0 || actionRow >= hotkey.actions().size()) {
    qDebug() << "Attempt to remove out of bounds action row: " << actionRow;
    return;
  }
  Action &action = hotkey.actionAt(actionRow);

  ActionDialog dlg(this, serverConfig(), hotkey, action);
  if (dlg.exec() == QDialog::Accepted) {
    ui->listActions->currentItem()->setText(action.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::removeAction()
{
  int hotkeyRow = ui->listHotkeys->currentRow();
  if (hotkeyRow < 0 || hotkeyRow >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to remove action from out of bounds hotkey row: " << hotkeyRow;
    return;
  }
  Hotkey &hotkey = serverConfig().hotkeys()[hotkeyRow];

  int actionRow = ui->listActions->currentRow();
  if (actionRow < 0 || actionRow >= hotkey.actions().size()) {
    qDebug() << "Attempt to remove out of bounds action row: " << actionRow;
    return;
  }

  hotkey.removeActionAt(actionRow);
  delete ui->listActions->currentItem();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleClipboard(bool enabled)
{
  if (m_enableClipboard == enabled)
    return;

  m_enableClipboard = enabled;

  ui->sbClipboardSizeLimit->setEnabled(enabled);
  if (enabled && !ui->sbClipboardSizeLimit->value()) {
    m_clipboardSize = Settings::defaultValue(Settings::Server::ClipboardSize).toUInt();
    ui->sbClipboardSizeLimit->setValue(m_clipboardSize ? m_clipboardSize : 1);
  }
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setClipboardLimit(int limit)
{
  if (m_clipboardSize == limit)
    return;

  m_clipboardSize = limit;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleHeartbeat(bool enabled)
{
  m_enableHeartbeat = enabled;
  ui->sbHeartbeat->setEnabled(enabled);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setHeartbeat(int rate)
{
  if (rate == m_heartbeatRate)
    return;
  m_heartbeatRate = rate;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleRelativeMouseMoves(bool enabled)
{
  if (m_relativeMouseMoves == enabled)
    return;
  m_relativeMouseMoves = enabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleProtocol()
{
  m_protocol = ui->rbProtocolBarrier->isChecked() ? NetworkProtocol::Barrier : NetworkProtocol::Synergy;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::listActionsSelectionChanged(const QItemSelection &selected, const QItemSelection &)
{
  bool enabled = !selected.isEmpty();
  ui->btnEditAction->setEnabled(enabled);
  ui->btnRemoveAction->setEnabled(enabled);
}

void ServerConfigDialog::toggleSwitchDoubleTap(bool enable)
{
  m_enableSwitchDoubleTap = enable;
  ui->sbSwitchDoubleTap->setEnabled(enable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setSwitchDoubleTap(int within)
{
  if (m_switchDoubleTap == within)
    return;
  m_switchDoubleTap = within;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleSwitchDelay(bool enable)
{
  m_enableSwitchDelay = enable;
  ui->sbSwitchDelay->setEnabled(enable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setSwitchDelay(int delay)
{
  if (m_switchDelay == delay)
    return;
  m_switchDelay = delay;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleDefaultLockToComputerState(bool state)
{
  if (m_defaultLockToComputerState == state)
    return;
  m_defaultLockToComputerState = state;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleLockToComputer(bool disabled)
{
  if (m_disableLockToComputer == disabled)
    return;
  m_disableLockToComputer = disabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleWin32Foreground(bool enabled)
{
  if (m_win32keepForeground == enabled)
    return;
  m_win32keepForeground = enabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::addClient()
{
  addComputer("", false);
}

void ServerConfigDialog::onComputerRemoved()
{
  ui->lblNewComputer->setEnabled(true);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleExternalConfig(bool checked)
{
  ui->widgetExternalConfigControls->setEnabled(checked);
  ui->tabWidget->setTabEnabled(0, !checked);
  ui->tabWidget->setTabEnabled(1, !checked);
  serverConfig().setUseExternalConfig(checked);
  setButtonBoxEnabledButtons();
}

bool ServerConfigDialog::browseConfigFile()
{
  //: %1 is replaced with the application names
  //: (*.conf) and (*.*) should not be translated
  const auto deskflowConfigFilter = tr("%1 Configurations (*.conf);;All files (*.*)");

  QString fileName =
      QFileDialog::getOpenFileName(this, tr("Browse for a config file"), "", deskflowConfigFilter.arg(kAppName));

  if (!fileName.isEmpty()) {
    ui->lineConfigFile->setText(fileName);
    setServerConfig();
    return true;
  }

  return false;
}

void ServerConfigDialog::loadFromConfig()
{
  m_protocol = Settings::networkProtocol();
  m_enableHeartbeat = Settings::value(Settings::Server::EnableHeartbeat).toBool();
  m_heartbeatRate = Settings::value(Settings::Server::Heartbeat).toInt();
  m_relativeMouseMoves = Settings::value(Settings::Server::RelativeMouseMoves).toBool();
  m_win32keepForeground = Settings::value(Settings::Server::Win32KeepForeground).toBool();
  m_enableSwitchDelay = Settings::value(Settings::Server::EnableSwitchDelay).toBool();
  m_switchDelay = Settings::value(Settings::Server::SwitchDelay).toInt();
  m_enableSwitchDoubleTap = Settings::value(Settings::Server::EnableSwitchDoubleTap).toBool();
  m_switchDoubleTap = Settings::value(Settings::Server::SwitchDoubleTap).toInt();
  m_defaultLockToComputerState = Settings::value(Settings::Server::DefaultLockToComputerState).toBool();
  m_disableLockToComputer = Settings::value(Settings::Server::DisableLockToComputer).toBool();
  m_enableClipboard = Settings::value(Settings::Server::EnableClipboard).toBool();
  m_clipboardSize = Settings::value(Settings::Server::ClipboardSize).toUInt();
  m_pauseApps = Settings::value(Settings::Server::PauseOnFocusApps).toStringList();
  ui->linePauseTitles->setText(Settings::value(Settings::Server::PauseOnFocusTitles).toStringList().join(", "));

  ui->lineConfigFile->setText(serverConfig().configFile());
  ui->groupExternalConfig->setChecked(serverConfig().useExternalConfig());

  refreshControls();

  ui->listHotkeys->clear();
  for (const Hotkey &hotkey : std::as_const(serverConfig().hotkeys()))
    ui->listHotkeys->addItem(hotkey.text());

  ui->computerSetupView->setModel(&m_computerSetupModel);

  auto &computers = serverConfig().computers();
  auto server = std::ranges::find_if(computers, [this](const Computer &computer) {
    return (computer.name() == serverConfig().getServerName());
  });

  if (server == computers.end()) {
    Computer serverComputer(serverConfig().getServerName());
    serverComputer.markAsServer();
    model().computer(m_columns / 2, m_rows / 2) = serverComputer;
  } else {
    server->markAsServer();
  }
  updateControls();
}

void ServerConfigDialog::resetFromSettings()
{
  m_serverConfig = m_originalServerConfig;
  m_serverConfig.setConfigFile(m_originalServerConfigUsesExternalFile);
  m_serverConfig.setUseExternalConfig(m_originalServerConfigIsExternal);
  loadFromConfig();
  if (ui->tabWidget->currentWidget() == ui->tabComputers) {
    ui->computerSetupView->reset();
    ui->computerSetupView->update();
  }
}

void ServerConfigDialog::refreshControls()
{
  ui->rbProtocolSynergy->setChecked(m_protocol == NetworkProtocol::Synergy);
  ui->rbProtocolBarrier->setChecked(m_protocol == NetworkProtocol::Barrier);
  ui->cbHeartbeat->setChecked(m_enableHeartbeat);
  ui->sbHeartbeat->setEnabled(ui->cbHeartbeat->isChecked());
  ui->sbHeartbeat->setValue(m_heartbeatRate);
  ui->cbRelativeMouseMoves->setChecked(m_relativeMouseMoves);
  ui->cbWin32KeepForeground->setChecked(m_win32keepForeground);
  ui->cbSwitchDelay->setChecked(m_enableSwitchDelay);
  ui->sbSwitchDelay->setEnabled(ui->cbSwitchDelay->isChecked());
  ui->sbSwitchDelay->setValue(m_switchDelay);
  ui->cbSwitchDoubleTap->setChecked(m_enableSwitchDoubleTap);
  ui->sbSwitchDoubleTap->setEnabled(ui->cbSwitchDoubleTap->isChecked());
  ui->sbSwitchDoubleTap->setValue(m_switchDoubleTap);
  ui->widgetExternalConfigControls->setEnabled(ui->groupExternalConfig->isChecked());
  toggleExternalConfig(ui->groupExternalConfig->isChecked());
  ui->cbDefaultLockToComputerState->setChecked(m_defaultLockToComputerState);
  ui->cbDisableLockToComputer->setChecked(m_disableLockToComputer);
  ui->cbEnableClipboard->setChecked(m_enableClipboard);
  ui->sbClipboardSizeLimit->setEnabled(m_enableClipboard);
  ui->sbClipboardSizeLimit->setValue(m_clipboardSize);
  refreshPauseAppList();
}

void ServerConfigDialog::initConnections() const
{
  connect(m_buttonBox, &SettingsDialogButtonBox::accepted, this, &ServerConfigDialog::save);
  connect(m_buttonBox, &SettingsDialogButtonBox::rejected, this, &ServerConfigDialog::cancel);
  connect(m_buttonBox, &SettingsDialogButtonBox::reset, this, &ServerConfigDialog::resetFromSettings);
  connect(m_buttonBox, &SettingsDialogButtonBox::restoreDefault, this, &ServerConfigDialog::restoreFromDefaults);
  connect(ui->tabWidget, &QTabWidget::currentChanged, this, &ServerConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->lblRemoveComputer, &RemoveComputerWidget::computerRemoved, this, &ServerConfigDialog::onComputerRemoved);
  connect(ui->btnNewHotkey, &QPushButton::clicked, this, &ServerConfigDialog::addHotkey);
  connect(ui->btnEditHotkey, &QPushButton::clicked, this, &ServerConfigDialog::editHotkey);
  connect(ui->btnRemoveHotkey, &QPushButton::clicked, this, &ServerConfigDialog::removeHotkey);
  connect(ui->listHotkeys, &QListView::doubleClicked, this, &ServerConfigDialog::editHotkey);
  connect(
      ui->listHotkeys->selectionModel(), &QItemSelectionModel::selectionChanged, this,
      &ServerConfigDialog::listHotkeysSelectionChanged
  );

  connect(ui->btnNewAction, &QPushButton::clicked, this, &ServerConfigDialog::addAction);
  connect(ui->btnEditAction, &QPushButton::clicked, this, &ServerConfigDialog::editAction);
  connect(ui->btnRemoveAction, &QPushButton::clicked, this, &ServerConfigDialog::removeAction);
  connect(ui->listActions, &QListView::doubleClicked, this, &ServerConfigDialog::editAction);
  connect(
      ui->listActions->selectionModel(), &QItemSelectionModel::selectionChanged, this,
      &ServerConfigDialog::listActionsSelectionChanged
  );

  connect(ui->rbProtocolBarrier, &QRadioButton::toggled, this, &ServerConfigDialog::toggleProtocol);
  connect(ui->cbHeartbeat, &QCheckBox::toggled, this, &ServerConfigDialog::toggleHeartbeat);
  connect(ui->sbHeartbeat, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setHeartbeat);
  connect(ui->cbWin32KeepForeground, &QCheckBox::toggled, this, &ServerConfigDialog::toggleWin32Foreground);
  connect(ui->cbSwitchDelay, &QCheckBox::toggled, this, &ServerConfigDialog::toggleSwitchDelay);
  connect(ui->sbSwitchDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setSwitchDelay);
  connect(ui->cbSwitchDoubleTap, &QCheckBox::toggled, this, &ServerConfigDialog::toggleSwitchDoubleTap);
  connect(
      ui->sbSwitchDoubleTap, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setSwitchDoubleTap
  );

  connect(ui->cbRelativeMouseMoves, &QCheckBox::toggled, this, &ServerConfigDialog::toggleRelativeMouseMoves);
  connect(ui->cbEnableClipboard, &QCheckBox::toggled, this, &ServerConfigDialog::toggleClipboard);
  connect(ui->btnBrowseConfigFile, &QPushButton::clicked, this, &ServerConfigDialog::browseConfigFile);
  connect(ui->groupExternalConfig, &QGroupBox::toggled, this, &ServerConfigDialog::toggleExternalConfig);
  connect(ui->lineConfigFile, &QLineEdit::textChanged, this, &ServerConfigDialog::setServerConfig);

  connect(
      ui->sbClipboardSizeLimit, QOverload<int>::of(&QSpinBox::valueChanged), this,
      &ServerConfigDialog::setClipboardLimit
  );
  connect(
      ui->cbDefaultLockToComputerState, &QCheckBox::toggled, this, &ServerConfigDialog::toggleDefaultLockToComputerState
  );
  connect(ui->cbDisableLockToComputer, &QCheckBox::toggled, this, &ServerConfigDialog::toggleLockToComputer);
  connect(
      &m_computerSetupModel, &ComputerSetupModel::computersChanged, this,
      &ServerConfigDialog::setButtonBoxEnabledButtons
  );
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, &ServerConfigDialog::updateControls);

  connect(ui->btnAddPauseApp, &QPushButton::clicked, this, &ServerConfigDialog::addPauseApp);
  connect(ui->btnRemovePauseApp, &QPushButton::clicked, this, &ServerConfigDialog::removePauseApp);
  connect(ui->btnClearPauseApps, &QPushButton::clicked, this, &ServerConfigDialog::clearPauseApps);
  connect(ui->cbPauseApp, &QComboBox::activated, this, &ServerConfigDialog::addPauseApp);
  connect(ui->linePauseTitles, &QLineEdit::textChanged, this, &ServerConfigDialog::setButtonBoxEnabledButtons);
}

void ServerConfigDialog::updateControls() const
{
  const bool writable = Settings::isWritable();
  ui->cbDefaultLockToComputerState->setEnabled(writable);
  ui->cbDisableLockToComputer->setEnabled(writable);
  ui->cbEnableClipboard->setEnabled(writable);
  ui->sbClipboardSizeLimit->setEnabled(writable);
  ui->rbProtocolBarrier->setEnabled(writable);
  ui->rbProtocolSynergy->setEnabled(writable);
  ui->cbHeartbeat->setEnabled(writable);
  ui->cbRelativeMouseMoves->setEnabled(writable);
  ui->cbSwitchDelay->setEnabled(writable);
  ui->cbWin32KeepForeground->setEnabled(writable);
  ui->cbSwitchDoubleTap->setEnabled(writable);
  ui->sbSwitchDoubleTap->setEnabled(writable && ui->cbSwitchDoubleTap->isChecked());
  ui->sbSwitchDelay->setEnabled(writable && ui->cbSwitchDelay->isChecked());
  ui->groupExternalConfig->setEnabled(writable);
  ui->groupPauseOnFocus->setEnabled(writable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::restoreFromDefaults()
{
  m_protocol = networkProtocolFromString(Settings::defaultValue(Settings::Server::Protocol).toString());
  m_enableHeartbeat = Settings::defaultValue(Settings::Server::EnableHeartbeat).toBool();
  m_heartbeatRate = Settings::defaultValue(Settings::Server::Heartbeat).toInt();
  m_relativeMouseMoves = Settings::defaultValue(Settings::Server::RelativeMouseMoves).toBool();
  m_win32keepForeground = Settings::defaultValue(Settings::Server::Win32KeepForeground).toBool();
  m_enableSwitchDelay = Settings::defaultValue(Settings::Server::EnableSwitchDelay).toBool();
  m_switchDelay = Settings::defaultValue(Settings::Server::SwitchDelay).toInt();
  m_enableSwitchDoubleTap = Settings::defaultValue(Settings::Server::EnableSwitchDoubleTap).toBool();
  m_switchDoubleTap = Settings::defaultValue(Settings::Server::SwitchDoubleTap).toInt();
  m_defaultLockToComputerState = Settings::defaultValue(Settings::Server::DefaultLockToComputerState).toBool();
  m_disableLockToComputer = Settings::defaultValue(Settings::Server::DisableLockToComputer).toBool();
  m_enableClipboard = Settings::defaultValue(Settings::Server::EnableClipboard).toBool();
  m_clipboardSize = Settings::defaultValue(Settings::Server::ClipboardSize).toUInt();

  ui->groupExternalConfig->setChecked(Settings::defaultValue(Settings::Server::ExternalConfig).toBool());
  ui->lineConfigFile->setText(Settings::defaultValue(Settings::Server::ExternalConfigFile).toString());
  m_pauseApps = Settings::defaultValue(Settings::Server::PauseOnFocusApps).toStringList();
  ui->linePauseTitles->setText(Settings::defaultValue(Settings::Server::PauseOnFocusTitles).toStringList().join(", "));

  refreshControls();
  updateControls();
}

void ServerConfigDialog::setServerConfig()
{
  const auto configFile = ui->lineConfigFile->text();
  if (!QFile::exists(configFile)) {
    m_buttonBox->enableSave(false);
    return;
  }
  serverConfig().setConfigFile(configFile);
  setButtonBoxEnabledButtons();
}

bool ServerConfigDialog::addComputer(const QString &clientName, bool doSilent)
{
  bool isAccepted = false;
  Computer newComputer(clientName);

  if (ComputerSettingsDialog dlg(this, &newComputer, &model().m_computers);
      doSilent || dlg.exec() == QDialog::Accepted) {
    model().addComputer(newComputer);
    isAccepted = true;
  }

  ui->lblNewComputer->setEnabled(!model().isFull());
  return isAccepted;
}

bool ServerConfigDialog::isGeneralConfigModified() const
{
  return m_originalServerConfigIsExternal != Settings::value(Settings::Server::ExternalConfig).toBool() ||
         m_originalServerConfigUsesExternalFile != Settings::value(Settings::Server::ExternalConfigFile).toString() ||
         m_protocol != Settings::networkProtocol() ||
         m_enableClipboard != Settings::value(Settings::Server::EnableClipboard).toBool() ||
         m_clipboardSize != Settings::value(Settings::Server::ClipboardSize).toUInt() ||
         m_enableHeartbeat != Settings::value(Settings::Server::EnableHeartbeat).toBool() ||
         m_heartbeatRate != Settings::value(Settings::Server::Heartbeat).toInt() ||
         m_enableSwitchDelay != Settings::value(Settings::Server::EnableSwitchDelay).toBool() ||
         m_switchDelay != Settings::value(Settings::Server::SwitchDelay).toInt() ||
         m_enableSwitchDoubleTap != Settings::value(Settings::Server::EnableSwitchDoubleTap).toBool() ||
         m_switchDoubleTap != Settings::value(Settings::Server::SwitchDoubleTap).toInt() ||
         m_relativeMouseMoves != Settings::value(Settings::Server::RelativeMouseMoves).toBool() ||
         m_win32keepForeground != Settings::value(Settings::Server::Win32KeepForeground).toBool() ||
         m_disableLockToComputer != Settings::value(Settings::Server::DisableLockToComputer).toBool() ||
         m_defaultLockToComputerState != Settings::value(Settings::Server::DefaultLockToComputerState).toBool() ||
         m_pauseApps != Settings::value(Settings::Server::PauseOnFocusApps).toStringList() ||
         pauseTitles() != Settings::value(Settings::Server::PauseOnFocusTitles).toStringList();
}

bool ServerConfigDialog::isGeneralConfigDefault() const
{
  return ui->groupExternalConfig->isChecked() == Settings::defaultValue(Settings::Server::ExternalConfig).toBool() &&
         ui->lineConfigFile->text() == Settings::defaultValue(Settings::Server::ExternalConfigFile).toString() &&
         m_protocol == networkProtocolFromString(Settings::defaultValue(Settings::Server::Protocol).toString()) &&
         m_enableClipboard == Settings::defaultValue(Settings::Server::EnableClipboard).toBool() &&
         m_clipboardSize == Settings::defaultValue(Settings::Server::ClipboardSize).toUInt() &&
         m_enableHeartbeat == Settings::defaultValue(Settings::Server::EnableHeartbeat).toBool() &&
         m_heartbeatRate == Settings::defaultValue(Settings::Server::Heartbeat).toInt() &&
         m_enableSwitchDelay == Settings::defaultValue(Settings::Server::EnableSwitchDelay).toBool() &&
         m_switchDelay == Settings::defaultValue(Settings::Server::SwitchDelay).toInt() &&
         m_enableSwitchDoubleTap == Settings::defaultValue(Settings::Server::EnableSwitchDoubleTap).toBool() &&
         m_switchDoubleTap == Settings::defaultValue(Settings::Server::SwitchDoubleTap).toInt() &&
         m_relativeMouseMoves == Settings::defaultValue(Settings::Server::RelativeMouseMoves).toBool() &&
         m_win32keepForeground == Settings::defaultValue(Settings::Server::Win32KeepForeground).toBool() &&
         m_disableLockToComputer == Settings::defaultValue(Settings::Server::DisableLockToComputer).toBool() &&
         m_defaultLockToComputerState ==
             Settings::defaultValue(Settings::Server::DefaultLockToComputerState).toBool() &&
         m_pauseApps == Settings::defaultValue(Settings::Server::PauseOnFocusApps).toStringList() &&
         pauseTitles() == Settings::defaultValue(Settings::Server::PauseOnFocusTitles).toStringList();
}

void ServerConfigDialog::setButtonBoxEnabledButtons() const
{
  const bool writable = Settings::isWritable();
  m_buttonBox->enableSave(writable && (isGeneralConfigModified() || !(m_originalServerConfig == m_serverConfig)));
  m_buttonBox->enableReset(writable && (isGeneralConfigModified() || !(m_originalServerConfig == m_serverConfig)));
  m_buttonBox->enableRestoreDefaults(writable && !isGeneralConfigDefault() && ui->tabWidget->currentIndex() == 2);
}

void ServerConfigDialog::addPauseApp()
{
  const auto index = ui->cbPauseApp->currentIndex();
  QString appName;
  if (index >= 0 && ui->cbPauseApp->currentText() == ui->cbPauseApp->itemText(index))
    appName = ui->cbPauseApp->itemData(index).toString();
  else
    appName = ui->cbPauseApp->currentText().trimmed();

  if (appName.isEmpty())
    return;

  if (!m_pauseApps.contains(appName, Qt::CaseInsensitive)) {
    m_pauseApps.append(appName);
    m_pauseApps.sort(Qt::CaseInsensitive);
  }
  refreshPauseAppList();
  ui->cbPauseApp->setCurrentIndex(-1);
  ui->cbPauseApp->setEditText(QString());
  ui->cbPauseApp->setFocus();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::removePauseApp()
{
  const auto selectedItems = ui->listPauseApps->selectedItems();
  if (selectedItems.isEmpty())
    return;

  for (const auto *item : selectedItems)
    m_pauseApps.removeAll(item->data(Qt::UserRole).toString());
  refreshPauseAppList();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::clearPauseApps()
{
  if (m_pauseApps.isEmpty())
    return;

  m_pauseApps.clear();
  refreshPauseAppList();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::refreshPauseAppList()
{
  ui->listPauseApps->clear();
  for (const auto &app : m_pauseApps) {
    const auto description = m_appDescriptions.value(app.toLower());
    const auto label = description.isEmpty() ? app : QStringLiteral("%1 (%2)").arg(description, app);
    auto *item = new QListWidgetItem(label, ui->listPauseApps);
    // The configuration always stores the executable name, the label is only for the user.
    item->setData(Qt::UserRole, app);
    item->setToolTip(app);
  }

  const bool writable = Settings::isWritable();
  ui->btnRemovePauseApp->setEnabled(writable && !m_pauseApps.isEmpty());
  ui->btnClearPauseApps->setEnabled(writable && !m_pauseApps.isEmpty());
}

void ServerConfigDialog::populatePauseAppChoices()
{
  ui->cbPauseApp->clear();
  m_appDescriptions.clear();

  const auto running = runningProcesses();
  QList<deskflow::gui::PauseAppChoice> choices;
  for (const auto &process : running.m_processes) {
    const auto key = process.m_name.toLower();
    if (!m_appDescriptions.contains(key) || m_appDescriptions.value(key).isEmpty())
      m_appDescriptions.insert(key, process.m_description);

    bool known = false;
    for (const auto &seen : choices) {
      if (seen.m_executable.compare(process.m_name, Qt::CaseInsensitive) == 0) {
        known = true;
        break;
      }
    }
    if (!known)
      choices.append({process.m_name, process.m_description, process.m_windowed ? 1 : 2});
  }

  // The window with focus is the settings dialog itself while it is open, so it never counts
  // as the foreground application. Use the application right behind it instead of leaving the
  // foreground group empty, which is what made the group invisible to users.
  const auto ownName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
  const auto foreground =
      deskflow::gui::foregroundExecutable(foregroundProcessName(), ownName, recentForegroundProcessName());

  for (auto &choice : choices) {
    if (!foreground.isEmpty() && choice.m_executable.compare(foreground, Qt::CaseInsensitive) == 0)
      choice.m_group = 0;
  }

  std::sort(choices.begin(), choices.end(), deskflow::gui::pauseAppLess);

  // Group headers are disabled so they can be seen but never chosen, like the task manager groups.
  auto addHeader = [this](const QString &text) {
    const int index = ui->cbPauseApp->count();
    ui->cbPauseApp->addItem(QStringLiteral("── %1 ──").arg(text));
    if (auto *model = qobject_cast<QStandardItemModel *>(ui->cbPauseApp->model()); model != nullptr) {
      if (auto *item = model->item(index); item != nullptr)
        item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
    }
  };

  int counts[3] = {};
  for (const auto &choice : choices)
    counts[choice.m_group]++;

  int currentGroup = -1;
  for (const auto &choice : choices) {
    if (choice.m_group != currentGroup) {
      currentGroup = choice.m_group;
      if (currentGroup == 0)
        addHeader(tr("Foreground (%1)").arg(counts[0]));
      else if (currentGroup == 1)
        addHeader(tr("Background (%1)").arg(counts[1]));
      else
        addHeader(tr("Other processes (%1)").arg(counts[2]));
    }

    const auto label =
        currentGroup == 0 ? QStringLiteral("%1 %2").arg(tr("(current foreground)"), choice.label()) : choice.label();
    ui->cbPauseApp->addItem(label, choice.m_executable);
  }

#ifdef Q_OS_WIN
  // Never hide an empty group silently. These numbers tell which step dropped an application.
  if (!running.m_processes.isEmpty()) {
    qDebug().nospace() << "pause app choices: processes=" << running.m_processes.size()
                       << " windowed=" << running.m_windowed.size() << " windows=" << running.m_windows
                       << " hidden=" << running.m_hiddenWindows << " shell=" << running.m_shellWindows
                       << " own=" << running.m_ownWindows << " cloaked=" << running.m_cloakedWindows
                       << " captionless=" << running.m_captionlessWindows << " helper=" << running.m_helperWindows
                       << " unnamed=" << running.m_unnamedWindows << " groups: foreground=" << counts[0]
                       << " windowed=" << counts[1] << " other=" << counts[2];
    if (counts[0] == 0)
      qWarning() << "pause app choices: no foreground application, the foreground window is ours or unknown";
    if (running.m_windowed.isEmpty())
      qWarning() << "pause app choices: no application with a visible window was enumerated";
  }
#endif

  ui->cbPauseApp->setCurrentIndex(-1);
  ui->cbPauseApp->setEditText(QString());
  refreshPauseAppList();
}

QStringList ServerConfigDialog::pauseTitles() const
{
  QStringList titles;
  for (const auto &part : ui->linePauseTitles->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const auto title = part.trimmed();
    if (!title.isEmpty())
      titles.append(title);
  }
  return titles;
}
