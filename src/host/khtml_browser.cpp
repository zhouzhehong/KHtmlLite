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
#include <QSharedMemory>
#include <QImage>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <functional>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <QDateTime>
#include <QMessageBox>
#include <QStandardPaths>

// Per-tab renderer process manager. Starts khtml_renderer.exe, embeds its
// window, relays navigation via QLocalSocket, and detects crashes.
#include <windows.h>
#include <psapi.h>

// PageSurface — browser-side widget that paints the latest software frame
// received from the renderer process via shared memory.  The browser owns
// this widget (and its native HWND); the renderer never owns a visible
// window, so dragging the browser window never touches the renderer.
//
// Input events are forwarded to the Renderer via QLocalSocket JSON IPC.
// mousemove and wheel are coalesced (latest only / accumulated delta);
// click and key events are sent immediately and never coalesced.
class PageSurface : public QWidget
{
    Q_OBJECT
public:
    explicit PageSurface(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_NativeWindow, true);
        setAutoFillBackground(true);
        setFocusPolicy(Qt::StrongFocus);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Qt::white);
        setPalette(pal);
    }
    // Publish a new frame.  dirty is the union of the frame-space rects that
    // actually changed; an empty/null rect repaints everything.  Painting is
    // a pure function of m_frame in every branch of paintEvent, so clipping
    // the update to dirty is always correct; the only case that must stay a
    // full repaint is a frame-size change, where pixels may have shifted or
    // fallen outside the old frame area.
    void setFrame(const QImage &img, const QRect &dirty = QRect()) {
        const bool sizeChanged = m_frame.size() != img.size();
        m_frame = img;
        m_frameW = img.width();
        m_frameH = img.height();
        if (sizeChanged || dirty.isNull() || !dirty.isValid())
            update();
        else
            update(dirty);
    }
    // Map a widget-relative point to frame coordinates.  The frame is always
    // drawn unscaled at the surface origin (see paintEvent), so this is an
    // identity mapping: hit testing and painting must agree on the geometry.
    QPoint mapToFrame(const QPoint &p) const { return p; }
    // Callback set by RenderTab to forward input events to the renderer.
    void setInputCallback(std::function<void(const QJsonObject &)> cb) { m_inputCb = std::move(cb); }

    // Callback set by RenderTab: fires with this surface's real size every time
    // it actually changes.  This widget is the page viewport, so it — not the
    // top level window — is the authoritative source for the renderer size.
    void setResizeCallback(std::function<void(int, int)> cb) { m_resizeCb = std::move(cb); }

    // Enter/leave the browser-side live-resize presentation mode. While true,
    // resizeEvent stops pushing intermediate sizes to the renderer and
    // paintEvent scales the last frame to fill the surface (visual continuity
    // only). The renderer's KHTML viewport/layout size is never changed.
    void setLiveResizing(bool on) {
        if (m_liveResize == on) return;
        m_liveResize = on;
        update();
    }

    // Apply the webpage-determined cursor (Qt::CursorShape as an int) reported
    // by the renderer. This is the on-screen surface, so this is where the
    // CSS cursor takes visible effect. Arrow (0) restores the default arrow.
    void applyCursor(int shape) {
        setCursor(QCursor(static_cast<Qt::CursorShape>(shape)));
    }

