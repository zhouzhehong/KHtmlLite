// Network reachability + rendering probe for the KHTML engine.
// Loads a sequence of real URLs through KHTMLPart/KIO and records what
// actually happened: completion vs cancellation, title, DOM size, and a
// couple of JS reads. Everything goes to a file because the GUI subsystem
// has no console when launched detached.
#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QDateTime>
#include <QDir>
#include <QUrl>
#include <QtGlobal>
#include <QMutex>
#include <KHTMLPart>
#include <khtml_part.h>
#include <kparts/part.h>

static QFile g_log;
static QMutex g_mtx;
static void rec(const QString &s)
{
    QMutexLocker l(&g_mtx);
    if (!g_log.isOpen()) {
        g_log.setFileName(QDir::currentPath() + "/netprobe_result.txt");
        g_log.open(QIODevice::WriteOnly | QIODevice::Append);
    }
    QTextStream(&g_log) << QDateTime::currentDateTime().toString("HH:mm:ss.zzz ")
                        << s << Qt::endl;
    g_log.flush();
}

static void msgHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    const QString cat = QString::fromLatin1(ctx.category);
    if (cat.contains("kio", Qt::CaseInsensitive) ||
        cat.contains("khtml", Qt::CaseInsensitive) ||
        cat.contains("ssl", Qt::CaseInsensitive) || type >= QtWarningMsg)
        rec(QStringLiteral("[%1] %2").arg(cat, msg));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    qInstallMessageHandler(msgHandler);
    g_log.remove();
    rec("=== KHTML network probe start ===");

    KHTMLPart *part = new KHTMLPart();
    part->setJScriptEnabled(true);
    part->widget()->resize(1000, 700);
    part->widget()->show();

    const QStringList urls = {
        QStringLiteral("https://cn.bing.com/"),
        QStringLiteral("https://www.baidu.com/")
    };
    int idx = 0;

    auto report = [part](const QString &tag, const QUrl &url) {
        auto js = [part](const QString &expr) {
            QVariant v = part->executeScript(expr);
            return v.isValid() ? v.toString() : QStringLiteral("<invalid>");
        };
        rec(QStringLiteral("---- %1 %2 ----").arg(tag, url.toString()));
        rec(QStringLiteral("  readyState: %1")
            .arg(js(QStringLiteral("document.readyState"))));
        rec(QStringLiteral("  title: %1").arg(js(QStringLiteral("document.title"))));
        rec(QStringLiteral("  DOM 元素总数: %1")
            .arg(js(QStringLiteral("document.getElementsByTagName('*').length"))));
        rec(QStringLiteral("  a/input/form/div 数: %1")
            .arg(js(QStringLiteral("var q=function(t){return document.getElementsByTagName(t).length};"
                                   "q('a')+'/'+q('input')+'/'+q('form')+'/'+q('div')"))));
        rec(QStringLiteral("  body 文本长度: %1")
            .arg(js(QStringLiteral("(document.body&&document.body.textContent||'').length"))));
        rec(QStringLiteral("  JS navigator: %1")
            .arg(js(QStringLiteral("navigator.userAgent"))));
    };

    auto loadNext = [&]() {
        if (idx >= urls.size()) { rec("=== PROBE DONE ==="); app.quit(); return; }
        const QUrl u(urls[idx]);
        rec(QStringLiteral(">>> OPEN %1").arg(u.toString()));
        part->openUrl(u);
    };

    QObject::connect(part, QOverload<>::of(&KParts::ReadOnlyPart::completed), [&]() {
        const QUrl u = part->url();
        rec(QStringLiteral("signal completed() url=%1").arg(u.toString()));
        // let layout/script settle, then inspect
        QTimer::singleShot(2500, [&, u]() {
            report(QStringLiteral("COMPLETED"), u);
            if (++idx >= urls.size()) { rec("=== PROBE DONE ==="); app.quit(); }
            else QTimer::singleShot(500, loadNext);
        });
    });
    QObject::connect(part, &KParts::ReadOnlyPart::canceled, [&](const QString &err) {
        rec(QStringLiteral("signal canceled() reason=%1 url=%2")
            .arg(err, part->url().toString()));
        if (++idx >= urls.size()) { rec("=== PROBE DONE ==="); app.quit(); }
        else QTimer::singleShot(500, loadNext);
    });

    // Hard safety timeout per whole run.
    QTimer::singleShot(40000, [&]() { rec("!!! global timeout, force quit"); app.quit(); });

    QTimer::singleShot(300, loadNext);
    return app.exec();
}
