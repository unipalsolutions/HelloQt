#include "performance.h"

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>

#include <opencv2/core.hpp>

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace Performance {

namespace {

QString settingsFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

int forced = -1;
bool cpuForced = false;

} // namespace

void force(Level level)
{
    forced = int(level);
}

Level saved()
{
    if (forced >= 0)
        return Level(forced);
    const QSettings settings(settingsFile(), QSettings::IniFormat);
    return Level(std::clamp(settings.value(QStringLiteral("performance"), int(Level::Balanced)).toInt(), 0, 2));
}

bool gpuEnabled()
{
    if (cpuForced)
        return false;
    return QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("useGpu"), true).toBool();
}

void setGpuEnabled(bool enabled)
{
    QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("useGpu"), enabled);
}

bool gpuVerified(const QString &gpu)
{
    if (gpu.isEmpty())
        return false;
    QSettings settings(settingsFile(), QSettings::IniFormat);
    const QString stored = settings.value(QStringLiteral("gpuVerified")).toString();
    if (stored == gpu)
        return true;
    // النسخ السابقة ربطت الفحص باسم الكرت ورقم إصدار البرنامج («الاسم|3.0.x») فكان يسقط مع كل تحديث؛
    // كرت اجتاز الفحص بتلك الصيغة يبقى مقبولاً ويُحفظ بالصيغة الجديدة
    if (stored.count(QLatin1Char('|')) == 1 && gpu.startsWith(stored.section(QLatin1Char('|'), 0, 0) + QLatin1Char('|'))) {
        settings.setValue(QStringLiteral("gpuVerified"), gpu);
        return true;
    }
    return false;
}

void setGpuVerified(const QString &gpu, bool verified)
{
    QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("gpuVerified"), verified ? gpu : QString());
}

bool lastRunOnGpu()
{
    return QSettings(settingsFile(), QSettings::IniFormat).value(QStringLiteral("lastRunOnGpu"), false).toBool();
}

void setLastRunOnGpu(bool onGpu)
{
    QSettings(settingsFile(), QSettings::IniFormat).setValue(QStringLiteral("lastRunOnGpu"), onGpu);
}

void forceCpu()
{
    cpuForced = true;
}

void save(Level level)
{
    QSettings settings(settingsFile(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("performance"), int(level));
}

int threads(Level level)
{
    const int cores = std::max(1, QThread::idealThreadCount());
    switch (level) {
    case Level::Light:
        return std::max(1, cores / 4);
    case Level::Balanced:
        return std::max(1, cores / 2);
    case Level::Max:
        break;
    }
    return cores;
}

void apply(Level level)
{
    const int count = threads(level);
    cv::setNumThreads(count);
    QThreadPool::globalInstance()->setMaxThreadCount(std::max(2, count));
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), level == Level::Max ? NORMAL_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS);
#else
    setpriority(PRIO_PROCESS, 0, level == Level::Max ? 0 : 5);
#endif
}

QString describe(Level level)
{
    const int count = threads(level);
    switch (level) {
    case Level::Light:
        return QStringLiteral("خفيف: %1 نواة بأولوية منخفضة، الجهاز يبقى سريعاً والتحليل أبطأ").arg(count);
    case Level::Balanced:
        return QStringLiteral("متوازن: %1 نواة بأولوية منخفضة (مُستحسن)").arg(count);
    case Level::Max:
        break;
    }
    return QStringLiteral("أقصى سرعة: كل الأنوية (%1)، قد يبطئ الجهاز أثناء التحليل").arg(count);
}

} // namespace Performance