protected:
    // When the pointer leaves the page surface, drop the webpage cursor and
    // fall back to the platform default. The renderer only reports cursors
    // while the pointer is over the page.
    void leaveEvent(QEvent *) override {
        unsetCursor();
    }

    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        if (m_liveResize) {
            // Live-resize: do NOT push intermediate pixel sizes to the
            // renderer. Its KHTML viewport/layout stay frozen at the last
            // committed size; paintEvent scales the last frame for visual
            // continuity. The real, final size is committed once at
            // WM_EXITSIZEMOVE (see KHtmlLiteWindow::endLiveResize).
            update();
            return;
        }
        // Push the new page viewport size to the renderer, so it re-lays out
        // and re-renders for this viewport instead of leaving the browser to
        // show a stale framebuffer.
        if (m_resizeCb)
            m_resizeCb(width(), height());
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        if (!m_frame.isNull()) {
            if (m_liveResize && m_frame.size() != size()) {
                // Live-resize: scale the last valid framebuffer to fill the
                // surface purely for visual continuity. This is a temporary
                // browser-side transform only — it never changes the CSS/layout
                // viewport in the renderer. At WM_EXITSIZEMOVE the real size is
                // committed and the renderer repaints at 1:1.
                p.drawImage(rect(), m_frame);
            } else if (m_frame.size() != size()) {
                // Discrete resize transition (maximize/restore/Aero-snap commit):
                // the renderer has not produced a frame for the new viewport
                // yet.  Scale the last frame to fill the surface for visual
                // continuity (same WebKit-style presentation as live-resize);
                // drawing 1:1 in the corner over white looked frozen/broken.
                // Log once per distinct mismatch so the size-sync invariant
                // stays observable.
                if (m_lastLoggedMismatch != m_frame.size()) {
                    m_lastLoggedMismatch = m_frame.size();
                    qWarning("PageSurface: frame size %dx%d != surface size %dx%d (frame scaled until the renderer catches up)",
                             m_frame.width(), m_frame.height(),
                             width(), height());
                }
                p.drawImage(rect(), m_frame);
            } else {
                // Draw the frame unscaled, anchored at the surface origin.
                p.drawImage(0, 0, m_frame);
            }
        } else {
            p.fillRect(rect(), Qt::white);
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        setFocus();
        QPoint fp = mapToFrame(e->pos());
        sendInput(QStringLiteral("mousepress"), e, fp);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        sendInput(QStringLiteral("mouserelease"), e, mapToFrame(e->pos()));
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        // Coalesce: keep only the latest pending mousemove.
        m_pendingMove = *e;
        m_pendingMovePos = mapToFrame(e->pos());
        if (!m_moveTimer) {
            m_moveTimer = new QTimer(this);
            m_moveTimer->setSingleShot(true);
            m_moveTimer->setInterval(0);
            connect(m_moveTimer, &QTimer::timeout, this, [this]() {
                sendInput(QStringLiteral("mousemove"), &m_pendingMove, m_pendingMovePos);
            });
        }
        m_moveTimer->start();
    }
    void wheelEvent(QWheelEvent *e) override {
        // Use angleDelta() in full (both axes). Do NOT quantize by /120.
        m_pendingWheelDeltaX += e->angleDelta().x();
        m_pendingWheelDeltaY += e->angleDelta().y();
        m_pendingWheelPos = mapToFrame(e->pos());
        m_pendingWheelMods = e->modifiers();
        if (!m_wheelTimer) {
            m_wheelTimer = new QTimer(this);
            m_wheelTimer->setSingleShot(true);
            m_wheelTimer->setInterval(16);
            connect(m_wheelTimer, &QTimer::timeout, this, [this]() {
                if (!m_inputCb) return;
                QJsonObject obj;
                obj[QStringLiteral("type")] = QStringLiteral("wheel");
                obj[QStringLiteral("x")] = m_pendingWheelPos.x();
                obj[QStringLiteral("y")] = m_pendingWheelPos.y();
                obj[QStringLiteral("deltaX")] = m_pendingWheelDeltaX;
                obj[QStringLiteral("deltaY")] = m_pendingWheelDeltaY;
                obj[QStringLiteral("modifiers")] = (int)m_pendingWheelMods;
                m_inputCb(obj);
                m_pendingWheelDeltaX = 0;
                m_pendingWheelDeltaY = 0;
            });
        }
        m_wheelTimer->start();
        e->accept();
    }
    void keyPressEvent(QKeyEvent *e) override {
        sendKey(QStringLiteral("keypress"), e);
    }
    void keyReleaseEvent(QKeyEvent *e) override {
        sendKey(QStringLiteral("keyrelease"), e);
    }

