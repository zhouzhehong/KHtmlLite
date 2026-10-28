// Minimal deterministic reproduction for the stale deferred local-file request.
//
// Old engine: Loader::scheduleRequest(file://) deferred the read with a
// zero-delay timer but cancelRequests() only cancelled KIO jobs.  Starting a
// new document in the same event-loop turn destroyed the old DocLoader, then
// the deferred callback emitted requestDone() with the freed DocLoader and
// KHTMLPart::slotLoaderRequestDone() crashed in DocumentImpl::part().
//
// New engine: cancelRequests() drops the pending local Request; the callback
// notices it is no longer pending and exits without touching the freed object.
//
// Build/run on the Windows x64 tree only.  Prints LIFETIME_PROBE_OK on success.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#include <vector>

#include <khtml_part.h>
#include <khtmlview.h>
#include <QWidget>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    const QString dir = QDir::currentPath();
    const QString css = QUrl::fromLocalFile(dir + "/khtml_lifetime_missing.css").toString();

    KHTMLPart part;
    // The real renderer shows an offscreen view before navigating; layout is
    // what triggers the CSS request for a <link> in the initial document.
    if (QWidget *w = part.widget()) {
        w->setAttribute(Qt::WA_DontShowOnScreen, true);
        w->resize(800, 600);
        w->show();
    }
    part.begin(QUrl::fromLocalFile(dir + "/khtml_lifetime_a.html"));
    QString head;
    for (int i = 0; i < 40; ++i) {
        head += "<link rel=\"stylesheet\" href=\"" + css + "\">";
    }
    const QByteArray first =
        ("<html><head>" + head + "</head><body>first</body></html>").toUtf8();
    part.write(first.constData(), first.size());
    part.end();
    if (part.view()) part.view()->layout();

    // Replace the document before the zero-delay local-file read can run.  On
    // the old engine this frees the DocLoader while the Request is still queued.
    std::printf("STAGE first-end\n"); std::fflush(stdout);
    part.closeUrl();
    std::printf("STAGE closed\n"); std::fflush(stdout);
    part.begin(QUrl::fromLocalFile(dir + "/khtml_lifetime_b.html"));
    const QByteArray second = "<html><body>second</body></html>";
    part.write(second.constData(), second.size());
    part.end();

    // The old engine keeps the 40 local-file Requests queued after closeUrl()
    // destroyed the DocLoader.  Force the freed DocumentImpl/DocLoader blocks to
    // be reused before the zero-delay callbacks run: the dumps show exactly such
    // reuse (DocumentImpl memory held QHash-like data), which turns the stale
    // dl->doc() into the 0xC0000005 in DocumentImpl::part().
    std::vector<QByteArray *> fillers;
    for (int i = 0; i < 20000; ++i) {
        const int sz = 64 + (i % 48) * 64;
        fillers.push_back(new QByteArray(sz, char(0x41 + (i % 26))));
    }
    std::printf("STAGE before-exec\n"); std::fflush(stdout);
    QTimer::singleShot(900, &app, &QCoreApplication::quit);
    app.exec();

    for (QByteArray *b : fillers) delete b;
    std::printf("LIFETIME_PROBE_OK\n"); std::fflush(stdout);
    return 0;
}
