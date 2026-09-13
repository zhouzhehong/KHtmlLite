// Loads the local CSS capability page through KHTMLPart over file:// and
// reads back the getComputedStyle report written by the page itself. This
// both verifies local sub-resource loading (external .css) and inventories
// which modern CSS features the engine actually understands.
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QDir>
#include <KHTMLPart>
#include <khtml_part.h>
#include <kparts/part.h>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QString candidate;
    const QStringList tries = {
        QCoreApplication::applicationDirPath() + "/cssprobe/test.html",
        QCoreApplication::applicationDirPath() + "/../cssprobe/test.html",
        QStringLiteral("cssprobe/test.html")
    };
    for (const QString &t : tries)
        if (QFileInfo::exists(t)) { candidate = QFileInfo(t).absoluteFilePath(); break; }

    QFile log(QCoreApplication::applicationDirPath() + "/csscap_result.txt");
    log.open(QIODevice::WriteOnly | QIODevice::Truncate);
    QTextStream rec(&log);
    rec << "page: " << candidate << Qt::endl; rec.flush();

    KHTMLPart *part = new KHTMLPart();
    part->setJScriptEnabled(true);
    part->widget()->resize(1000, 860);
    part->widget()->move(20, 20);
    part->widget()->setWindowTitle(QStringLiteral("KHTML CSS capability"));
    part->widget()->show();
    part->widget()->raise();

    QObject::connect(part, QOverload<>::of(&KParts::ReadOnlyPart::completed), [&]() {
        rec << "signal completed()" << Qt::endl; rec.flush();
        QTimer::singleShot(4500, [&, part]() {
            QVariant v = part->executeScript(
                QStringLiteral("document.getElementById('report').textContent"));
            rec << "==== computed-style report ====" << Qt::endl;
            rec << (v.isValid() ? v.toString() : QStringLiteral("<report unavailable>")) << Qt::endl;
            rec << "==== held for screenshot ====" << Qt::endl; rec.flush();
            part->executeScript(QStringLiteral("window.scrollTo(0,620)"));
            QTimer::singleShot(15000, &QCoreApplication::quit);
        });
    });
    QObject::connect(part, &KParts::ReadOnlyPart::canceled, [&](const QString &err) {
        rec << "canceled: " << err << Qt::endl; rec.flush();
    });

    if (candidate.isEmpty()) { rec << "test.html not found" << Qt::endl; return 2; }
    part->openUrl(QUrl::fromLocalFile(candidate));
    QTimer::singleShot(25000, [&]() { rec << "global timeout" << Qt::endl; QCoreApplication::quit(); });
    int rc = app.exec();
    rec << "exit rc=" << rc << Qt::endl;
    return rc;
}
