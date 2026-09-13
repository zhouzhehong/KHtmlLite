// Fetch a real page with Qt Network (QNetworkAccessManager) instead of KIO,
// then feed the bytes into KHTMLPart for rendering. This is the portable
// network path that also works on Android/iOS/macOS. It doubles as a render
// compatibility probe for a real, modern search page.
#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QRegularExpression>
#include <KHTMLPart>
#include <khtml_part.h>

static QFile g_log;
static void rec(const QString &s)
{
    if (!g_log.isOpen()) {
        g_log.setFileName(QCoreApplication::applicationDirPath() + "/qnam_result.txt");
        g_log.open(QIODevice::WriteOnly | QIODevice::Append);
    }
    QTextStream(&g_log) << s << Qt::endl; g_log.flush();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    g_log.remove();
    rec("=== QNetworkAccessManager -> KHTML probe ===");

    KHTMLPart *part = new KHTMLPart();
    const QString mode = app.arguments().size() > 1 ? app.arguments().at(1) : QStringLiteral("js");
    part->setJScriptEnabled(mode == QStringLiteral("js"));
    rec("mode = " + mode + (mode == "js" ? " (JS on)" : " (JS off)"));
    part->widget()->resize(1100, 800);
    part->widget()->move(20, 20);
    part->widget()->setWindowTitle(QStringLiteral("KHTML render [%1]").arg(mode));
    part->widget()->show();
    part->widget()->raise();
    part->widget()->activateWindow();

    QNetworkAccessManager nam;
    const QUrl url(QStringLiteral("https://cn.bing.com/"));
    QNetworkRequest req(url);
    // Present a common desktop UA so the server returns the standard page.
    req.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) KHtml/5.116 Safari/537.36");
    rec(">>> QNAM get " + url.toString());

    QNetworkReply *reply = nam.get(req);
    QObject::connect(reply, &QNetworkReply::finished, [&, reply, url]() {
        const QVariant sc = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const QByteArray raw = reply->readAll();
        rec(QStringLiteral("network: HTTP=%1  bytes=%2  contentType=%3  err=%4")
            .arg(sc.toString()).arg(raw.size())
            .arg(QString::fromLatin1(reply->header(QNetworkRequest::ContentTypeHeader).toByteArray()))
            .arg(reply->errorString()));
        if (reply->error() != QNetworkReply::NoError && raw.isEmpty()) {
            rec("!!! fetch failed, nothing to render"); app.quit(); return;
        }
        QString html = QString::fromUtf8(raw);
        if (mode == QStringLiteral("force") || mode == QStringLiteral("strip")) {
            // Strip external/inline scripts and force everything visible so we
            // can tell "content hidden by CSS/JS gate" from "engine can't lay out".
            html.remove(QRegularExpression(QStringLiteral("<script[\\s\\S]*?</script>"),
                                           QRegularExpression::CaseInsensitiveOption));
            const QString gate = QStringLiteral(
                "<style>html,body{background:#fff!important;color:#000!important}"
                "*{visibility:visible!important;opacity:1!important}</style></head>");
            html.replace(QRegularExpression(QStringLiteral("</head>"),
                                            QRegularExpression::CaseInsensitiveOption), gate);
        }
        if (mode == QStringLiteral("strip")) {
            // Remove every external sub-resource reference: these are fetched by
            // KHTML's own loader through KIO, which hangs on Windows and keeps the
            // document in "loading" so first paint never happens.
            const QStringList pats = {
                QStringLiteral("<link[^>]*>"),
                QStringLiteral("<img[^>]*>"),
                QStringLiteral("<source[^>]*>"),
                QStringLiteral("<iframe[\\s\\S]*?</iframe>"),
                QStringLiteral("<meta[^>]*http-equiv[^>]*>"),
                QStringLiteral("url\\(\\s*https?[^)]*\\)")
            };
            for (const QString &p : pats)
                html.remove(QRegularExpression(p, QRegularExpression::CaseInsensitiveOption));
            html.replace(QRegularExpression(QStringLiteral("\\ssrc\\s*=\\s*[\"'][^\"']*[\"']"),
                                            QRegularExpression::CaseInsensitiveOption),
                         QStringLiteral(" data-x="));
        }
        part->begin(url);
        part->write(html);
        part->end();
        rec("fed HTML to KHTML, waiting for parse/script...");

        QTimer::singleShot(4000, [part, mode]() {
            auto js = [part](const QString &e) {
                QVariant v = part->executeScript(e);
                return v.isValid() ? v.toString() : QStringLiteral("<invalid>");
            };
            rec("---- render inspection ----");
            rec("  title: " + js(QStringLiteral("document.title")));
            rec("  readyState: " + js(QStringLiteral("document.readyState")));
            rec("  DOM 元素总数: " + js(QStringLiteral("document.getElementsByTagName('*').length")));
            rec("  a/form/input/script/link: " +
                js(QStringLiteral("var q=function(t){return document.getElementsByTagName(t).length};"
                                  "q('a')+'/'+q('form')+'/'+q('input')+'/'+q('script')+'/'+q('link')")));
            rec("  body 文本长度: " +
                js(QStringLiteral("(document.body&&document.body.textContent||'').length")));
            rec("  body 前 200 字: " +
                js(QStringLiteral("(document.body&&document.body.textContent||'').replace(/\\s+/g,' ').slice(0,200)")));
            // Internal grab: immune to other windows covering the view.
            part->widget()->repaint();
            QCoreApplication::processEvents();
            QTimer::singleShot(800, [part, mode]() {
                QPixmap pm = part->widget()->grab();
                const QString out = QCoreApplication::applicationDirPath()
                                    + QStringLiteral("/bing_%1.png").arg(mode);
                rec(QStringLiteral("internal grab saved=%1 size=%2x%3")
                    .arg(pm.save(out) ? "ok" : "FAIL").arg(pm.width()).arg(pm.height()));
                rec("=== PROBE DONE, window held for screenshot ===");
                QTimer::singleShot(20000, &QCoreApplication::quit);
            });
        });
    });

    QTimer::singleShot(25000, [](){ rec("!!! global timeout"); QCoreApplication::quit(); });
    return app.exec();
}