private:
    void sendInput(const QString &type, QMouseEvent *e, const QPoint &framePos) {
        if (!m_inputCb) return;
        QJsonObject obj;
        obj[QStringLiteral("type")] = type;
        obj[QStringLiteral("x")] = framePos.x();
        obj[QStringLiteral("y")] = framePos.y();
        obj[QStringLiteral("button")] = (int)e->button();
        obj[QStringLiteral("buttons")] = (int)e->buttons();
        obj[QStringLiteral("modifiers")] = (int)e->modifiers();
        m_inputCb(obj);
    }
    void sendKey(const QString &type, QKeyEvent *e) {
        if (!m_inputCb) return;
        QJsonObject obj;
        obj[QStringLiteral("type")] = type;
        obj[QStringLiteral("key")] = e->key();
        obj[QStringLiteral("text")] = e->text();
        obj[QStringLiteral("modifiers")] = (int)e->modifiers();
        m_inputCb(obj);
    }

    QImage m_frame;
    int m_frameW = 0, m_frameH = 0;
    QSize m_lastLoggedMismatch;
    std::function<void(const QJsonObject &)> m_inputCb;
    QMouseEvent m_pendingMove{QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::NoButton, Qt::NoModifier};
    QPoint m_pendingMovePos;
    QTimer *m_moveTimer = nullptr;
    int m_pendingWheelDeltaX = 0;
    int m_pendingWheelDeltaY = 0;
    QPoint m_pendingWheelPos;
    Qt::KeyboardModifiers m_pendingWheelMods;
    QTimer *m_wheelTimer = nullptr;
    std::function<void(int, int)> m_resizeCb;
    bool m_liveResize = false;
};

class RenderTab : public QObject
{
    Q_OBJECT
public:
    explicit RenderTab(QObject *parent = nullptr) : QObject(parent) {
        // PageSurface owns the visible page area.  The renderer paints into
        // shared memory; this widget draws the latest frame in paintEvent.
        m_container = new PageSurface;
        // Forward input events from PageSurface to the renderer via IPC.
        m_container->setInputCallback([this](const QJsonObject &obj) {
            sendInput(obj);
        });
        // The page surface is the page viewport.  This notification — not the
        // top level window resize — is what drives the renderer size, so the
        // size is always read after the layout has actually been applied.
        // Emitting a signal (rather than sending here) keeps the existing
        // coalescing: a drag-resize still produces one IPC message per idle
        // moment, not one per resize event.
        m_container->setResizeCallback([this](int, int) {
            emit pageResized();
        });
    }

