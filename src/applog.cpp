#include "applog.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

#include <csignal>
#include <cstdio>
#include <exception>
#include <mutex>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#endif

namespace {

constexpr int KeepDays = 30;

std::mutex &logMutex()
{
    static std::mutex mutex;
    return mutex;
}

QString &logFolder()
{
    static QString folder;
    return folder;
}

QString markerPath()
{
    return logFolder() + QStringLiteral("/running.flag");
}

QString fileFor(const QDate &date)
{
    return logFolder() + QStringLiteral("/malamih-%1.log").arg(date.toString(QStringLiteral("yyyy-MM-dd")));
}

void append(const QString &line)
{
    if (logFolder().isEmpty())
        return;
    std::lock_guard<std::mutex> lock(logMutex());
    QFile file(fileFor(QDate::currentDate()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")) << "  " << line << '\n';
}

QtMessageHandler previousHandler = nullptr;

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    // التحذيرات والأخطاء فقط؛ رسائل التتبع كثيرة ولا تفيد المستخدم
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        append(QStringLiteral("[%1] %2").arg(type == QtWarningMsg ? QStringLiteral("تحذير") : QStringLiteral("خطأ"), message));
    if (previousHandler)
        previousHandler(type, context, message);
}

// الانهيار: سطر في السجل (وملف dump على ويندوز لمعرفة مكانه)، دون تحرير ذاكرة قدر الإمكان
#ifdef Q_OS_WIN
wchar_t dumpPath[MAX_PATH] = {};

LONG WINAPI crashFilter(EXCEPTION_POINTERS *info)
{
    append(QStringLiteral("[انهيار] استثناء 0x%1 عند العنوان 0x%2")
               .arg(quint32(info->ExceptionRecord->ExceptionCode), 8, 16, QLatin1Char('0'))
               .arg(quintptr(info->ExceptionRecord->ExceptionAddress), 0, 16));
    if (dumpPath[0]) {
        HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception;
            exception.ThreadId = GetCurrentThreadId();
            exception.ExceptionPointers = info;
            exception.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &exception, nullptr, nullptr);
            CloseHandle(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
void crashSignal(int signal)
{
    append(QStringLiteral("[انهيار] إشارة %1").arg(signal));
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}
#endif

void onTerminate()
{
    QString what = QStringLiteral("خطأ غير معالج");
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception &e) {
            what = QString::fromLocal8Bit(e.what());
        } catch (...) {
        }
    }
    append(QStringLiteral("[انهيار] %1").arg(what));
    std::abort();
}

void removeOld()
{
    const QDate limit = QDate::currentDate().addDays(-KeepDays);
    const QFileInfoList files = QDir(logFolder()).entryInfoList({QStringLiteral("malamih-*.log"), QStringLiteral("crash-*.dmp")},
                                                               QDir::Files);
    for (const QFileInfo &info : files) {
        if (info.lastModified().date() < limit)
            QFile::remove(info.absoluteFilePath());
    }
}

} // namespace

namespace AppLog {

QDateTime start()
{
    logFolder() = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logs");
    QDir().mkpath(logFolder());
    removeOld();

    QDateTime previous;
    QFile marker(markerPath());
    if (marker.open(QIODevice::ReadOnly)) {
        previous = QDateTime::fromString(QString::fromUtf8(marker.readAll()).trimmed(), Qt::ISODate);
        if (!previous.isValid())
            previous = QFileInfo(marker).lastModified();
        marker.close();
    }
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
        marker.write(QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8());
    marker.close();

    previousHandler = qInstallMessageHandler(messageHandler);
    std::set_terminate(onTerminate);
#ifdef Q_OS_WIN
    const QString dump = QDir::toNativeSeparators(
        logFolder() + QStringLiteral("/crash-%1.dmp").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"))));
    const std::wstring wide = dump.toStdWString();
    if (wide.size() < MAX_PATH)
        wcscpy_s(dumpPath, wide.c_str());
    SetUnhandledExceptionFilter(crashFilter);
#else
    for (int signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL})
        std::signal(signal, crashSignal);
#endif

    if (previous.isValid())
        append(QStringLiteral("[تحذير] لم يُغلق البرنامج إغلاقاً طبيعياً في التشغيل السابق (بدأ %1)")
                   .arg(previous.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
    append(QStringLiteral("بدء التشغيل — الإصدار %1").arg(QApplication::applicationVersion()));
    return previous;
}

void finish()
{
    append(QStringLiteral("إغلاق البرنامج"));
    QFile::remove(markerPath());
}

void write(const QString &text)
{
    append(text);
}

QString folder()
{
    return logFolder();
}

QStringList lines(const QDate &date)
{
    QStringList result;
    if (logFolder().isEmpty())
        return result;
    std::lock_guard<std::mutex> lock(logMutex());
    QFile file(fileFor(date));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;
    const QString text = QString::fromUtf8(file.readAll());
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        result << line;
    return result;
}

} // namespace AppLog
