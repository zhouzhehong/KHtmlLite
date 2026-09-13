// KHtmlLite - multi-process sandboxed browser shell on KHTML.
//
// Architecture (Safari-style):
//   UI process (this file)  — tabs, address bar, memory budget, crash recovery
//   Renderer process (khtml_renderer.exe) — one per tab, runs KHTMLPart
//
// A renderer crash kills only that tab's process; the UI process stays alive
// and shows a "page crashed, click to reload" placeholder. Background tabs
// are tombstoned (renderer process terminated) after inactivity or memory
// pressure, freeing all engine memory.
#include <QMainWindow>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTabWidget>
#include <QTabBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QStatusBar>
#include <QLabel>
#include <QTimer>
#include <QStyle>
#include <QApplication>
#include <QRegularExpression>
#include <QProcess>
#include <QLocalSocket>
#include <QWindow>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <QDateTime>
#include <QMessageBox>
#include <QStandardPaths>

// Per-tab renderer process manager. Starts khtml_renderer.exe, embeds its
// window, relays navigation via QLocalSocket, and detects crashes.
class RenderTab : public QObject
{
    Q_OBJECT
public:
    explicit RenderTab(QObject *parent = nullptr) : QObject(parent) {
        // Container exists from day one so the tab widget has something to
        // hold before the renderer window is ready.
        m_container = new QWidget;
        m_container->setAttribute(Qt::WA_NativeWindow, true);
    }

    void start(const QUrl &url, int w, int h, bool disableJs = false)
    {
        m_current = url;
        m_socketName = QStringLiteral("khtml_rt_%1_%2")
            .arg(QCoreApplication::applicationPid()).arg(++m_tabCounter);

        m_process = new QProcess(this);
        m_process->setProcessChannelMode(QProcess::MergedChannels);
        const QString exe = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/khtml_renderer.exe");
        QStringList args;
        args << QStringLiteral("--url") << url.toString()
             << QStringLiteral("--socket") << m_socketName
             << QStringLiteral("--width") << QString::number(w)
             << QStringLiteral("--height") << QString::number(h);
        if (disableJs) args << QStringLiteral("--disable-js");

        connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &RenderTab::onFinished);
        // Connect IPC socket immediately — the renderer sends its HWND through
        // it as soon as we connect (chicken-and-egg: we need the socket to
        // receive the HWND, and the HWND to embed the window).
        m_socket = new QLocalSocket(this);
        connect(m_socket, &QLocalSocket::readyRead, this, &RenderTab::onSocketReady);
        connect(m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            // Server not ready yet — retry with backoff.
            if (m_connectRetries < 50) {
                m_connectRetries++;
                QTimer::singleShot(100, this, [this]() {
                    if (m_socket) m_socket->connectToServer(m_socketName);
                });
            }
        });
        m_socket->connectToServer(m_socketName);
        m_process->start(exe, args);
        // Fallback: if no HWND via socket in 3s, read the temp file the
        // renderer writes as a backup.
        QTimer::singleShot(3000, this, [this]() {
            if (m_embedded) return;
            QFile hf(QDir::tempPath() + QStringLiteral("/khtml_hwnd_%1.txt").arg(m_socketName));
            if (hf.open(QIODevice::ReadOnly)) {
                bool ok = false;
                const WId wid = hf.readAll().trimmed().toULongLong(&ok);
                if (ok && wid) embedWindow(wid);
                hf.close();
            }
        });
    }

    void navigate(const QUrl &url)
    {
        m_current = url;
        if (m_socket && m_socket->isOpen()) {
            QJsonObject obj;
            obj[QStringLiteral("cmd")] = QStringLiteral("navigate");
            obj[QStringLiteral("url")] = url.toString();
            m_socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
            m_socket->flush();
        }
    }

    void reload()
    {
        if (m_socket && m_socket->isOpen()) {
            QJsonObject obj;
            obj[QStringLiteral("cmd")] = QStringLiteral("reload");
            m_socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
            m_socket->flush();
        }
    }

    void resize(int w, int h)
    {
        if (m_socket && m_socket->isOpen()) {
            QJsonObject obj;
            obj[QStringLiteral("cmd")] = QStringLiteral("resize");
            obj[QStringLiteral("w")] = w;
            obj[QStringLiteral("h")] = h;
            m_socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
            m_socket->flush();
        }
    }

    void terminate()
    {
        if (m_process) {
            disconnect(m_process, nullptr, this, nullptr);
            m_process->kill();
            // Non-blocking: QProcess cleans up asynchronously via deleteLater.
            // waitForFinished() would block the UI thread for up to 2 s and
            // make the browser window show "Not Responding".
            m_process->deleteLater();
            m_process = nullptr;
        }
        if (m_socket) { m_socket->deleteLater(); m_socket = nullptr; }
        // m_container is owned by the QTabWidget; don't delete here.
        m_embedded = false;
        m_crashed = false;
        m_connectRetries = 0;
    }

    QWidget *container() const { return m_container; }
    QUrl currentUrl() const { return m_current; }
    QString title() const { return m_title; }
    bool isCrashed() const { return m_crashed; }
    bool isRunning() const { return m_process && m_process->state() != QProcess::NotRunning; }