    ~RenderTab() {
        // The page surface can outlive this object (it is owned by the tab
        // widget), so drop the callback that captures `this`.
        if (m_container)
            m_container->setResizeCallback(nullptr);
        // Ensure the renderer process and socket are torn down non-blockingly.
        // Without this, the compiler-generated destructor deletes the QProcess
        // child, whose destructor blocks until the process fully exits — which
        // can freeze the browser UI thread for several seconds.
        terminate();
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
        // Drain renderer stdout/stderr (MergedChannels) so the pipe buffer
        // never fills and blocks the renderer process on write.
        connect(m_process, &QProcess::readyReadStandardOutput,
                this, &RenderTab::onStdout);
        // Connect IPC socket immediately — the renderer sends its HWND through
        // it as soon as we connect (chicken-and-egg: we need the socket to
        // receive the HWND, and the HWND to embed the window).
        m_socket = new QLocalSocket(this);
        connect(m_socket, &QLocalSocket::readyRead, this, &RenderTab::onSocketReady);
        connect(m_socket, &QLocalSocket::connected, this, [this]() {
            sendRestoreIfConnected();
            flushPendingResize();
        });
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

    // Forward an input event (mouse/key/wheel) to the renderer.  Never blocks:
    // writes the JSON line and returns immediately.  The renderer dispatches
    // on its main thread and schedules a frame asynchronously.
    void sendInput(const QJsonObject &obj)
    {
        if (!m_socket || !m_socket->isOpen()) return;
        QJsonObject msg;
        msg[QStringLiteral("cmd")] = QStringLiteral("input");
        for (auto it = obj.begin(); it != obj.end(); ++it)
            msg[it.key()] = it.value();
        m_socket->write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n");
        m_socket->flush();
    }

    void resize(int w, int h)
    {
        if (m_socket && m_socket->isOpen()) {
            QJsonObject obj;
            obj[QStringLiteral("cmd")] = QStringLiteral("resize");
            obj[QStringLiteral("w")] = w;
            obj[QStringLiteral("h")] = h;
            // Growing resize: proactively release our SHM attachment BEFORE
            // sending the message.  On Windows the renderer cannot
            // QSharedMemory::create() a larger segment while we still hold
            // the old one open, so without this the renderer's ensureShm()
            // fails, sends "framesize", and has to wait for our detach plus
            // a whole extra frame tick before it can render at the new size.
            // Detaching here (before the renderer processes the resize) lets
            // its first ensureShm() succeed immediately.
            if (m_shm && m_shm->isAttached() &&
                m_shm->size() < (qint64)w * (qint64)h * 4) {
                m_shm->detach();
                m_attachedShmW = 0;
                m_attachedShmH = 0;
            }
            m_socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
            m_socket->flush();
            m_resizePending = false;
        } else {
            // The renderer process and its IPC socket start asynchronously, so
            // a resize that happens in the meantime cannot be delivered yet.
            // Dropping it leaves the renderer rendering at its command-line
            // default size while the page surface stretches that framebuffer.
            // Remember it and push the surface's real size once the channel is
            // usable (see flushPendingResize).
            m_resizePending = true;
        }
    }

    // Push the page surface's current size to the renderer.  Called when the
    // IPC channel becomes usable, so a resize issued while the renderer was
    // still starting up is never lost.  The size is re-read here (not the
    // value captured earlier) because the layout may have settled since.
    void flushPendingResize()
    {
        if (!m_resizePending || !m_container) return;
        if (!m_socket || !m_socket->isOpen()) return;
        const QSize s = m_container->size();
        if (s.width() <= 0 || s.height() <= 0) return;
        resize(s.width(), s.height());
    }

    // Propagate the browser-side live-resize presentation flag to the visible
    // page surface. While on, the surface scales its last frame instead of
    // pushing intermediate sizes to the renderer (see PageSurface).
    void setLiveResizing(bool on) {
        if (m_container) m_container->setLiveResizing(on);
    }

    // Send tombstone restore (scroll + zoom) to the renderer. If the socket
    // is not connected yet, the values are stored and sent when it connects.
    void setRestoreState(int x, int y, int zoom)
    {
        m_restoreX = x; m_restoreY = y; m_restoreZoom = zoom; m_hasRestore = true;
        sendRestoreIfConnected();
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
        if (m_shm) { m_shm->detach(); m_shm->deleteLater(); m_shm = nullptr; }
        // m_container is owned by the QTabWidget; don't delete here.
        m_crashed = false;
        m_connectRetries = 0;
    }

    QWidget *container() const { return m_container; }
    QUrl currentUrl() const { return m_current; }
    QString title() const { return m_title; }
    bool isCrashed() const { return m_crashed; }
    bool isRunning() const { return m_process && m_process->state() != QProcess::NotRunning; }

    // Renderer process WorkingSet in KB. Returns 0 if not running or
    // the process handle cannot be opened. Used by the browser memory
    // budget so per-tab renderer memory is counted alongside the UI process.
    qint64 memoryKB() const {
        if (!m_process || m_process->state() == QProcess::NotRunning)
            return 0;
        const qint64 pid = m_process->processId();
        if (pid <= 0)
            return 0;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (!h)
            return 0;
        PROCESS_MEMORY_COUNTERS pmc;
        BOOL ok = GetProcessMemoryInfo(h, &pmc, sizeof(pmc));
        CloseHandle(h);
        return ok ? (qint64)(pmc.WorkingSetSize / 1024) : 0;
    }

Q_SIGNALS:
    void titleChanged(const QString &title);
    void urlChanged(const QUrl &url);
    void crashed();
    void ready();          // window embedded, ready to show
    void openNewTab(const QUrl &url);
    void stateChanged(int scrollX, int scrollY, int zoom);
    // The page surface was resized: the renderer viewport has to follow.
    void pageResized();

private slots:
    void onStdout()
    {
        if (m_process)
            m_process->readAllStandardOutput();
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
            if (type == QLatin1String("ready")) {
                // Renderer is up and its offscreen surface is initialized.
                flushPendingResize();
                if (!m_embedded) {
                    m_embedded = true;
                    emit ready();
                }
            } else if (type == QLatin1String("tiles")) {
                // Tiled frame: "w,h" — the shared segment holds a tile grid:
                // 32-byte header (w,h,tilesX,tilesY,dirtyCount), a dirty-tile
                // index list, then tilesX*tilesY 256x256 RGBA tiles.  Only the
                // listed tiles changed; the rest of the canvas stays valid.
                const QStringList parts = data.split(QLatin1Char(','));
                if (parts.size() >= 2) {
                    const int fw = parts[0].toInt();
                    const int fh = parts[1].toInt();
                    if (fw > 0 && fh > 0) {
                        readTiles(fw, fh);
                    }
                }
            } else if (type == QLatin1String("scroll")) {
                // Scroll blit (renderer scroll fast path): the page content
                // shifted rigidly by (dx,dy).  Shift our canvas now and
                // white-fill the exposed edge; the strip tiles that actually
                // changed follow immediately in a "tiles" message.
                const QStringList parts = data.split(QLatin1Char(','));
                if (parts.size() == 2 && m_container && !m_canvas.isNull()) {
                    const int dx = parts[0].toInt();
                    const int dy = parts[1].toInt();
                    if (dx != 0 || dy != 0) {
                        QImage shifted(m_canvas.size(), m_canvas.format());
                        shifted.fill(Qt::white);
                        QPainter sp(&shifted);
                        sp.drawImage(QPoint(-dx, -dy), m_canvas);
                        sp.end();
                        m_canvas = shifted;
                        // Everything moved: full repaint (a partial dirty
                        // rect cannot express "all pixels changed place").
                        m_container->setFrame(m_canvas);
                    }
                }
            } else if (type == QLatin1String("framesize")) {
                // Renderer needs a larger SHM segment but cannot create it
                // while we hold the old one attached. Detach so the next
                // renderer tick can create a segment of the new size.
                const QStringList parts = data.split(QLatin1Char(','));
                if (parts.size() == 2) {
                    m_desiredFrameW = parts[0].toInt();
                    m_desiredFrameH = parts[1].toInt();
                }
                if (m_shm) {
                    m_shm->detach();
                    m_attachedShmW = 0;
                    m_attachedShmH = 0;
                }
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
            } else if (type == QLatin1String("cursor")) {
                // Webpage-determined CSS cursor shape (Qt::CursorShape int),
                // produced by KHTML's hover hit-test. Apply to the visible
                // surface.
                if (m_container)
                    m_container->applyCursor(data.toInt());
            } else if (type == QLatin1String("state")) {
                const QStringList parts = data.split(QLatin1Char(','));
                if (parts.size() >= 3)
                    emit stateChanged(parts[0].toInt(), parts[1].toInt(),
                                      parts[2].toInt());
            }
        }
    }

private:
    void sendRestoreIfConnected()
    {
        if (!m_hasRestore || !m_socket || !m_socket->isOpen()) return;
        QJsonObject obj;
        obj[QStringLiteral("cmd")] = QStringLiteral("restore");
        obj[QStringLiteral("x")] = m_restoreX;
        obj[QStringLiteral("y")] = m_restoreY;
        obj[QStringLiteral("z")] = m_restoreZoom;
        m_socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
        m_socket->flush();
        m_hasRestore = false;
    }

