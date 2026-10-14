// startuplog.h — gated startup timing marks for KHtmlLite.
//
// Enabled by setting environment variable KHTML_STARTUP_TIMING=1 before
// launching khtml_browser.exe (the renderer inherits the environment).
// When enabled, every STARTUP_MARK() appends one line
//   <ms-since-process-start>\t<process>\t<label>
// to <appDir>/khtml_startup.log AND to stderr.  When disabled the marks
// are pure no-ops (single env read, cached).
//
// Use tests/startup_timing_report.py to render a timeline from the log.
#pragma once

// <cstdio> must come from us: this header is also included inside the KHTML
// library sources, where stdio is not guaranteed to be pulled in earlier.
#include <cstdio>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QProcessEnvironment>

namespace StartupLog {

inline QElapsedTimer &clock()
{
    static QElapsedTimer c;
    static bool started = []() { c.start(); return true; }();
    Q_UNUSED(started);
    return c;
}

inline bool enabled()
{
    static const bool e =
        qEnvironmentVariableIntValue("KHTML_STARTUP_TIMING") != 0;
    return e;
}

inline void mark(const char *label)
{
    if (!enabled()) return;
    const qint64 ms = clock().elapsed();
    const QString line = QString::number(ms) + QLatin1Char('\t')
        + QString::fromUtf8(QCoreApplication::applicationName().toUtf8())
        + QLatin1Char('\t') + QString::fromUtf8(label);
    // stderr is merged into the parent console in debug runs.
    fprintf(stderr, "[startup] %s\n", line.toUtf8().constData());
    QFile f(QCoreApplication::applicationDirPath()
            + QStringLiteral("/khtml_startup.log"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(line.toUtf8());
        f.write("\n");
        f.close();
    }
}

} // namespace StartupLog

#define STARTUP_MARK(label) StartupLog::mark(label)