Q_SIGNALS:
    void titleChanged(const QString &title);
    void urlChanged(const QUrl &url);
    void crashed();
    void ready();          // window embedded, ready to show
    void openNewTab(const QUrl &url);

private slots:
    void onStdout()
    {
        while (m_process && m_process->canReadLine()) {
            const QString line = QString::fromUtf8(m_process->readLine()).trimmed();
            if (line.startsWith(QStringLiteral("HWND="))) {
                bool ok = false;
                const WId wid = line.mid(5).toULongLong(&ok);
                if (ok && wid) embedWindow(wid);
            }
        }
    }

    void onFinished(int exitCode, QProcess::ExitStatus status)
    {
        if (m_embedded || exitCode != 0) {
            m_crashed = true;
            emit crashed();
        }
    }

    void onSocketReady()
    {
        while (m_socket && m_socket->canReadLine()) {
            const QByteArray line = m_socket->readLine().trimmed();
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) continue;
            const QJsonObject obj = doc.object();
            const QString type = obj.value(QStringLiteral("type")).toString();
            const QString data = obj.value(QStringLiteral("data")).toString();
            if (type == QLatin1String("hwnd")) {
                bool ok = false;
                const WId wid = data.toULongLong(&ok);
                if (ok && wid) embedWindow(wid);
            } else if (type == QLatin1String("title")) {
                m_title = data;
                emit titleChanged(m_title);
            } else if (type == QLatin1String("url")) {
                const QUrl u(data);
                if (u.isValid()) {
                    m_current = u;
                    emit urlChanged(u);
                }
            } else if (type == QLatin1String("newtab")) {
                emit openNewTab(QUrl(data));
            }
        }
    }