    // === Tiled frame transport: browser-side compositing ===
    // Mirrors the renderer's layout (khtml_renderer.cpp).  The shared
    // segment holds a fixed tile grid; each "tiles" message means the
    // header's dirty-tile list was rewritten with the tiles it names.
    static constexpr int kTileSize = 256;
    static constexpr qsizetype kTileBytes = qsizetype(kTileSize) * kTileSize * 4;
    static constexpr int kHeaderBytes = 32;
    static qsizetype shmNeeded(int w, int h)
    {
        const int tx = (w + kTileSize - 1) / kTileSize;
        const int ty = (h + kTileSize - 1) / kTileSize;
        return qsizetype(kHeaderBytes) + qsizetype(tx) * ty * 4
             + qsizetype(tx) * ty * kTileBytes;
    }

    // Read the dirty tiles from shared memory into the persistent canvas and
    // hand the canvas to the PageSurface.  Clean tiles keep their previous
    // pixels — that is where the tiling transport saves its bandwidth.
    void readTiles(int w, int h)
    {
        if (!m_container) return;
        if (!m_shm) {
            m_shmKey = QStringLiteral("khtml_frame_%1").arg(m_socketName);
            m_shm = new QSharedMemory(m_shmKey, this);
        }
        // Only detach when the attached segment can no longer hold the
        // announced grid — i.e. the renderer actually recreated it bigger.
        // With the renderer's grow-only reuse, a plain frame-size change
        // (restore, then maximize again) no longer means a new segment;
        // detaching/reattaching on every size change was pure churn and
        // added latency to exactly the transition the user notices.
        const qsizetype needed = shmNeeded(w, h);
        if (m_shm->isAttached() && m_shm->size() < needed) {
            m_shm->detach();
            m_attachedShmW = 0;
            m_attachedShmH = 0;
            // The canvas we composited against the old grid is stale.
            m_canvas = QImage();
        }
        if (!m_shm->isAttached()) {
            if (!m_shm->attach()) {
                // Renderer may not have created it yet; skip this frame.
                return;
            }
        }
        if (m_shm->size() < needed) {
            // Size mismatch — do not copy; the renderer will notify us via
            // "framesize" and recreate. Leave attached for the retry.
            return;
        }
        if (!m_shm->lock()) return;
        const uchar *base = (const uchar *)m_shm->constData();
        const qint32 *hdr = (const qint32 *)base;
        const int fw = hdr[0], fh = hdr[1];
        const int tilesX = hdr[2], tilesY = hdr[3];
        const int dirtyCount = hdr[4];
        // Sanity-check the header before trusting the index list.
        if (fw != w || fh != h ||
            tilesX != (w + kTileSize - 1) / kTileSize ||
            tilesY != (h + kTileSize - 1) / kTileSize ||
            dirtyCount < 0 || dirtyCount > tilesX * tilesY) {
            m_shm->unlock();
            return;
        }
        const qint32 *idx = hdr + kHeaderBytes / 4;
        const uchar *tileData = base + kHeaderBytes + qsizetype(dirtyCount) * 4;

        const bool dimsChanged = m_canvas.isNull() ||
                                 m_canvas.width() != w ||
                                 m_canvas.height() != h;
        if (dimsChanged) {
            // Frame-size commit.  Start white, but carry the overlap from the
            // old canvas: a pure height change leaves those tiles valid (the
            // renderer only re-paints the newly exposed bottom strip), so a
            // fresh white canvas would flash on restore/maximize.
            QImage next(w, h, QImage::Format_RGB32);
            next.fill(Qt::white);
            if (!m_canvas.isNull()) {
                QPainter cp(&next);
                cp.drawImage(0, 0, m_canvas);
                cp.end();
            }
            m_canvas = next;
        } else {
            // detach() so we never mutate the QImage the PageSurface may
            // still be painting from.
            m_canvas.detach();
        }
        const int stride = m_canvas.bytesPerLine();
        // Track the union of the tile rects actually copied (frame coords,
        // clipped) so the PageSurface only repaints that area instead of the
        // whole window.  On a frame-size change we repaint fully (handled via
        // QRect() passed to setFrame below).
        QRect dirtyUnion;
        for (int k = 0; k < dirtyCount; ++k) {
            const int t = idx[k];
            if (t < 0 || t >= tilesX * tilesY) continue;
            const int tx = t % tilesX;
            const int ty = t / tilesX;
            const uchar *src = tileData + qsizetype(t) * kTileBytes;
            // Clip right/bottom edges: the frame size is exact, tiles are not.
            const int copyW = qMin(kTileSize, w - tx * kTileSize) * 4;
            const int copyH = qMin(kTileSize, h - ty * kTileSize);
            uchar *dst = m_canvas.scanLine(ty * kTileSize) + qsizetype(tx) * kTileSize * 4;
            for (int y = 0; y < copyH; ++y)
                memcpy(dst + y * stride, src + y * kTileSize * 4, copyW);
            dirtyUnion = dirtyUnion.isNull()
                ? QRect(tx * kTileSize, ty * kTileSize, copyW / 4, copyH)
                : dirtyUnion.united(QRect(tx * kTileSize, ty * kTileSize,
                                          copyW / 4, copyH));
        }
        m_shm->unlock();
        m_attachedShmW = w;
        m_attachedShmH = h;
        m_container->setFrame(m_canvas, dimsChanged ? QRect() : dirtyUnion);
    }

public:

    QProcess *m_process = nullptr;
    QLocalSocket *m_socket = nullptr;
    PageSurface *m_container = nullptr;
    QSharedMemory *m_shm = nullptr;
    QString m_shmKey;
    QString m_socketName;
    QUrl m_current;
    QString m_title;
    bool m_embedded = false;
    bool m_crashed = false;
    // A resize arrived before the IPC socket was connected and still has to be
    // delivered (see resize / flushPendingResize).
    bool m_resizePending = false;
    int m_connectRetries = 0;
    int m_restoreX = 0, m_restoreY = 0, m_restoreZoom = 100;
    bool m_hasRestore = false;
    // SHM bookkeeping: track which frame dimensions we are currently attached
    // to so a renderer-side segment recreation triggers a clean re-attach.
    int m_attachedShmW = 0, m_attachedShmH = 0;
    int m_desiredFrameW = 0, m_desiredFrameH = 0;
    // Persistent page canvas for tile compositing (see readTiles).
    QImage m_canvas;
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
    // Lightweight tombstone: scroll position + zoom for restore after suspend.
    int scrollX = 0;
    int scrollY = 0;
    int zoom = 100;
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
        m_resizeTimer = new QTimer(this);
        m_resizeTimer->setSingleShot(true);
        connect(m_resizeTimer, &QTimer::timeout, this, &KHtmlLiteWindow::applyPendingResize);
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
            // With software frames, each tab's PageSurface is already a child
            // of the QTabWidget stack — no show/hide of renderer windows needed.
            TabPage *pg = (idx >= 0 && idx < m_pages.size()) ? m_pages.at(idx) : nullptr;
            if (pg) pg->lastActive = QDateTime::currentMSecsSinceEpoch();
            if (pg && pg->suspended) resumeTab(pg);
            else {
                syncChrome();
                QTimer::singleShot(0, this, &KHtmlLiteWindow::applyPendingResize);
            }
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
        // A page surface was resized: coalesce through the existing single-shot
        // timer, then push each renderer to its surface's current size.
        connect(pg->render, &RenderTab::pageResized, this, [this]() {
            if (m_resizeTimer)
                m_resizeTimer->start(0);
        });
        connect(pg->render, &RenderTab::stateChanged, this,
                [pg](int x, int y, int zoom) {
                    pg->scrollX = x; pg->scrollY = y; pg->zoom = zoom;
                });
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

