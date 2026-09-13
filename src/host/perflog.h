#ifndef PERFLOG_H
#define PERFLOG_H

// Minimal zero-allocation performance logger. Writes timestamped events to
// a per-process log file. Designed to be safe inside hot paths: no QString
// construction on the fast path, no heap allocations beyond the initial
// file open. Use PERF_MARK("event") to record an instant event.
#include <QFile>
#include <QTextStream>
#include <QElapsedTimer>
#include <QString>
#include <cstdio>

class PerfLog
{
public:
    static PerfLog &instance()
    {
        static PerfLog inst;
        return inst;
    }

    void begin(const QString &label)
    {
        if (!m_enabled) return;
        m_timer.restart();
        m_out << "==== NAV: " << label << " ====\n";
        m_out.flush();
        mark("navigation_start");
    }

    void mark(const char *event)
    {
        if (!m_enabled) return;
        m_out << m_timer.nsecsElapsed() / 1000 << "\t" << event << "\n";
        m_out.flush();
    }

    void mark(const char *event, const QString &detail)
    {
        if (!m_enabled) return;
        m_out << m_timer.nsecsElapsed() / 1000 << "\t" << event << "\t" << detail << "\n";
        m_out.flush();
    }

    qint64 elapsedUs() const { return m_timer.nsecsElapsed() / 1000; }

    void setEnabled(bool e)
    {
        m_enabled = e;
        if (e && !m_file.isOpen()) {
            m_file.setFileName(QStringLiteral("C:/Users/zhouzhehong/Desktop/KHtmlLite/perf.log"));
            m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
            m_out.setDevice(&m_file);
        }
    }

private:
    PerfLog() : m_enabled(false) {}
    QFile m_file;
    QTextStream m_out;
    QElapsedTimer m_timer;
    bool m_enabled;
};

#define PERF_BEGIN(label)  PerfLog::instance().begin(QLatin1String(label))
#define PERF_MARK(event)   PerfLog::instance().mark(event)
#define PERF_MARK_D(event, detail) PerfLog::instance().mark(event, detail)
#define PERF_US()          PerfLog::instance().elapsedUs()

#endif // PERFLOG_H
