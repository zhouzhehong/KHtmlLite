#ifndef PAGELOADER_H
#define PAGELOADER_H

// Fetches a page and all of its sub-resources (CSS, JS, images, fonts) over
// Qt Network, rewrites the HTML to reference local copies, and hands back a
// file:// URL that KHTML can render without touching KIO. This is the portable
// network layer: it works on Windows, Android, iOS and macOS because it never
// goes through KIO's D-Bus-dependent worker model.
#include <QObject>
#include <QString>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QHash>
#include <QSet>
#include <QDir>
#include <QTimer>

class PageLoader : public QObject
{
    Q_OBJECT
public:
    explicit PageLoader(QObject *parent = nullptr);

    // Start loading |url|. Emits finished(localFileUrl, finalUrl, errorString)
    // when done. finalUrl is the URL after any HTTP redirects (equal to the
    // requested URL when no redirect occurred). On error, localFileUrl is empty
    // and errorString is non-empty.
    void load(const QUrl &url);

    // Where downloaded pages/resources are cached.
    static QString cacheRoot();

Q_SIGNALS:
    void finished(const QUrl &localFileUrl, const QUrl &finalUrl, const QString &error);

private Q_SLOTS:
    void onMainFinished();
    void onResourceFinished();
    void onTimeout();

private:
    QNetworkAccessManager m_nam;
    QNetworkReply *m_mainReply = nullptr;
    QUrl m_original;   // URL the user requested (before redirects)
    QUrl m_finalUrl;   // URL after HTTP redirects (== m_original when none)
    QByteArray m_mainHtml;
    QString m_cacheDir;
    int m_pending = 0;
    int m_criticalPending = 0;
    bool m_failed = false;
    bool m_timedOut = false;
    bool m_finished = false;
    bool m_initialDelivered = false;
    int m_navGeneration = 0;
    QTimer *m_timeout = nullptr;

    struct Res { QUrl url; QString localName; bool isCss = false; bool isJs = false; int gen = 0; };
    QList<Res> m_resources;
    QHash<QNetworkReply*, Res> m_pendingMap;
    // Raw CSS content keyed by local name, held so references can be rewritten
    // after every nested resource (fonts, images, @import sheets) has landed.
    QHash<QString, QByteArray> m_cssRaw;
    QHash<QString, QString> m_cssVars;
    // Literal relative references found in the page (e.g. "red.png") mapped to
    // the local absolute file URL they were rewritten to. Pages often use
    // relative URLs, and the rewrite pass needs them to patch those too.
    QHash<QString, QString> m_rawToLocal;
    // Per-stylesheet variant: a relative url() inside a sheet resolves against
    // the sheet's own URL, so each sheet gets its own raw-reference map.
    QHash<QString, QHash<QString, QString> > m_cssRawToLocal;
    QSet<QString> m_seenUrls;

    void extractResources();
    void extractFromCss(const QByteArray &css, const QUrl &baseUrl, const QString &localName);
    void startDownloads();
    void startOne(const Res &res);
    QString hashName(const QUrl &u) const;
    QString localAbs(const QString &localName) const;
    void rewriteAndSave();
    void rewriteCssFiles();
    void deliverInitialDocument();
    void finalizeSubresources();
    void maybeFinish();
    void fail(const QString &msg);
};

#endif // PAGELOADER_H