private:
    void embedWindow(WId wid)
    {
        if (m_embedded) return;
        m_embedded = true;
        // m_container already exists (created in constructor) and is already
        // parented inside the QTabWidget. Just plug the renderer window in.
        const HWND containerHwnd = (HWND)m_container->winId();
        const HWND childHwnd = (HWND)wid;
        SetParent(childHwnd, containerHwnd);
        SetWindowLongPtr(childHwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
        EnableWindow(childHwnd, TRUE);
        SetFocus(childHwnd);
        RECT rc;
        GetClientRect(containerHwnd, &rc);
        MoveWindow(childHwnd, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        emit ready();
    }

    QProcess *m_process = nullptr;
    QLocalSocket *m_socket = nullptr;
    QWidget *m_container = nullptr;
    QString m_socketName;
    QUrl m_current;
    QString m_title;
    bool m_embedded = false;
    bool m_crashed = false;
    int m_connectRetries = 0;
    static int m_tabCounter;
};
int RenderTab::m_tabCounter = 0;

// One tab's state: renderer process + history + tombstone metadata.
struct TabPage {
    RenderTab *render = nullptr;
    QWidget *crashWidget = nullptr;  // shown when renderer crashed
    QUrl current;
    QList<QUrl> back;
    QList<QUrl> fwd;
    bool suspended = false;
    QString savedTitle;
    qint64 lastActive = 0;
    bool jsDisabled = false;
    int crashCount = 0;
};

class KHtmlLiteWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit KHtmlLiteWindow(const QUrl &start = QUrl(), QWidget *parent = nullptr)
        : QMainWindow(parent)
    {
        setWindowTitle(QStringLiteral("KHtmlLite"));
        resize(1024, 720);
        buildChrome();
        m_memLabel = new QLabel(QStringLiteral("内存 -- MB"), this);
        statusBar()->addPermanentWidget(m_memLabel);
        m_memTimer = new QTimer(this);
        connect(m_memTimer, &QTimer::timeout, this, &KHtmlLiteWindow::checkMemoryBudget);
        m_memTimer->start(10000);
        addTab(start.isEmpty() ? QUrl(QStringLiteral("https://cn.bing.com")) : start);
        syncChrome();
    }

    void buildChrome()
    {
        QWidget *central = new QWidget(this);
        QVBoxLayout *root = new QVBoxLayout(central);
        root->setContentsMargins(6, 6, 6, 6);
        root->setSpacing(4);
        QHBoxLayout *bar = new QHBoxLayout();
        m_back = mkBtn(QStyle::SP_ArrowBack);
        m_fwd  = mkBtn(QStyle::SP_ArrowForward);
        m_reload = mkBtn(QStyle::SP_BrowserReload);
        for (QPushButton *b : {m_back, m_fwd, m_reload}) {
            b->setFixedWidth(34);
            bar->addWidget(b);
        }
        m_home = new QPushButton(QStringLiteral("主页"), this);
        m_home->setFixedWidth(44);
        bar->addWidget(m_home);
        m_addr = new QLineEdit(central);
        m_addr->setPlaceholderText(QStringLiteral("输入网址或关键词，回车访问 / 搜索（!y 前缀用 Yandex）"));
        m_addr->setClearButtonEnabled(true);
        bar->addWidget(m_addr, 1);
        QPushButton *go = new QPushButton(QStringLiteral("前往"), central);
        go->setFixedWidth(56);
        bar->addWidget(go);
        root->addLayout(bar);
        m_tabs = new QTabWidget(central);
        m_tabs->setTabsClosable(true);
        m_tabs->setMovable(true);
        m_tabs->setStyleSheet(QStringLiteral(
            "QTabWidget::pane { border: 1px solid #c8c8c8; background: #fff; }"
            "QTabBar::tab { background: #e6e6e6; border: 1px solid #c0c0c0;"
            "  border-bottom: none; padding: 3px 12px; min-width: 90px; }"
            "QTabBar::tab:selected { background: #ffffff; }"
            "QTabBar::tab:hover:!selected { background: #efefef; }"));
        QPushButton *newTab = new QPushButton(QStringLiteral("+"));
        newTab->setFixedSize(28, 26);
        newTab->setToolTip(QStringLiteral("新标签页"));
        QHBoxLayout *tabrow = new QHBoxLayout();
        tabrow->setContentsMargins(0, 0, 0, 0);
        tabrow->setSpacing(0);
        tabrow->addWidget(m_tabs, 1);
        tabrow->addWidget(newTab, 0, Qt::AlignTop);
        root->addLayout(tabrow);
        setCentralWidget(central);
        connect(m_back, &QPushButton::clicked, this, &KHtmlLiteWindow::goBack);
        connect(m_fwd, &QPushButton::clicked, this, &KHtmlLiteWindow::goForward);
        connect(m_reload, &QPushButton::clicked, this, [this] {
            TabPage *pg = currentPage();
            if (pg && pg->render && pg->render->isRunning()) pg->render->reload();
            else if (pg && pg->suspended) resumeTab(pg);
        });
        connect(m_home, &QPushButton::clicked, this, [this] {
            navigate(currentPage(), QUrl(QStringLiteral("https://cn.bing.com")), true);
        });
        connect(go, &QPushButton::clicked, this, &KHtmlLiteWindow::onAddressEntered);
        connect(m_addr, &QLineEdit::returnPressed, this, &KHtmlLiteWindow::onAddressEntered);
        connect(newTab, &QToolButton::clicked, this, [this] { addTab(QUrl(QStringLiteral("https://cn.bing.com"))); });
        connect(m_tabs, &QTabWidget::tabCloseRequested, this, &KHtmlLiteWindow::closeTab);
        connect(m_tabs, &QTabWidget::currentChanged, this, [this](int idx) {
            TabPage *pg = (idx >= 0 && idx < m_pages.size()) ? m_pages.at(idx) : nullptr;
            if (pg) pg->lastActive = QDateTime::currentMSecsSinceEpoch();
            if (pg && pg->suspended) resumeTab(pg);
            else syncChrome();
        });
    }

    QPushButton *mkBtn(QStyle::StandardPixmap sp) {
        return new QPushButton(style()->standardIcon(sp), QString(), this);
    }

    TabPage *currentPage() const {
        const int idx = m_tabs->currentIndex();
        return (idx >= 0 && idx < m_pages.size()) ? m_pages.at(idx) : nullptr;
    }

    QWidget *makeCrashWidget(TabPage *pg) {
        QWidget *w = new QWidget;
        QVBoxLayout *lay = new QVBoxLayout(w);
        lay->setAlignment(Qt::AlignCenter);
        QLabel *icon = new QLabel(QStringLiteral("⚠"), w);
        icon->setStyleSheet(QStringLiteral("font-size:48px;"));
        icon->setAlignment(Qt::AlignCenter);
        QLabel *msg = new QLabel(QStringLiteral("页面渲染进程已崩溃\n点击重新加载"), w);
        msg->setAlignment(Qt::AlignCenter);
        msg->setStyleSheet(QStringLiteral("color:#666;font-size:14px;"));
        QPushButton *retry = new QPushButton(QStringLiteral("重新加载"), w);
        retry->setFixedWidth(100);
        connect(retry, &QPushButton::clicked, this, [this, pg]() { resumeTab(pg); });
        lay->addWidget(icon);
        lay->addWidget(msg);
        lay->addWidget(retry, 0, Qt::AlignCenter);
        return w;
    }

    void addTab(const QUrl &url) {
        TabPage *pg = new TabPage;
        pg->current = url;
        pg->lastActive = QDateTime::currentMSecsSinceEpoch();
        startRenderer(pg, url);
        m_tabs->addTab(pg->render ? pg->render->container() : new QWidget,
                       url.isEmpty() ? QStringLiteral("新标签页") : url.host());
        m_pages.append(pg);
        m_tabs->setCurrentIndex(m_pages.size() - 1);
        QTimer::singleShot(0, this, [this, pg]() {
            if (!pg || !pg->render || !pg->render->isRunning())
                return;
            QWidget *container = pg->render->container();
            if (!container)
                return;
            const QSize s = container->size();
            pg->render->resize(s.width(), s.height());
        });
    }

    void startRenderer(TabPage *pg, const QUrl &url) {
        pg->render = new RenderTab(this);
        connect(pg->render, &RenderTab::titleChanged, this, [this, pg](const QString &t) {
            const int idx = m_pages.indexOf(pg);
            if (idx >= 0 && !t.isEmpty()) m_tabs->setTabText(idx, t.left(30));
            if (pg == currentPage())
                setWindowTitle(t.isEmpty() ? QStringLiteral("KHtmlLite")
                                           : QStringLiteral("%1 - KHtmlLite").arg(t));
        });
        connect(pg->render, &RenderTab::urlChanged, this, [this, pg](const QUrl &u) {
            // The renderer reports the final URL after HTTP redirects. Update
            // the tab's current URL and the address bar so the user sees the
            // real document location (e.g. bing.com → cn.bing.com).
            pg->current = u;
            if (pg == currentPage()) {
                m_addr->setText(u.toString());
                updateNavButtons();
            }
        });
        connect(pg->render, &RenderTab::crashed, this, [this, pg]() {
            const int idx = m_pages.indexOf(pg);
            if (idx < 0) return;
            pg->crashCount++;
            // Auto-recovery: first crash → retry with JS disabled (most
            // modern-site crashes are KJS-native; without JS the page
            // degrades gracefully). Second crash → show crash UI.
            if (pg->crashCount == 1 && !pg->jsDisabled) {
                pg->jsDisabled = true;
                if (pg->render) { delete pg->render; pg->render = nullptr; }
                startRenderer(pg, pg->current);
                m_tabs->removeTab(idx);
                m_tabs->insertTab(idx, pg->render->container(), pg->savedTitle);
                m_tabs->setCurrentIndex(idx);
                return;
            }
            if (pg->crashWidget) delete pg->crashWidget;
            pg->crashWidget = makeCrashWidget(pg);
            m_tabs->removeTab(idx);
            m_tabs->insertTab(idx, pg->crashWidget, pg->render ? pg->render->title() : QStringLiteral("已崩溃"));
            m_tabs->setTabText(idx, QStringLiteral("已崩溃"));
        });
        connect(pg->render, &RenderTab::openNewTab, this, [this](const QUrl &u) { addTab(u); });
        pg->render->start(url, 800, 560, pg->jsDisabled);
    }

    void closeTab(int idx) {
        if (idx < 0 || idx >= m_pages.size()) return;
        TabPage *pg = m_pages.takeAt(idx);
        m_tabs->removeTab(idx);
        if (pg->render) pg->render->terminate();
        if (pg->crashWidget) delete pg->crashWidget;
        delete pg;
        if (m_pages.isEmpty()) addTab(QUrl(QStringLiteral("https://cn.bing.com")));
        syncChrome();
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QMainWindow::resizeEvent(event);
        QTimer::singleShot(0, this, [this]() {
            for (TabPage *pg : m_pages) {
                if (!pg || !pg->render || !pg->render->isRunning())
                    continue;
                QWidget *container = pg->render->container();
                if (!container)
                    continue;
                const QSize s = container->size();
                pg->render->resize(s.width(), s.height());
            }
        });
    }

    void closeEvent(QCloseEvent *event) override {
        // Non-blocking shutdown: kill every renderer and close immediately.
        // Renderer processes are terminated asynchronously; the OS reclaims
        // any that are still alive when this process exits. A busy-wait loop
        // with processEvents() here caused the window to hang as "Not
        // Responding" for up to 5 s on close.
        for (TabPage *pg : m_pages) {
            if (pg->render) pg->render->terminate();
        }
        m_pages.clear();
        QMainWindow::closeEvent(event);
    }

    void navigate(TabPage *pg, const QUrl &url, bool pushHistory) {
        if (!pg) return;
        if (pushHistory && pg->current.isValid()) pg->back.append(pg->current);
        pg->fwd.clear();
        pg->current = url;
        pg->lastActive = QDateTime::currentMSecsSinceEpoch();
        if (pg->suspended) { resumeTab(pg); return; }
        if (pg->render && pg->render->isRunning()) pg->render->navigate(url);
        else { startRenderer(pg, url); }
        m_addr->setText(url.toString());
        updateNavButtons();
    }

    void goBack() {
        TabPage *pg = currentPage();
        if (!pg || pg->back.isEmpty()) return;
        pg->fwd.append(pg->current);
        pg->current = pg->back.takeLast();
        navigate(pg, pg->current, false);
    }

    void goForward() {
        TabPage *pg = currentPage();
        if (!pg || pg->fwd.isEmpty()) return;
        pg->back.append(pg->current);
        pg->current = pg->fwd.takeLast();
        navigate(pg, pg->current, false);
    }

    void updateNavButtons() {
        TabPage *pg = currentPage();
        m_back->setEnabled(pg && !pg->back.isEmpty());
        m_fwd->setEnabled(pg && !pg->fwd.isEmpty());
    }

    void syncChrome() {
        TabPage *pg = currentPage();
        if (!pg) return;
        m_addr->setText(pg->current.isValid() ? pg->current.toString() : QString());
        updateNavButtons();
        const QString t = pg->render ? pg->render->title() : QString();
        setWindowTitle(t.isEmpty() ? QStringLiteral("KHtmlLite")
                                   : QStringLiteral("%1 - KHtmlLite").arg(t));
    }

    QUrl resolveInput(const QString &raw) const {
        const QString t = raw.trimmed();
        if (t.isEmpty()) return QUrl();
        if (t.startsWith(QLatin1String("http://")) || t.startsWith(QLatin1String("https://"))
            || t.startsWith(QLatin1String("file://")) || t.startsWith(QLatin1String("about:")))
            return QUrl(t);
        if (t.startsWith(QLatin1String("!y ")))
            return QUrl(QStringLiteral("https://yandex.com/search/?text=%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(t.mid(3)))));
        static const QRegularExpression hostish(
            QStringLiteral("^[a-z0-9-]+(\\.[a-z0-9-]+)+(:[0-9]+)?(/.*)?$"),
            QRegularExpression::CaseInsensitiveOption);
        if (hostish.match(t).hasMatch())
            return QUrl(QStringLiteral("https://%1").arg(t));
        return QUrl(QStringLiteral("https://cn.bing.com/search?q=%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(t))));
    }

    void onAddressEntered() {
        const QUrl u = resolveInput(m_addr->text());
        if (u.isValid()) navigate(currentPage(), u, true);
    }

    // ---- Tombstone / memory budget (Safari-style) ----
    void suspendTab(TabPage *pg) {
        if (!pg || pg->suspended) return;
        pg->savedTitle = m_tabs->tabText(m_pages.indexOf(pg));
        if (pg->savedTitle.isEmpty()) pg->savedTitle = pg->current.host();
        // Kill the renderer process — this frees ALL engine memory (KHTML DLL,
        // JS heap, caches). Only URL + title remain.
        if (pg->render) { pg->render->terminate(); pg->render = nullptr; }
        if (pg->crashWidget) { delete pg->crashWidget; pg->crashWidget = nullptr; }
        pg->suspended = true;
        pg->lastActive = QDateTime::currentMSecsSinceEpoch();
        const int idx = m_pages.indexOf(pg);
        if (idx >= 0) {
            m_tabs->removeTab(idx);
            QLabel *ph = new QLabel(QStringLiteral("  [标签已挂起]  %1\n  点击恢复").arg(pg->savedTitle));
            ph->setAlignment(Qt::AlignCenter);
            ph->setStyleSheet(QStringLiteral("color:#999;font-size:13px;"));
            m_tabs->insertTab(idx, ph, pg->savedTitle + QStringLiteral(" [挂起]"));
        }
    }

    void resumeTab(TabPage *pg) {
        if (!pg || !pg->suspended) return;
        const int idx = m_pages.indexOf(pg);
        if (idx >= 0) m_tabs->removeTab(idx);
        if (pg->render) { delete pg->render; pg->render = nullptr; }
        if (pg->crashWidget) { delete pg->crashWidget; pg->crashWidget = nullptr; }
        startRenderer(pg, pg->current);
        pg->suspended = false;
        if (idx >= 0) {
            m_tabs->insertTab(idx, pg->render->container() ? pg->render->container() : new QWidget, pg->savedTitle);
            m_tabs->setCurrentIndex(idx);
        }
    }

    qint64 currentMemKB() {
        HANDLE h = GetCurrentProcess();
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
            return pmc.WorkingSetSize / 1024;
        return 0;
    }

    void purgeCache() {
        QDir d(QDir::tempPath() + QStringLiteral("/khtml_cache"));
        if (d.exists()) d.removeRecursively();
    }

    void checkMemoryBudget() {
        const qint64 memKB = currentMemKB();
        const qint64 limitKB = 400 * 1024;
        if (m_memLabel)
            m_memLabel->setText(QStringLiteral("内存 %1 MB").arg(memKB / 1024));
        if (memKB > limitKB) {
            for (TabPage *pg : m_pages) {
                if (pg != currentPage() && !pg->suspended) { suspendTab(pg); break; }
            }
            purgeCache();
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 idleMs = 5 * 60 * 1000;
        for (TabPage *pg : m_pages) {
            if (pg != currentPage() && !pg->suspended && pg->lastActive > 0
                && (now - pg->lastActive) > idleMs)
                suspendTab(pg);
        }
    }

    QTimer *m_memTimer = nullptr;
    QLabel *m_memLabel = nullptr;
    QTabWidget *m_tabs = nullptr;
    QList<TabPage *> m_pages;
    QLineEdit *m_addr = nullptr;
    QPushButton *m_back = nullptr, *m_fwd = nullptr, *m_reload = nullptr, *m_home = nullptr;
};

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("KHtmlLite"));
    app.setOrganizationName(QStringLiteral("KHtmlLite"));
    const QString caPath = QCoreApplication::applicationDirPath()
                           + QStringLiteral("/ssl-ca-bundle.crt");
    if (QFile::exists(caPath))
        qputenv("SSL_CERT_FILE", QDir::toNativeSeparators(caPath).toUtf8());
    QUrl start;
    if (argc > 1) start = QUrl(QString::fromLocal8Bit(argv[1]));
    KHtmlLiteWindow w(start);
    w.show();
    return app.exec();
}

#include "khtml_browser.moc"
