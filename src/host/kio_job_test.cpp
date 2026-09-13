// Minimal KIO-only fetch, independent of KHTML, to decide whether the KIO
// network layer itself works on this Windows build. Writes the outcome to a
// file because it runs as a console-less GUI binary.
#include <QCoreApplication>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QUrl>
#include <KIO/StoredTransferJob>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QFile f(QCoreApplication::applicationDirPath() + "/kiojob_result.txt");
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    QTextStream o(&f);

    const QUrl url(QStringLiteral("https://cn.bing.com/"));
    o << "storedGet " << url.toString() << Qt::endl; o.flush();

    KIO::StoredTransferJob *job =
        KIO::storedGet(url, KIO::NoReload, KIO::HideProgressInfo);
    QObject::connect(job, &KJob::result, [&]() {
        o << "RESULT error=" << job->error()
          << " errorText=" << job->errorString()
          << " bytes=" << job->data().size() << Qt::endl;
        o.flush();
        app.quit();
    });
    QTimer::singleShot(15000, [&]() { o << "TIMEOUT after 15s\n"; o.flush(); app.quit(); });
    return app.exec();
}
