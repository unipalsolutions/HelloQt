#include "performance.h"

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
