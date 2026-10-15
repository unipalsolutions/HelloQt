#pragma once

#include <QString>

// إعدادات البرنامج العامة للعمل في الخلفية (تُحفظ في settings.ini بجانب بقية الإعدادات)
namespace AppSettings {

QString settingsPath();

// إغلاق النافذة والكاميرا تعمل يخفيها بجانب الساعة بدل إيقاف المراقبة
bool keepRunningInTray();
void setKeepRunningInTray(bool on);
bool startCameraOnLaunch();
void setStartCameraOnLaunch(bool on);
// Windows فقط؛ false ودون أثر في غيره
bool launchWithWindows();
void setLaunchWithWindows(bool on);
bool canLaunchWithWindows();

} // namespace AppSettings
