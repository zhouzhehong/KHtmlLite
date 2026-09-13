// Minimal host for the modernized KHTML engine.
//
// This is not another browser: it is a smoke harness that loads a page
// exercising JavaScript (DOM mutation through the embedded engine) and a
// CSS keyframe animation, then reads the DOM back. Every checkpoint is
// appended to a result file next to the executable, because a Qt widget
// program on Windows has no reliable console once launched detached.
#include <QApplication>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QDateTime>
#include <KHTMLPart>
#include <khtml_part.h>

static QFile g_log;

static void mark(const QString &line)
{
    static bool s_first = true;
    if (!g_log.isOpen()) {
        g_log.setFileName(QDir::currentPath() + "/smoke_result.txt");
        g_log.open(s_first ? (QIODevice::WriteOnly | QIODevice::Truncate)
                           : (QIODevice::WriteOnly | QIODevice::Append));
        s_first = false;
    }
    QTextStream ts(&g_log);
    ts << QDateTime::currentDateTime().toString("HH:mm:ss.zzz ")
       << line << Qt::endl;
    ts.flush();
}

static const char *kTestPage =
    "<!doctype html><html><head><meta charset='utf-8'><style>"
    "@keyframes pulse { from { opacity: 0.2; transform: scale(0.9); }"
    "                  to   { opacity: 1;   transform: scale(1.05); } }"
    "#box { width: 220px; margin: 24px; padding: 28px; font: 22px sans-serif;"
    "       background: #2d5; color: #113; border-radius: 12px;"
    "       animation: pulse 1.1s ease-in-out infinite alternate; }"
    "#fade { margin: 0 24px; padding: 12px; background: #cef; color: #024;"
    "        transition: background-color 0.8s ease; }"
    "</style></head><body>"
    "<div id='box'>KHTML</div>"
    "<div id='fade'>transition target</div>"
    "<div id='result'>PENDING</div>"
    "<script>"
    "var r = document.getElementById('result');"
    "var n = document.getElementsByTagName('div').length;"
    "r.textContent = (n === 3) ? 'JS-OK divs=' + n : 'JS-FAIL ' + n;"
    "var f = document.getElementById('fade');"
    "f.style.backgroundColor = '#fa6';"
    "</script>"
    "</body></html>";

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    mark("step=app-started");

    KHTMLPart *part = new KHTMLPart();
    mark("step=part-constructed");
    part->setJScriptEnabled(true);
    part->setJavaEnabled(false);
    part->widget()->resize(520, 360);
    part->widget()->show();
    mark("step=widget-shown");

    mark("step=before-begin");
    part->begin();
    mark("step=after-begin");
    part->write(QString::fromUtf8(kTestPage));
    mark("step=after-write");
    part->end();
    mark("step=page-loaded");

    QTimer::singleShot(1500, [part, &app]() {
        mark("step=probe-timer-fired");
        QVariant v = part->executeScript(QStringLiteral(
            "document.getElementById('result').textContent"));
        const QString got = v.toString();
        mark(QString("DOM-READBACK: %1").arg(got));
        mark(QString("WIDGET-VISIBLE: %1 SIZE: %2x%3")
             .arg(part->widget()->isVisible() ? "yes" : "no")
             .arg(part->widget()->width()).arg(part->widget()->height()));
        const bool jsOk = got.startsWith(QStringLiteral("JS-OK"));
        mark(jsOk ? "RESULT: PASS (JavaScript executed and mutated DOM)"
                  : "RESULT: CHECK (script result unexpected)");
        // Demo build stays on screen so the CSS animation/result can be seen.
        QTimer::singleShot(60000, [jsOk, &app]() {
            mark(QString("step=quitting code=%1").arg(jsOk ? 0 : 2));
            g_log.close();
            app.exit(jsOk ? 0 : 2);
        });
    });

    int rc = app.exec();
    mark(QString("step=event-loop-returned rc=%1").arg(rc));
    return rc;
}
