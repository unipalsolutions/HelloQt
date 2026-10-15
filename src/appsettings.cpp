#include "appsettings.h"

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace AppSettings {

namespace {

const QString RunKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");

} // namespace

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

bool keepRunningInTray()
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("keepRunningInTray"), true).toBool();
}

void setKeepRunningInTray(bool on)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("keepRunningInTray"), on);
}

bool startCameraOnLaunch()
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("startCameraOnLaunch"), false).toBool();
}

void setStartCameraOnLaunch(bool on)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("startCameraOnLaunch"), on);
}

bool canLaunchWithWindows()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool launchWithWindows()
{
#ifdef Q_OS_WIN
    return QSettings(RunKey, QSettings::NativeFormat).contains(QStringLiteral("Malamih"));
#else
    return false;
#endif
}

void setLaunchWithWindows(bool on)
{
#ifdef Q_OS_WIN
    QSettings run(RunKey, QSettings::NativeFormat);
    if (on)
        run.setValue(QStringLiteral("Malamih"), QStringLiteral("\"%1\" --startup").arg(QDir::toNativeSeparators(QApplication::applicationFilePath())));
    else
        run.remove(QStringLiteral("Malamih"));
#else
    Q_UNUSED(on);
#endif
}

} // namespace AppSettings