    // Called by the coalescing resize timer once a page surface has changed
    // size.  Every tab's surface is laid out with the window, so every running
    // renderer is brought to its surface's current size.
    void applyPendingResize() {
        for (TabPage *pg : m_pages) {
            if (!pg || !pg->render || !pg->render->isRunning())
                continue;
            QWidget *container = pg->render->container();
            if (!container)
                continue;
            const QSize s = container->size();
            if (s.width() <= 0 || s.height() <= 0)
                continue;
            pg->render->resize(s.width(), s.height());
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QMainWindow::resizeEvent(event);
        if (m_liveResize) {
            // Live-resize in progress: do not ship intermediate pixel sizes to
            // the renderer. PageSurface scales its last frame for visual
            // continuity; the real size is committed once at WM_EXITSIZEMOVE.
            return;
        }
        // Coalesce: restarting an already-running single-shot timer discards
        // the previous pending resize, so only the last size in a drag is sent.
        if (m_resizeTimer)
            m_resizeTimer->start(0);
    }

    void moveEvent(QMoveEvent *event) override {
        QMainWindow::moveEvent(event);
        // Software-frame architecture: the browser owns the only visible
        // surface.  No renderer window to reposition, so dragging is purely
        // a local window move — the renderer process is never touched.
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

    // Native live-resize detection. WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE bracket
    // an interactive drag-resize (and Aero-snap), but NOT discrete geometry
    // changes such as maximize / restore / programmatic resize. Those go
    // straight through resizeEvent → the existing coalescing timer, so they
    // commit the real final size immediately with no temporary transform.
    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override {
        if (eventType == "windows_generic_MSG") {
            MSG *msg = reinterpret_cast<MSG *>(message);
            switch (msg->message) {
            case WM_ENTERSIZEMOVE:
                beginLiveResize();
                break;
            case WM_EXITSIZEMOVE:
                endLiveResize();
                break;
            default:
                break;
            }
        }
        return QMainWindow::nativeEvent(eventType, message, result);
    }

    void beginLiveResize() {
        if (m_liveResize) return;
        m_liveResize = true;
        // Cancel any in-flight coalesced resize so no stale intermediate size
        // is committed to the renderer. The renderer keeps its last committed
        // viewport; only the browser-side frame is scaled for continuity.
        if (m_resizeTimer) m_resizeTimer->stop();
        setLiveResizing(true);
    }

    void endLiveResize() {
        if (!m_liveResize) return;
        m_liveResize = false;
        setLiveResizing(false);
        // Commit the real final viewport size exactly once, reusing the
        // existing resize path (no second resize architecture): it reads each
        // PageSurface's final size and sends a single resize IPC per tab. The
        // renderer then does the final layout/repaint at 1:1.
        applyPendingResize();
    }

    void setLiveResizing(bool on) {
        for (TabPage *pg : m_pages) {
            if (pg && pg->render) pg->render->setLiveResizing(on);
        }
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
        // Restore tombstone scroll + zoom. Sent when the renderer socket
        // connects; applied by the renderer after the document completes.
        if (pg->scrollX != 0 || pg->scrollY != 0 || pg->zoom != 100)
            pg->render->setRestoreState(pg->scrollX, pg->scrollY, pg->zoom);
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
        // Non-blocking cache purge: delete a small batch per timer tick so the
        // GUI thread never stalls on a large cache directory.  Previously this
        // called QDir::removeRecursively() synchronously inside checkMemoryBudget(),
        // which could freeze the window for several seconds.
        if (m_purgingCache) return;
        m_purgingCache = true;
        purgeCacheStep();
    }

    void purgeCacheStep() {
        QDir d(QDir::tempPath() + QStringLiteral("/khtml_cache"));
        if (!d.exists()) { m_purgingCache = false; return; }
        QFileInfoList entries = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
        const int batch = 25;
        int count = 0;
        for (const QFileInfo &fi : entries) {
            if (fi.isDir())
                QDir(fi.absoluteFilePath()).removeRecursively();
            else
                QFile::remove(fi.absoluteFilePath());
            if (++count >= batch) break;
        }
        if (entries.size() > batch) {
            QTimer::singleShot(0, this, &KHtmlLiteWindow::purgeCacheStep);
        } else {
            d.rmdir(d.absolutePath());
            m_purgingCache = false;
        }
    }

    void checkMemoryBudget() {
        // Measure browser UI process + every active renderer process.
        // Previously only the browser process was counted, which made the
        // 400 MB threshold meaningless since KHTML/JS memory lives in the
        // out-of-process renderer.
        const qint64 browserKB = currentMemKB();
        qint64 rendererKB = 0;
        for (TabPage *pg : m_pages) {
            if (pg && pg->render && !pg->suspended)
                rendererKB += pg->render->memoryKB();
        }
        const qint64 memKB = browserKB + rendererKB;
        const qint64 limitKB = 400 * 1024;
        if (m_memLabel)
            m_memLabel->setText(QStringLiteral("内存 %1 MB (UI %2 / 渲染 %3)")
                .arg(memKB / 1024).arg(browserKB / 1024).arg(rendererKB / 1024));
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
    QTimer *m_resizeTimer = nullptr;
    bool m_liveResize = false;
    bool m_purgingCache = false;
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
