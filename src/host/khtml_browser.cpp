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
#include <QClipboard>
#include <QPushButton>
#include <cstring>
#include <QToolButton>
#include <QTabWidget>
#include <QTabBar>
#include <QMenu>
#include <QIcon>
#include <QPolygonF>
#include <functional>
#include <QStackedWidget>
#include <QWindow>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QKeySequence>
#include "pageloader.h"
#include "resourcemanager.h"
#include <QUrl>
#include <QLabel>
#include <QTimer>
#include <QStyle>
#include <QApplication>
#include <QScreen>
#include <QRegularExpression>
#include <QProcess>
#include <QLocalSocket>
#include <QWindow>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QSharedMemory>
#include <QShowEvent>
#include <QHideEvent>
#include <QMoveEvent>
#include <rendering/khtml_compositor.h>
#include <QImage>
#include <QPainter>
#include <QTransform>
#include <QtMath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include "startuplog.h"
#include <functional>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <QAction>
#include <QDateTime>
#include <QMessageBox>
#include <QStandardPaths>
#include <QThread>

// Per-tab renderer process manager. Starts khtml_renderer.exe, embeds its
// window, relays navigation via QLocalSocket, and detects crashes.
#include <windows.h>
#include <psapi.h>

// PageSurface — browser-side widget that presents the latest frame received
// from the renderer process via shared memory through a DWM composition
// canvas (D3D11 hardware -> WARP; no GDI/QPainter fallback by policy).  The browser owns
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
        // paintEvent draws the frame 1:1 only while the frame's physical size
        // matches this widget's.  Otherwise it stretches (live-resize preview,
        // where the widget has been resized before the renderer answered), and
        // frame-space dirty rects no longer describe widget space — repaint
        // fully there.  A scroll preview likewise offsets the draw position.
        const qreal dpr = devicePixelRatioF();
        const bool frameMatchesWidget =
                qAbs(img.width() - qRound(width() * dpr)) <= 1
                && qAbs(img.height() - qRound(height() * dpr)) <= 1;
        if (!m_dwmReady) {
            ensureCanvas();
        }
        if (sizeChanged || dirty.isNull() || !dirty.isValid()
                || !frameMatchesWidget
                || m_previewDX != 0 || m_previewDY != 0) {
            update();
            presentFrame();
        } else {
            // `dirty` is in PHYSICAL frame coordinates; update() takes this
            // widget's LOGICAL coordinates.  Map through QRectF and round
            // OUTWARD: QTransform::mapRect(QRect) rounds the origin and the
            // size independently, which can fall a logical pixel short at a
            // fractional dpr (that is exactly how the pre-fix code clipped
            // the repaint to a 1.5x-offset region, leaving ghost pixels in
            // the frame canvas visible on screen forever).
            const QRectF f = QTransform::fromScale(1.0 / dpr, 1.0 / dpr)
                                 .mapRect(QRectF(dirty));
            update(QRect(QPoint(qFloor(f.left()), qFloor(f.top())),
                         QPoint(qCeil(f.right()), qCeil(f.bottom()))));
            presentFrame();
        }
    }
    // Map a widget-relative point to the coordinates the renderer's input
    // path expects: CSS pixels of the page viewport.  This widget's own
    // coordinate system IS that viewport (deviceScaleFactor: the renderer
    // lays KHTML out at CSS pixels and rasters at dpr onto a physical frame),
    // so the mapping is the identity.  The frame is physical, but input
    // targets the LAYOUT, never the framebuffer — dividing by dpr here would
    // shift every click toward the top-left by 1/dpr (the old model needed it
    // because the renderer widget used to be physical).
    QPoint mapToFrame(const QPoint &p) const { return p; }

public:
    // Callback set by RenderTab to forward input events to the renderer.
    void setInputCallback(std::function<void(const QJsonObject &)> cb) { m_inputCb = std::move(cb); }

    // Predictive scroll, presentation-only.  On wheel we add the predicted
    // pixel delta to a transient draw offset so the content appears to move
    // instantly; the committed frame pixels are untouched, so a wrong
    // prediction never drops rows off the canvas edge.  The renderer's
    // authoritative scroll message (absolute offset) calls
    // resetScrollPreview() once the true frame arrives.
    void bumpScrollPreview(int dx, int dy)
    {
        m_previewDX += dx;
        m_previewDY += dy;
        update();
        presentFrame();
    }
    void resetScrollPreview()
    {
        if (m_previewDX == 0 && m_previewDY == 0) return;
        m_previewDX = 0;
        m_previewDY = 0;
        update();
        presentFrame();
    }

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
        presentFrame();
    }

    // Apply the webpage-determined cursor (Qt::CursorShape as an int) reported
    // by the renderer. This is the on-screen surface, so this is where the
    // CSS cursor takes visible effect. Arrow (0) restores the default arrow.
    void applyCursor(int shape) {
        setCursor(QCursor(static_cast<Qt::CursorShape>(shape)));
    }

protected:
    void showEvent(QShowEvent *e) override
    {
        QWidget::showEvent(e);
        if (!m_dwmReady) {
            ensureCanvas();
        }
        if (m_dwm) {
            m_dwm->setVisible(true);
            presentFrame();
        }
    }

    void hideEvent(QHideEvent *e) override
    {
        if (m_dwm) {
            m_dwm->setVisible(false);
        }
        QWidget::hideEvent(e);
    }

    void moveEvent(QMoveEvent *e) override
    {
        QWidget::moveEvent(e);
        if (m_dwm) {
            m_dwm->setGeometry(0, 0, qRound(width() * devicePixelRatioF()),
                               qRound(height() * devicePixelRatioF()));
            presentFrame();
        }
    }

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
        // CSS-px viewport (deviceScaleFactor): send the LOGICAL size.  The
        // renderer lays KHTML out at CSS pixels and rasters at dpr onto the
        // physical frame canvas (RenderTab::resize attaches the ratio).
        if (m_resizeCb) {
            m_resizeCb(int(width()), int(height()));
        }
        if (!m_dwmReady) {
            ensureCanvas();
        }
        if (m_dwm) {
            m_dwm->setGeometry(0, 0, qRound(width() * devicePixelRatioF()),
                               qRound(height() * devicePixelRatioF()));
            presentFrame();
        }
    }

    void paintEvent(QPaintEvent *) override {
        // The DirectComposition overlay owns the page pixels.  This widget only
        // paints the background colour; there is deliberately NO QPainter/GDI
        // fallback: the canvas is hardware D3D11 -> WARP, and if WARP is also
        // unavailable the graphics stack is considered broken (policy).
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
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
        // Predictive scroll: shift the visible canvas NOW (~0.5px per wheel
        // unit, close to Qt's default 3-line scroll step); the renderer's
        // authoritative scroll message reconciles any prediction error.
        // Predictive scroll, presentation-only: ~0.5px per wheel unit (close
        // to Qt's default 3-line scroll step).  Moved pixels show instantly;
        // the renderer's authoritative scroll message re-anchors.
        {
            const int pdx = qRound(e->angleDelta().x() * 0.5);
            const int pdy = qRound(e->angleDelta().y() * 0.5);
            if (pdx || pdy)
                bumpScrollPreview(pdx, pdy);
        }
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
    std::function<void(const QJsonObject &)> m_inputCb;
    // Transient predictive-scroll draw offset (pixels).  Presentation only;
    // always returns to 0 when the renderer's authoritative offset arrives.
    int m_previewDX = 0, m_previewDY = 0;
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

    // ---- DWM composition canvas (GPU hardware -> WARP -> GDI fallback) ----
    void ensureCanvas()
    {
        if (m_dwmReady) {
            return;
        }
        m_dwmReady = true;
        if (!isVisible() || width() <= 0 || height() <= 0) {
            // The overlay is created on the first show/resize instead.
            m_dwmReady = false;
            return;
        }
        const qreal dpr = devicePixelRatioF();
        m_dwm = new khtml::KHTMLCompositor;
        const bool forceWarp = qEnvironmentVariableIsSet("KHTML_FORCE_WARP");
        const bool forceFail = qEnvironmentVariableIsSet("KHTML_FORCE_D3D_FAIL");
        if (!m_dwm->create((void *)winId(), 0, 0,
                         qRound(width() * dpr), qRound(height() * dpr),
                         forceWarp, forceFail)) {
            delete m_dwm;
            m_dwm = nullptr;
            // No GDI fallback by policy: hardware D3D11 -> WARP.  If even
            // WARP composition is unavailable, the graphics stack itself is
            // broken and a software QPainter path would not be meaningful.
            fprintf(stderr, "[canvas] backend=none — D3D11/WARP composition "
                            "unavailable (no fallback by policy)\n");
            fflush(stderr);
            update();
        }
    }

    void presentFrame()
    {
        if (!m_dwm || !m_dwm->isActive() || m_frame.isNull()) {
            return;
        }
        const qreal dpr = devicePixelRatioF();
        const int pw = qRound(width() * dpr);
        const int ph = qRound(height() * dpr);
        if (pw <= 0 || ph <= 0) {
            return;
        }
        const bool matches = qAbs(m_frame.width() - pw) <= 1
                             && qAbs(m_frame.height() - ph) <= 1;
        QImage composed;
        const bool needsCompose = !matches || m_previewDX != 0 || m_previewDY != 0;
        if (needsCompose) {
            composed = QImage(pw, ph, QImage::Format_RGB32);
            composed.fill(Qt::white);
            QPainter cp(&composed);
            if (matches) {
                // Same sign convention as applyScrollAuthority(): Qt wheel-up
                // (positive angleDelta) must move content DOWN while the
                // authoritative renderer frame is pending.
                cp.drawImage(QPoint(qRound(m_previewDX * dpr),
                                    qRound(m_previewDY * dpr)), m_frame);
            } else {
                cp.setRenderHint(QPainter::SmoothPixmapTransform, !m_liveResize);
                cp.drawImage(QRect(0, 0, pw, ph), m_frame);
            }
            cp.end();
        }
        const QImage &src = needsCompose ? composed : m_frame;
        if (!m_dwm->present(src.constBits(), src.width(), src.height(),
                            src.bytesPerLine())) {
            delete m_dwm;
            m_dwm = nullptr;
            fprintf(stderr, "[canvas] backend=none — DWM/WARP presentation "
                            "failed (no fallback by policy)\n");
            fflush(stderr);
            update();
        }
    }

    khtml::KHTMLCompositor *m_dwm = nullptr;
    bool m_dwmReady = false;
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
        QString exe;
        // === Process pre-warm (WebKit precedent): one renderer is spawned
        // idle at browser startup (about:blank, socket listening).  The tab
        // that arrives while it is warm ADOPTS it — skipping the ~500 ms
        // process+Qt+KHTMLPart construction path entirely.  The adopted
        // process then navigates once the socket handshake completes.
        if (!disableJs && s_warmProcess
                && s_warmProcess->state() != QProcess::NotRunning) {
            m_socketName = s_warmSocket;
            m_process = s_warmProcess;
            s_warmProcess = nullptr;
            m_process->setParent(this);
            m_adopted = true;
            assignJob();               // already running: sandbox now
            STARTUP_MARK("tab start (warm-hit)");
        } else {
            m_socketName = QStringLiteral("khtml_rt_%1_%2")
                .arg(QCoreApplication::applicationPid()).arg(++m_tabCounter);
            m_process = new QProcess(this);
            m_process->setProcessChannelMode(QProcess::ForwardedChannels);
            exe = QCoreApplication::applicationDirPath()
                  + QStringLiteral("/khtml_renderer.exe");
            QStringList args;
            // Ship the ratio on the command line so the renderer's FIRST
            // parse already lays out and evaluates @media at the real dpr —
            // the socket resize arrives later than the initial load.
            const qreal startDpr = m_container
                    ? m_container->devicePixelRatioF()
                    : (QGuiApplication::primaryScreen()
                           ? QGuiApplication::primaryScreen()->devicePixelRatio()
                           : 1.0);
            args << QStringLiteral("--url") << url.toString()
                 << QStringLiteral("--socket") << m_socketName
                 << QStringLiteral("--width") << QString::number(w)
                 << QStringLiteral("--height") << QString::number(h)
                 << QStringLiteral("--dpr") << QString::number(startDpr, 'g', 4);
            if (disableJs) args << QStringLiteral("--disable-js");
            STARTUP_MARK("tab start (cold)");
            m_process->start(exe, args);
            STARTUP_MARK("renderer process start");
            // Sandbox the renderer into its Job Object as soon as the pid
            // exists (QProcess::started fires after a successful CreateProcess).
            connect(m_process, &QProcess::started, this, [this]() { assignJob(); });
        }
        // Keep one warm renderer standing by for the NEXT tab.  Delayed a
        // beat so the very first (cold) tab is not competing with the warm
        // spawn for cold-start I/O — WebKit pre-warms off an idle timer for
        // the same reason.
        QTimer::singleShot(600, []() { prewarm(); });

        connect(m_process, &QProcess::readyReadStandardOutput,
                this, &RenderTab::onStdout);
        connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &RenderTab::onFinished);
        m_socket = new QLocalSocket(this);
        connect(m_socket, &QLocalSocket::readyRead, this, &RenderTab::onSocketReady);
        connect(m_socket, &QLocalSocket::connected, this, [this]() {
            STARTUP_MARK("socket connected");
            sendRestoreIfConnected();
            flushPendingResize();
        });
        connect(m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            // Server not ready yet — retry with a short progressive backoff:
            // the renderer normally listens within the first few hundred ms,
            // so early retries should be quick; fall back to 100 ms later.
            if (m_connectRetries < 50) {
                const int delay = m_connectRetries < 10 ? 25 : 100;
                m_connectRetries++;
                QTimer::singleShot(delay, this, [this]() {
                    if (m_socket) m_socket->connectToServer(m_socketName);
                });
            }
        });
        m_socket->connectToServer(m_socketName);
    }

    void resetScrollPosition()
    {
        m_baseCX = 0;
        m_baseCY = 0;
        if (m_container) {
            m_container->resetScrollPreview();
        }
    }

    void navigate(const QUrl &url)
    {
        m_current = url;
        // New document: the renderer restarts at scroll (0,0).  The canvas
        // authority base belongs to the previous page; keeping it would make
        // the first frames shift by the old offset (visible jump / wrong
        // strip after back/forward and rapid link clicks).
        resetScrollPosition();
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
        resetScrollPosition();
        if (m_socket && m_socket->isOpen()) {
            QJsonObject obj;
            obj[QStringLiteral("cmd")] = QStringLiteral("reload");
            m_socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
            m_socket->flush();
        }
    }

    // Stop the current page load; the renderer reports loaddone when closeUrl
    // has completed.  Used by the address-field stop icon during real loading.
    void stop()
    {
        if (m_socket && m_socket->isOpen()) {
            QJsonObject obj;
            obj[QStringLiteral("cmd")] = QStringLiteral("stop");
            m_socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
            m_socket->flush();
        }
    }

    // Background-tab frame throttling: hidden tabs tell their renderer to
    // stop the 60 Hz frame timer; damage accumulates there and one full
    // frame is rendered when the tab becomes visible again (WebKit: hidden
    // pages get no DisplayRefreshMonitor ticks).
    void setVisible(bool vis)
    {
        if (m_socket && m_socket->isOpen()) {
            QJsonObject obj;
            obj[QStringLiteral("cmd")] = QStringLiteral("visible");
            obj[QStringLiteral("v")] = vis ? 1 : 0;
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
        // w/h are the LOGICAL (CSS px) viewport; the ratio to physical pixels
        // rides along so the renderer can lay out at CSS px and raster at dpr.
        const qreal dpr = m_container ? m_container->devicePixelRatioF() : 1.0;
        // Redundancy guard: a same-size resize would trigger a synchronous
        // KHTML relayout in the renderer for nothing (tab switches re-fire
        // surface resizes, and drags that end on the start size).  Only a
        // genuinely new viewport is shipped.  A fresh renderer (resume,
        // crash-restart) starts at m_lastSentW=0, so its first size always
        // goes out.
        if (w == m_lastSentW && h == m_lastSentH && dpr == m_lastSentDpr)
            return;
        if (m_socket && m_socket->isOpen()) {
            QJsonObject obj;
            obj[QStringLiteral("cmd")] = QStringLiteral("resize");
            obj[QStringLiteral("w")] = w;
            obj[QStringLiteral("h")] = h;
            obj[QStringLiteral("dpr")] = dpr;
            // Growing resize: proactively release our SHM attachment BEFORE
            // sending the message.  On Windows the renderer cannot
            // QSharedMemory::create() a larger segment while we still hold
            // the old one open, so without this the renderer's ensureShm()
            // fails, sends "framesize", and has to wait for our detach plus
            // a whole extra frame tick before it can render at the new size.
            // Detaching here (before the renderer processes the resize) lets
            // its first ensureShm() succeed immediately.
            const qsizetype physBytes = qsizetype(qRound(w * dpr))
                                      * qsizetype(qRound(h * dpr)) * 4;
            if (m_shm && m_shm->isAttached() && m_shm->size() < physBytes) {
                m_shm->detach();
                m_attachedShmW = 0;
                m_attachedShmH = 0;
            }
            m_socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
            m_socket->flush();
            m_resizePending = false;
            m_lastSentW = w;
            m_lastSentH = h;
            m_lastSentDpr = dpr;
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

    // === Renderer sandbox: per-process Job Object (Windows shape of
    // WebKit's per-WebProcess memory limits) ===
    // Every renderer lives in its own job with (1) a hard COMMIT ceiling
    // that tracks the dynamic per-renderer budget (set every watchdog
    // tick, 2x the soft budget — the soft layer tombstones on working-set
    // growth long before commit can hit the cap) and (2)
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: if the browser ever dies
    // abnormally, the kernel instantly reaps every renderer — no orphaned
    // KHTML/QuickJS processes left waiting for the 10-min watchdog.
    void assignJob()
    {
        if (m_job || !m_process || m_process->state() == QProcess::NotRunning)
            return;
        const qint64 pid = m_process->processId();
        if (pid <= 0)
            return;
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (!h)
            return;
        applyJobLimit(h, 2 * 768 * 1024 * 1024LL);   // provisional; tuned per tick
        HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, (DWORD)pid);
        if (!proc || !AssignProcessToJobObject(h, proc)) {
            CloseHandle(h);
            if (proc) CloseHandle(proc);
            return;
        }
        CloseHandle(proc);
        m_job = h;
    }
    static void applyJobLimit(HANDLE h, qint64 commitBytes)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        jeli.ProcessMemoryLimit = (SIZE_T)qMax<qint64>(128ll * 1024 * 1024, commitBytes);
        SetInformationJobObject(h, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
    }
    // Dynamic tuning: the hard ceiling follows the current soft budget.
    void setMemoryCeilingKB(qint64 kb)
    {
        if (kb <= 0) return;
        m_ceilingKB = kb;
        if (m_job)
            applyJobLimit(m_job, kb * 1024);
    }

    void terminate()
    {
        if (m_job) {
            // Kill-on-close makes this an immediate kernel reap; the
            // process is being terminated here anyway.
            CloseHandle(m_job);
            m_job = nullptr;
        }
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
    // Last committed page pixels; used for the tombstone snapshot when the
    // tab is suspended (Safari keeps the frozen page visible as an anchor).
    QImage canvasImage() const { return m_canvas; }
    // Prefill the surface with a previous snapshot while the new renderer
    // is still starting/loading — the user keeps a visual anchor instead
    // of staring at a white surface.  Real tiles overwrite it seamlessly.
    void seedFrame(const QImage &img) { if (m_container && !img.isNull()) { resetScrollPosition(); m_container->setFrame(img); } }
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

    // Cumulative kernel+user CPU time of the renderer process in
    // milliseconds (0 = not running / handle refused). The memory/CPU
    // watchdog samples this every tick and turns the delta into a
    // per-core utilization figure, which drives the CPU-adaptive half
    // of the tombstone policy (Safari suspends busy background tabs to
    // enforce its "background CPU ~ 0" rule even while memory is free).
    qint64 cpuTimeMs() const {
        if (!m_process || m_process->state() == QProcess::NotRunning)
            return 0;
        const qint64 pid = m_process->processId();
        if (pid <= 0)
            return 0;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (!h)
            return 0;
        FILETIME ftCreate, ftExit, ftKernel, ftUser;
        BOOL ok = GetProcessTimes(h, &ftCreate, &ftExit, &ftKernel, &ftUser);
        CloseHandle(h);
        if (!ok)
            return 0;
        const qint64 k = (qint64(ftKernel.dwHighDateTime) << 32) | ftKernel.dwLowDateTime;
        const qint64 u = (qint64(ftUser.dwHighDateTime) << 32) | ftUser.dwLowDateTime;
        return (k + u) / 10000;  // FILETIME 100ns units -> ms
    }

    // === Renderer pre-warm pool (WebKit WebProcessPool::prewarmedWebProcess) ===
    // One spare renderer process sits paused-after-init, about:blank loaded,
    // socket listening.  start() adopts it when a real tab appears, avoiding
    // ~500 ms of process+Qt+KHTMLPart construction per new tab.
    static void prewarm()
    {
        if (s_warmProcess && s_warmProcess->state() != QProcess::NotRunning)
            return;
        if (s_warmProcess) { s_warmProcess->deleteLater(); s_warmProcess = nullptr; }
        const QString exe = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/khtml_renderer.exe");
        s_warmSocket = QStringLiteral("khtml_warm_%1_%2")
            .arg(QCoreApplication::applicationPid()).arg(++m_tabCounter);
        QProcess *p = new QProcess(qApp);
        // ForwardedChannels: renderer debug output (startup marks, env-gated
        // [rend] lines) flows to OUR stderr instead of being drained into
        // nowhere.  No Qt pipe is involved, so there is no buffer to fill
        // and no drain loop needed.
        p->setProcessChannelMode(QProcess::ForwardedChannels);
        const qreal warmDpr = QGuiApplication::primaryScreen()
                ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
        p->start(exe, QStringList() << QStringLiteral("--url") << QStringLiteral("about:blank")
                 << QStringLiteral("--socket") << s_warmSocket
                 << QStringLiteral("--width") << QStringLiteral("800")
                 << QStringLiteral("--height") << QStringLiteral("560")
                 << QStringLiteral("--dpr") << QString::number(warmDpr, 'g', 4));
        s_warmProcess = p;
    }

Q_SIGNALS:
    void titleChanged(const QString &title);
    void urlChanged(const QUrl &url);
    void crashed();
    void ready();          // window embedded, ready to show
    void firstFrame();     // first composited page frame (recovery hand-off)
    void loadStarted();
    void loadProgress(int percent);
    void loadFinished();
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
            // Diagnostic only: distinguish crash from user/navigation teardown.
            // terminate() disconnects this finished() connection, so normal tab
            // close / suspend / browser exit never reaches here.
            fprintf(stderr, "[rend-crash] pid=%d embedded=%d exit=%d status=%s url=%s\n",
                    (int)(m_process ? m_process->processId() : 0), (int)m_embedded,
                    exitCode, status == QProcess::CrashExit ? "CrashExit" : "NormalExit",
                    m_current.toString().toUtf8().constData());
            fflush(stderr);
            m_crashed = true;
            emit crashed();
        }
    }

    void onSocketReady()
    {
        static bool s_loggedFirstData = false;
        if (!s_loggedFirstData) {
            s_loggedFirstData = true;
            STARTUP_MARK("socket first data received");
        }
        while (m_socket && m_socket->canReadLine()) {
            const QByteArray line = m_socket->readLine().trimmed();
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) continue;
            const QJsonObject obj = doc.object();
            const QString type = obj.value(QStringLiteral("type")).toString();
            const QString data = obj.value(QStringLiteral("data")).toString();
            if (type == QLatin1String("ready")) {
                // Renderer is up and its offscreen surface is initialized.
                // data = renderer-local ms at send → precise inter-process
                // clock skew for the startup log (us minus renderer).
                if (!data.isEmpty() && StartupLog::enabled()) {
                    const qint64 rendMs = data.toLongLong();
                    const qint64 skew = StartupLog::clock().elapsed() - rendMs;
                    STARTUP_MARK(QStringLiteral("clock skew ms=%1")
                                 .arg(skew).toUtf8().constData());
                }
                flushPendingResize();
                if (!m_embedded) {
                    m_embedded = true;
                    // Adopted (pre-warmed) renderer loaded about:blank; the
                    // real URL arrives now, as soon as the pipe works.
                    if (m_adopted && m_current.isValid())
                        navigate(m_current);
                    emit ready();
                }
            } else if (type == QLatin1String("tiles")) {
                // Tiled frame: "w,h[,cx,cy]" — the shared segment holds a tile
                // grid: 32-byte header (w,h,tilesX,tilesY,dirtyCount), a
                // dirty-tile index list, then tilesX*tilesY 256x256 RGBA
                // tiles.  Only the listed tiles changed; the rest of the
                // canvas stays valid.  cx,cy carry the renderer's absolute
                // scroll offset, letting us reconcile predictive scroll.
                const QStringList parts = data.split(QLatin1Char(','));
                if (parts.size() >= 2) {
                    const int fw = parts[0].toInt();
                    const int fh = parts[1].toInt();
                    if (fw > 0 && fh > 0) {
                        if (parts.size() >= 4)
                            applyScrollAuthority(parts[2].toInt(),
                                                 parts[3].toInt());
                        readTiles(fw, fh);
                    }
                }
            } else if (type == QLatin1String("scroll")) {
                // Scroll blit / reconciliation: "sdx,sdy,cx,cy".  The page
                // content shifted rigidly by (sdx,sdy) and now sits at the
                // absolute offset (cx,cy).  Rather than trusting the delta we
                // reconcile against the absolute offset, which also corrects
                // any predictive scroll the user applied via the wheel since
                // the last authoritative frame.
                const QStringList parts = data.split(QLatin1Char(','));
                if (parts.size() >= 4 && m_container && !m_canvas.isNull())
                    applyScrollAuthority(parts[2].toInt(), parts[3].toInt());
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
            } else if (type == QLatin1String("loadstart")) {
                emit loadStarted();
            } else if (type == QLatin1String("loadprogress")) {
                emit loadProgress(qBound(0, data.toInt(), 100));
            } else if (type == QLatin1String("loaddone")) {
                emit loadFinished();
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

    // === Predictive scroll (WebKit async-scrolling analogue) ===
    // Two layers, deliberately separated:
    //  1. PageSurface paints the canvas with a transient "preview offset"
    //     between wheel event and the renderer's authority message — pure
    //     presentation, provides instant feedback; committed pixels are NOT
    //     touched, so a wrong prediction never loses rows off the canvas
    //     edge (that was the failure the protocol test caught).
    //  2. applyScrollAuthority() shifts the canvas by the EXACT absolute
    //     delta reported by the renderer ((cx,cy) rides on every "scroll"
    //     and "tiles" message) and resets the preview.  Strip tiles in the
    //     accompanying frame then fill the newly exposed rows.
    void shiftCanvas(int dx, int dy)
    {
        if (m_canvas.isNull() || (dx == 0 && dy == 0)) return;
        QImage shifted(m_canvas.size(), m_canvas.format());
        shifted.fill(Qt::white);
        QPainter sp(&shifted);
        sp.drawImage(QPoint(-dx, -dy), m_canvas);
        sp.end();
        m_canvas = shifted;
    }

    void applyScrollAuthority(int cx, int cy)
    {
        if (!m_container) return;
        // cx/cy arrive in CSS pixels (KHTML's scroll offset now that the
        // renderer lays out at CSS px); the canvas is physical.  Convert the
        // ABSOLUTE offsets, never the delta, so per-step rounding cannot
        // accumulate over a long scroll.
        const qreal dpr = m_container->devicePixelRatioF();
        const int pcx = qRound(cx * dpr);
        const int pcy = qRound(cy * dpr);
        const int dx = pcx - m_baseCX;
        const int dy = pcy - m_baseCY;
        m_baseCX = pcx;
        m_baseCY = pcy;
        m_container->resetScrollPreview();
        if (dx == 0 && dy == 0)
            return;
        if (!m_canvas.isNull()) {
            shiftCanvas(dx, dy);
            // Everything moved: full repaint.
            m_container->setFrame(m_canvas);
        }
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
            // Frame-size commit.  WebKit2 continuity rule: the UI process
            // never exposes an unpainted canvas.  Start from the old canvas
            // STRETCHED to the new size — any region the next tiles message
            // does not cover keeps showing page content instead of white
            // (this was the maximize white flash: first partial frame
            // after a resize replaced the surface with a half-white
            // canvas, bypassing the stale-frame stretch in paintEvent) —
            // then carry the 1:1 overlap on top so pure-height strip
            // repaints land on exact pixels.
            QImage next(w, h, QImage::Format_RGB32);
            next.fill(Qt::white);
            if (!m_canvas.isNull()) {
                QPainter cp(&next);
                if (m_canvas.width() != w || m_canvas.height() != h) {
                    cp.drawImage(QRect(0, 0, w, h), m_canvas);
                }
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
        if (!m_gotFirstFrame) {
            m_gotFirstFrame = true;
            STARTUP_MARK("tiles message dispatched");
            STARTUP_MARK("first frame composited");
            emit firstFrame();
        }
    }

public:

    QProcess *m_process = nullptr;
    HANDLE m_job = nullptr;          // per-renderer sandbox job (see assignJob)
    qint64 m_ceilingKB = 0;          // current hard commit ceiling
    QLocalSocket *m_socket = nullptr;
    PageSurface *m_container = nullptr;
    QSharedMemory *m_shm = nullptr;
    QString m_shmKey;
    QString m_socketName;
    QUrl m_current;
    QString m_title;
    bool m_embedded = false;
    bool m_crashed = false;
    bool m_gotFirstFrame = false;  // startup timing: first tiles composited
    // A resize arrived before the IPC socket was connected and still has to be
    // delivered (see resize / flushPendingResize).
    bool m_resizePending = false;
    // Last viewport size actually shipped to the renderer (see resize()):
    // same-size requests are dropped instead of re-triggering a relayout.
    int m_lastSentW = 0, m_lastSentH = 0;   // logical (CSS px) viewport
    qreal m_lastSentDpr = 0.0;
    int m_connectRetries = 0;
    int m_restoreX = 0, m_restoreY = 0, m_restoreZoom = 100;
    bool m_hasRestore = false;
    // SHM bookkeeping: track which frame dimensions we are currently attached
    // to so a renderer-side segment recreation triggers a clean re-attach.
    int m_attachedShmW = 0, m_attachedShmH = 0;
    int m_desiredFrameW = 0, m_desiredFrameH = 0;
    // Persistent page canvas for tile compositing (see readTiles).
    QImage m_canvas;
    // Predictive-scroll bookkeeping (see applyScrollAuthority): the content
    // offset the committed canvas currently corresponds to.  The transient
    // wheel prediction lives in PageSurface (presentation only).
    int m_baseCX = 0, m_baseCY = 0;
    static int m_tabCounter;
    // Pre-warm pool state.
    static QProcess *s_warmProcess;
    static QString s_warmSocket;
    bool m_adopted = false;   // renderer came from prewarm; navigates after ready
};
int RenderTab::m_tabCounter = 0;
QProcess *RenderTab::s_warmProcess = nullptr;
QString RenderTab::s_warmSocket;

// One tab's state: renderer process + history + tombstone metadata.
struct TabPage {
    RenderTab *render = nullptr;
    QWidget *crashWidget = nullptr;  // shown when renderer crashed
    QWidget *recoveryWidget = nullptr;  // "恢复中" placeholder during automatic/manual restart
    QWidget *tombstoneWidget = nullptr;  // shown while suspended (owns snapshot view)
    bool cacheCleanupRetried = false;  // second crash already cleared regenerable HTTP/page cache
    bool loading = false;
    int loadProgress = 0;
    QUrl current;
    QList<QUrl> back;
    QList<QUrl> fwd;
    bool suspended = false;
    QString savedTitle;
    qint64 lastActive = 0;
    bool navPushed = false;   // navigate() already pushed the previous URL
    bool jsDisabled = false;
    int crashCount = 0;
    // Lightweight tombstone: scroll position + zoom for restore after suspend.
    int scrollX = 0;
    int scrollY = 0;
    int zoom = 100;
    // Last displayed frame, captured at suspend.  Shown while the tab is
    // tombstoned and as the seed canvas during resume (mechanism: Safari's
    // frozen-page snapshot — memory is freed, pixels remain).
    QImage snapshot;
    // CPU-adaptive tombstone bookkeeping (sampled by checkMemoryBudget):
    // cumulative renderer CPU ms at the last tick and per-core utilization
    // (per-mille) over the last tick window — status-bar diagnostics only;
    // eviction decisions follow SYSTEM load (memory pressure + total CPU).
    qint64 cpuTimeMs = 0;
    int cpuPerMill = 0;
};

// Tombstone placeholder: paints the last snapshot (top-left anchored, scaled
// to fit) under a light veil, with the tab title centred.  Interactive input
// is intentionally NOT forwarded — the renderer is dead; clicking the tab
// itself triggers resume via currentChanged.
class TombstoneWidget : public QWidget
{
public:
    TombstoneWidget(const QImage &snap, const QString &title, QWidget *parent = nullptr)
        : QWidget(parent), m_snap(snap), m_title(title)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(247, 247, 247));
        if (!m_snap.isNull()) {
            const QImage scaled = m_snap.scaled(size(), Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation);
            p.drawImage(QPoint(0, 0), scaled);
            // Frozen signal: faint white veil + grey title badge.
            p.fillRect(scaled.rect(), QColor(255, 255, 255, 60));
        }
        if (!m_title.isEmpty()) {
            p.setPen(QColor(120, 120, 120));
            QFont f = font();
            f.setPointSize(12);
            p.setFont(f);
            p.drawText(rect(), Qt::AlignCenter,
                       QStringLiteral("%1\n（已挂起 · 点击标签恢复）").arg(m_title));
        }
    }
private:
    QImage m_snap;
    QString m_title;
};

// Frameless-window drag area for the Safari-style chrome: dragging the tab
// strip / toolbar background moves the window; double-click toggles maximize.
class DragStrip : public QWidget
{
public:
    explicit DragStrip(QWidget *parent = nullptr) : QWidget(parent) {}
protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            if (QWindow *w = window() ? window()->windowHandle() : nullptr)
                w->startSystemMove();
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            QMainWindow *mw = qobject_cast<QMainWindow *>(window());
            if (mw) {
                if (mw->isMaximized()) mw->showNormal();
                else mw->showMaximized();
            }
            e->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(e);
    }
};

// Safari-style tab bar: rounded tabs in WhiteSur-Monterey colours
// (#f5f5f5 inactive / #f0f0f0 hover / #e8e8e8 selected, 8 px radius,
// 32 px tab height in the reference; 22 logical px here to keep the page
// content origin at the historical (11, 87) physical px).  Draws its own
// close "x" on the active/hovered tab; no Qt default close buttons.
class SafariTabBar : public QTabBar
{
public:
    explicit SafariTabBar(QWidget *parent = nullptr) : QTabBar(parent)
    {
        setMouseTracking(true);
        setElideMode(Qt::ElideRight);
        setUsesScrollButtons(true);
        setDrawBase(false);
        setExpanding(false);
        setMovable(true);
        setTabsClosable(false);
        setContextMenuPolicy(Qt::CustomContextMenu);
    }

    std::function<void(int)> onCloseTab;

protected:
    QSize tabSizeHint(int index) const override
    {
        QSize s = QTabBar::tabSizeHint(index);
        s.setHeight(22);
        // Integrated tab strip is only 220 logical wide.  Dividing the
        // available strip among the open tabs keeps the FIRST tab visible
        // (and clickable) when a second tab is selected; the old fixed 144
        // minimum forced QTabBar to scroll it out of view.
        const int avail = width() > 0 ? width() : 220;
        const int n = qMax(1, count());
        s.setWidth(qBound(96, avail / n, 220));
        return s;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < count(); ++i) {
            const bool sel = (i == currentIndex());
            const bool hov = (i == m_hover);
            QRectF r = QRectF(tabRect(i)).adjusted(2.0, 2.0, -2.0, -2.0);
            // Selected tab joins the warm toolbar; inactive tabs stay flat.
            const QColor fill = sel ? QColor(0xFB, 0xF7, 0xF8)
                                    : (hov ? QColor(0xE9, 0xDF, 0xE2) : QColor(0xED, 0xE4, 0xE7));
            p.setPen(QPen(sel ? QColor(0, 0, 0, 42) : QColor(0, 0, 0, 16), 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, sel ? 1.0 : -0.5), 4, 4);

            QFont f = font();
            f.setPixelSize(11);
            p.setFont(f);
            p.setPen(sel ? QColor(58, 52, 56) : QColor(112, 105, 109));
            const int closeW = (sel || hov) ? 18 : 7;
            QRectF textR = r.adjusted(9, 0, -closeW, 0);
            p.drawText(textR, Qt::AlignVCenter | Qt::AlignLeft,
                       fontMetrics().elidedText(tabText(i).startsWith(QLatin1Char('&'))
                                                 ? tabText(i).mid(1) : tabText(i),
                                                 Qt::ElideRight,
                                                int(textR.width())));

            if (sel || hov) {
                const QPointF c(r.right() - 12, r.center().y());
                p.setPen(QPen(QColor(122, 114, 118), 1.4, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(c + QPointF(-3.0, -3.0), c + QPointF(3.0, 3.0));
                p.drawLine(c + QPointF(3.0, -3.0), c + QPointF(-3.0, 3.0));
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int idx = tabAt(e->pos());
        if (idx != m_hover) {
            m_hover = idx;
            update();
        }
        QTabBar::mouseMoveEvent(e);
    }

    void leaveEvent(QEvent *e) override
    {
        m_hover = -1;
        update();
        QTabBar::leaveEvent(e);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const int idx = tabAt(e->pos());
        if (idx >= 0 && e->button() == Qt::LeftButton) {
            m_lastCaptionClick = 0;
            const QRect r = tabRect(idx);
            const QRect closeRect(r.right() - 20, r.center().y() - 7, 16, 14);
            if (closeRect.contains(e->pos())) {
                if (onCloseTab) {
                    onCloseTab(idx);
                }
                e->accept();
                return;
            }
        }
        if (idx < 0 && e->button() == Qt::LeftButton) {
            // Bug 2: the blank title-strip area is part of this stretch-filled
            // QTabBar, not of the parent DragStrip.  Handle the caption here
            // (this was the no-drag root cause).  A manual double-click check
            // is needed because the first press starts the system move loop,
            // which can swallow Qt's normal double-click event.
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const bool dbl = m_lastCaptionClick > 0
                    && now - m_lastCaptionClick <= QApplication::doubleClickInterval()
                    && (e->pos() - m_lastCaptionPos).manhattanLength()
                       <= QApplication::startDragDistance();
            m_lastCaptionClick = now;
            m_lastCaptionPos = e->pos();
            if (dbl) {
                m_lastCaptionClick = 0;
                if (QMainWindow *mw = qobject_cast<QMainWindow *>(window())) {
                    if (mw->isMaximized()) mw->showNormal();
                    else mw->showMaximized();
                }
                e->accept();
                return;
            }
            if (QWindow *win = window() ? window()->windowHandle() : nullptr)
                win->startSystemMove();
            e->accept();
            return;
        }
        QTabBar::mousePressEvent(e);
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && tabAt(e->pos()) < 0) {
            // Also honor Qt's double-click event when the platform delivers it.
            m_lastCaptionClick = 0;
            if (QMainWindow *mw = qobject_cast<QMainWindow *>(window())) {
                if (mw->isMaximized()) mw->showNormal();
                else mw->showMaximized();
            }
            e->accept();
            return;
        }
        QTabBar::mouseDoubleClickEvent(e);
    }

private:
    int m_hover = -1;
    qint64 m_lastCaptionClick = 0;
    QPoint m_lastCaptionPos;
};

class AddressEdit : public QLineEdit
{
public:
    explicit AddressEdit(QWidget *parent = nullptr) : QLineEdit(parent) {}

    // -1 hides the line; 0..100 is the real KHTML loading progress.
    void setLoadProgress(int progress)
    {
        progress = qBound(0, progress, 100);
        if (m_loadProgress == progress) return;
        m_loadProgress = progress;
        update();
    }
    void clearLoadProgress()
    {
        if (m_loadProgress < 0) return;
        m_loadProgress = -1;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QLineEdit::paintEvent(event);
        if (m_loadProgress < 0) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        qreal w = (width() - 2.0) * m_loadProgress / 100.0;
        // A 2px leading segment makes the real "started, 0%" state visible;
        // it is not a synthetic animation.
        if (w < 2.0) w = 2.0;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x0A, 0x84, 0xFF));
        p.drawRoundedRect(QRectF(1.0, height() - 3.0, w, 2.0), 1.0, 1.0);
    }

private:
    int m_loadProgress = -1;
};

// One-window toolbar / caption metrics (logical pixels).  The native title
// bar is extended away; CaptionButtonsWidget paints the three Windows glyphs
// while WM_NCHITTEST returns the real HT* codes so the system still performs
// min/max/close, Snap Layouts and drag behavior.
static constexpr int kUnifiedHeaderHeight = 58;
static constexpr int kCaptionButtonWidth = 31;
static constexpr int kCaptionButtonsWidth = kCaptionButtonWidth * 3;

class CaptionButtonsWidget : public QWidget
{
public:
    explicit CaptionButtonsWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
    }

    void setHovered(int index)
    {
        if (m_hover == index) return;
        m_hover = index;
        update();
    }

protected:
    int buttonAt(const QPoint &p) const
    {
        const int bw = qMax(1, width() / 3);
        const int idx = qBound(0, p.x() / bw, 2);
        return rect().contains(p) ? idx : -1;
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        setHovered(buttonAt(e->pos()));
        QWidget::mouseMoveEvent(e);
    }

    void leaveEvent(QEvent *e) override
    {
        setHovered(-1);
        QWidget::leaveEvent(e);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(e);
            return;
        }
        m_pressed = buttonAt(e->pos());
        setHovered(m_pressed);
        update();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int idx = buttonAt(e->pos());
        if (idx >= 0 && idx == m_pressed) {
            QWidget *top = window();
            if (auto *mw = qobject_cast<QMainWindow *>(top)) {
                if (idx == 0) mw->showMinimized();
                else if (idx == 1) {
                    if (mw->isMaximized()) mw->showNormal();
                    else mw->showMaximized();
                } else {
                    mw->close();
                }
            }
        }
        m_pressed = -1;
        update();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(0xF3, 0xEC, 0xEF));
        // Preserve the toolbar's bottom hairline across the overlay.
        p.setPen(QPen(QColor(0xDC, 0xD2, 0xD5), 1));
        p.drawLine(0, height() - 1, width(), height() - 1);

        const int bw = qMax(1, width() / 3);
        for (int i = 0; i < 3; ++i) {
            const QRect r(i * bw, 0, (i == 2 ? width() - 2 * bw : bw), height());
            if (m_hover == i) {
                p.fillRect(r, i == 2 ? QColor(0xE8, 0x11, 0x23)
                                     : QColor(0xE5, 0xDF, 0xE2));
            }
            const QColor glyph = (m_hover == 2 && i == 2)
                    ? QColor(255, 255, 255) : QColor(0x45, 0x40, 0x44);
            p.setPen(QPen(glyph, 1.2));
            const QPointF c(r.center());
            if (i == 0) {                         // minimize
                p.drawLine(QPointF(c.x() - 5, c.y()),
                           QPointF(c.x() + 5, c.y()));
            } else if (i == 1) {                  // maximize / restore
                p.drawRect(QRectF(c.x() - 5, c.y() - 5, 10, 10));
            } else {                              // close
                p.drawLine(QPointF(c.x() - 5, c.y() - 5),
                           QPointF(c.x() + 5, c.y() + 5));
                p.drawLine(QPointF(c.x() + 5, c.y() - 5),
                           QPointF(c.x() - 5, c.y() + 5));
            }
        }
    }

private:
    int m_hover = -1;
    int m_pressed = -1;
};

class KHtmlLiteWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit KHtmlLiteWindow(const QUrl &start = QUrl(), QWidget *parent = nullptr)
        : QMainWindow(parent)
    {
        setWindowTitle(QStringLiteral("KHtmlLite"));
        // Native Win11 frame is retained for DWM shadow/rounded corners, but
        // WM_NCCALCSIZE extends the client over the caption.  The tab/address
        // toolbar therefore starts at the very top; CaptionButtonsWidget draws
        // the min/max/close glyphs on the same row.  Do not set
        // FramelessWindowHint.  Default window size — LOGICAL (Qt/CSS) pixels:
        // sizing follows the desktop-browser convention, a fraction of the
        // work area capped at a sane default DIP size (Chrome/Edge open
        // ~1200x650 outer on this desktop), never smaller than 720x480 client.
        //   History: a literal 1024x720 became 1536x1080 DEVICE px once
        //   High-DPI scaling was on; the previous "fix" divided by the screen
        //   ratio to restore the pre-High-DPI PHYSICAL footprint (683x480
        //   logical).  That was wrong twice over: 683 logical is only 53% of
        //   this desktop's work area (responsive sites fell into their
        //   narrow/large-type layout — the "网页 UI 又被调大" complaint), and
        //   resize() sets the CLIENT rect, so the frame still has to fit.
        // The Win11 frame is ~15x37 logical px (measured: window 1047x776 for
        // a 1025x720 client).  Reserve a conservative 16x40 budget so the
        // OUTER window stays inside availableGeometry, and clamp with
        // qMin(lower, max) first — qBound asserts min<=max in debug builds.
        if (QScreen *scr = QGuiApplication::primaryScreen()) {
            const QRect avail = scr->availableGeometry();   // logical px
            const int frameW = 16, frameH = 40;             // logical frame budget
            const int maxW = qMax(320, avail.width() - frameW);
            const int maxH = qMax(240, avail.height() - frameH);
            const int w = qBound(qMin(720, maxW),
                                qMin(1280, qRound(avail.width() * 0.94)), maxW);
            const int h = qBound(qMin(480, maxH),
                                qMin(800, qRound(avail.height() * 0.92)), maxH);
            resize(w, h);
            move(avail.x() + (avail.width() - w - frameW) / 2,
                 avail.y() + (avail.height() - h - frameH) / 2);
        } else {
            resize(1200, 800);
        }
        STARTUP_MARK("chrome build begin");
        buildChrome();
        STARTUP_MARK("chrome built");
        // No status bar: its only content was the memory/CPU/GPU readout,
        // which was removed (it clipped into unreadable text at 150% and the
        // numbers are a diagnostics detail, not browser UI).  The pressure
        // monitor itself keeps running — it still drives cache purges and
        // tab eviction.
        m_memTimer = new QTimer(this);
        connect(m_memTimer, &QTimer::timeout, this, &KHtmlLiteWindow::checkMemoryBudget);

        // F11 fullscreen toggle (Windows browser convention; every major
        // browser maps F11 here).  Registered as a window QAction so it
        // fires regardless of which child widget holds focus.
        QAction *fsAct = new QAction(this);
        fsAct->setShortcut(QKeySequence(Qt::Key_F11));
        connect(fsAct, &QAction::triggered, this, [this]() {
            if (isFullScreen())
                showNormal();      // restore previous maximized/normal state
            else
                showFullScreen();
        });
        addAction(fsAct);

        // Safari tabs close via the red × on hover; QSS cannot draw that icon
        // reliably, so expose the same action on Ctrl+W and the tab context menu.
        QAction *closeTabAct = new QAction(this);
        closeTabAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
        connect(closeTabAct, &QAction::triggered, this, [this] {
            closeTab(currentPageTabIndex());
        });
        addAction(closeTabAct);

        // Cache policy is UI-owned: the kernel exposes PageLoader::clearCache()
        // and this built-in action is the browser UI hook for it (Ctrl+Shift+
        // Del, the browser convention for "clear browsing data").
        QAction *clearAct = new QAction(this);
        clearAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Delete));
        connect(clearAct, &QAction::triggered, this, &KHtmlLiteWindow::clearPageCacheNow);
        addAction(clearAct);
        m_memTimer->start(10000);
        startMemoryPressureWatch();
        m_resizeTimer = new QTimer(this);
        m_resizeTimer->setSingleShot(true);
        connect(m_resizeTimer, &QTimer::timeout, this, &KHtmlLiteWindow::applyPendingResize);
        addTab(start.isEmpty() ? QUrl(QStringLiteral("https://cn.bing.com")) : start);
        STARTUP_MARK("first tab added");
        syncChrome();
        // Extend the client into the caption strip and put the tab/address
        // toolbar on that row, with custom-drawn Win11 caption glyphs.  This
        // replaces the previous DWM "paint native caption text invisible"
        // approach, which left an empty native strip above the toolbar.
        extendClientIntoCaption();
        qApp->installEventFilter(this);
    }

    ~KHtmlLiteWindow() override {
        if (qApp) qApp->removeEventFilter(this);
    }

    // Clear the engine's page-cache mirror on demand.  Settings UIs should call
    // this (or PageLoader::clearCache() directly); the engine does not decide
    // the moment.
    void clearPageCacheNow()
    {
        PageLoader::clearCache();
    }

    // Windows 11 draws the native slightly-rounded frame and caption for this
    // top-level window.  The only supported way to remove the caption *text*
    // while keeping the real WM_NCPAINT buttons/frame is DWMWA_TEXT_COLOR
    // (Windows 11): draw the text in the actual caption-background colour.
    // Do not set an empty window title: GetWindowText is also the Alt+Tab /
    // taskbar identity and all page-title/recovery/crash UI writes rely on it.
    static COLORREF fallbackCaptionBackground()
    {
        // This Qt shell is a light UI and does not opt into
        // DWMWA_USE_IMMERSIVE_DARK_MODE, so its native Win32 caption is the
        // light default even when Windows apps are configured dark.  Use the
        // light native caption colour; refine with the sampled DWM/Mica value
        // once the window is foreground.
        return RGB(243, 243, 243);
    }

    static bool sampleCaptionBackground(HWND hwnd, COLORREF &out)
    {
        RECT wr {};
        POINT clientTop { 0, 0 };
        if (!GetWindowRect(hwnd, &wr) || !ClientToScreen(hwnd, &clientTop))
            return false;
        // Native caption = non-client strip above the client area.  Avoid the
        // app icon/title text at the far left and the native buttons at right.
        const int left = wr.left + 80;
        const int right = wr.right - 220;
        const int top = wr.top + 3;
        const int bottom = clientTop.y - 2;
        if (right - left < 80 || bottom - top < 4)
            return false;

        const int sw = right - left;
        const int sh = bottom - top;
        if (sw < 80 || sh < 4)
            return false;

        // One BitBlt into a DIB section.  Do NOT call GetPixel per sample
        // point: a single caption strip is ~15k pixels, and thousands of
        // synchronous desktop GetPixel calls blocked the UI thread long
        // enough for Windows to mark the browser "Not Responding".
        HDC screenDc = GetDC(nullptr);
        if (!screenDc)
            return false;
        BITMAPINFO bmi {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = sw;
        bmi.bmiHeader.biHeight = -sh;   // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC memDc = CreateCompatibleDC(screenDc);
        bool bltOk = false;
        if (bmp && bits && memDc) {
            HGDIOBJ old = SelectObject(memDc, bmp);
            bltOk = BitBlt(memDc, 0, 0, sw, sh, screenDc, left, top, SRCCOPY) != FALSE;
            SelectObject(memDc, old);
        }
        if (memDc)
            DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        if (!bltOk || !bits) {
            if (bmp)
                DeleteObject(bmp);
            return false;
        }

        struct Bin { unsigned long long n = 0, r = 0, g = 0, b = 0; };
        Bin bins[16 * 16 * 16] = {};
        unsigned long long total = 0;
        const unsigned int *pixels = static_cast<const unsigned int *>(bits);
        for (int y = 0; y < sh; y += 2) {
            for (int x = 0; x < sw; x += 2) {
                const unsigned int c = pixels[y * sw + x];   // BGRA in DIB
                const int b = int(c & 0xFF), g = int((c >> 8) & 0xFF),
                          r = int((c >> 16) & 0xFF);
                Bin &bin = bins[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
                bin.n++;
                bin.r += r;
                bin.g += g;
                bin.b += b;
                total++;
            }
        }
        if (total < 100) {
            if (bmp)
                DeleteObject(bmp);
            return false;
        }
        int best = 0;
        for (int i = 1; i < 16 * 16 * 16; ++i) {
            if (bins[i].n > bins[best].n)
                best = i;
        }
        if (bins[best].n < total / 10) {
            if (bmp)
                DeleteObject(bmp);
            return false;
        }
        out = RGB(static_cast<int>(bins[best].r / bins[best].n),
                  static_cast<int>(bins[best].g / bins[best].n),
                  static_cast<int>(bins[best].b / bins[best].n));
        if (bmp)
            DeleteObject(bmp);
        return true;
    }

    void hideNativeCaptionText()
    {
        if (m_captionTextHidden)
            return;
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (!hwnd)
            return;

        COLORREF bg = fallbackCaptionBackground();
        bool sampled = false;
        if (GetForegroundWindow() == hwnd && !IsIconic(hwnd))
            sampled = sampleCaptionBackground(hwnd, bg);

        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (!dwm)
            return;
        typedef HRESULT (WINAPI *SetWindowAttr)(HWND, DWORD, LPCVOID, DWORD);
        SetWindowAttr fn = reinterpret_cast<SetWindowAttr>(
            GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (!fn) {
            FreeLibrary(dwm);
            return;
        }
        // Always lock the caption fill to the same colour as the text.  This
        // is required for a truly invisible title (a text colour sampled from
        // the Mica gradient can leave a faint ghost where the gradient
        // differs), while native buttons/icons/frame and all hit-testing stay
        // native.  It intentionally gives up the active/inactive Mica tint.
        fn(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &bg, sizeof(bg));
        const HRESULT hr = fn(hwnd, 36 /* DWMWA_TEXT_COLOR */, &bg, sizeof(bg));
        FreeLibrary(dwm);
        if (SUCCEEDED(hr)) {
            m_captionTextHidden = true;
            m_captionColorSampled = sampled;
            fprintf(stderr, "[ui] native caption text hidden color=#%02X%02X%02X sampled=%d\n",
                    GetRValue(bg), GetGValue(bg), GetBValue(bg), sampled ? 1 : 0);
        } else {
            // Pre-Win11 / non-DWM caption: leave the native text untouched
            // rather than changing the window architecture or clearing the
            // window title (which would break Alt+Tab/taskbar identity).
            fprintf(stderr, "[ui] native caption text hide unsupported hr=0x%08lx\n",
                    static_cast<unsigned long>(hr));
        }
    }

    void hideNativeCaptionIcon()
    {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (!hwnd)
            return;
        if (!m_transparentCaptionIcon) {
            // Monochrome 16x16 fully transparent icon: AND mask all 1,
            // XOR mask all 0.  The native title bar uses ICON_SMALL, so this
            // removes only the caption icon; ICON_BIG (taskbar/Alt+Tab) stays
            // the application's original icon.
            BYTE andMask[32];
            std::memset(andMask, 0xFF, sizeof(andMask));
            BYTE xorMask[32] = {};
            m_transparentCaptionIcon = CreateIcon(
                GetModuleHandleW(nullptr), 16, 16, 1, 1, andMask, xorMask);
        }
        if (m_transparentCaptionIcon) {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(m_transparentCaptionIcon));
        }
    }

    void refineNativeCaptionText()
    {
        if (!m_captionTextHidden || m_captionColorSampled)
            return;
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (!hwnd || GetForegroundWindow() != hwnd || IsIconic(hwnd))
            return;
        COLORREF bg = 0;
        if (!sampleCaptionBackground(hwnd, bg))
            return;

        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (!dwm)
            return;
        typedef HRESULT (WINAPI *SetWindowAttr)(HWND, DWORD, LPCVOID, DWORD);
        SetWindowAttr fn = reinterpret_cast<SetWindowAttr>(
            GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (!fn) {
            FreeLibrary(dwm);
            return;
        }
        fn(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &bg, sizeof(bg));
        const HRESULT hr = fn(hwnd, 36 /* DWMWA_TEXT_COLOR */, &bg, sizeof(bg));
        FreeLibrary(dwm);
        if (SUCCEEDED(hr)) {
            m_captionColorSampled = true;
            fprintf(stderr, "[ui] native caption text hidden color=#%02X%02X%02X sampled=1\n",
                    GetRValue(bg), GetGValue(bg), GetBValue(bg));
        }
    }

    enum ChromeGlyph { GlyphBack, GlyphForward, GlyphReload, GlyphShare,
                       GlyphSidebar, GlyphPlus, GlyphSearch, GlyphStop };

    static QIcon chromeGlyph(ChromeGlyph g, const QColor &col = QColor(84, 79, 83))
    {
        QPixmap pm(36, 36);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(2.0, 2.0);
        p.setPen(QPen(col, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        switch (g) {
        case GlyphBack:
            p.drawPolyline(QPolygonF() << QPointF(11.5, 4.5) << QPointF(5.5, 9)
                                       << QPointF(11.5, 13.5));
            break;
        case GlyphForward:
            p.drawPolyline(QPolygonF() << QPointF(6.5, 4.5) << QPointF(12.5, 9)
                                       << QPointF(6.5, 13.5));
            break;
        case GlyphReload:
            p.drawArc(QRectF(4.5, 4.5, 9.0, 9.0), 45 * 16, 285 * 16);
            p.drawLine(QPointF(12.0, 3.2), QPointF(14.0, 5.2));
            p.drawLine(QPointF(14.0, 5.2), QPointF(12.0, 7.2));
            break;
        case GlyphShare:
            p.drawLine(QPointF(9.0, 11.5), QPointF(9.0, 3.5));
            p.drawPolyline(QPolygonF() << QPointF(5.5, 6.8) << QPointF(9.0, 3.3)
                                       << QPointF(12.5, 6.8));
            p.drawRoundedRect(QRectF(4.5, 11.0, 9.0, 4.0), 1.2, 1.2);
            break;
        case GlyphSidebar:
            p.drawRoundedRect(QRectF(3.5, 3.5, 11.0, 11.0), 2.0, 2.0);
            p.drawLine(QPointF(8.3, 3.8), QPointF(8.3, 14.2));
            break;
        case GlyphPlus:
            p.drawLine(QPointF(4.5, 9.0), QPointF(13.5, 9.0));
            p.drawLine(QPointF(9.0, 4.5), QPointF(9.0, 13.5));
            break;
        case GlyphSearch:
            p.drawEllipse(QPointF(8.0, 8.0), 3.6, 3.6);
            p.drawLine(QPointF(10.7, 10.7), QPointF(13.5, 13.5));
            break;
        case GlyphStop:
            p.drawEllipse(QPointF(9.0, 9.0), 6.1, 6.1);
            p.drawLine(QPointF(6.8, 6.8), QPointF(11.2, 11.2));
            p.drawLine(QPointF(11.2, 6.8), QPointF(6.8, 11.2));
            break;
        }
        return QIcon(pm);
    }

    static QPushButton *mkToolButton(QWidget *parent, ChromeGlyph g)
    {
        QPushButton *b = new QPushButton(parent);
        b->setObjectName(QStringLiteral("toolBtn"));
        b->setIcon(chromeGlyph(g));
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(24, 24);
        b->setFlat(true);
        b->setCursor(Qt::ArrowCursor);
        return b;
    }

    static QToolButton *mkGlyphButton(QWidget *parent, ChromeGlyph g)
    {
        QToolButton *b = new QToolButton(parent);
        b->setObjectName(QStringLiteral("glyphBtn"));
        b->setIcon(chromeGlyph(g));
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(22, 22);
        b->setAutoRaise(true);
        b->setCursor(Qt::ArrowCursor);
        return b;
    }

    static QToolButton *mkLight(QWidget *parent, const QColor &c)
    {
        QToolButton *b = new QToolButton(parent);
        b->setObjectName(QStringLiteral("titleLight"));
        b->setFixedSize(12, 12);
        b->setAutoRaise(true);
        b->setCursor(Qt::ArrowCursor);
        b->setStyleSheet(QStringLiteral(
            "QToolButton{background:%1;border-radius:6px;border:1px solid rgba(0,0,0,0.14);}"
            "QToolButton:hover{border-color:rgba(0,0,0,0.38);}")
            .arg(c.name()));
        return b;
    }

    static const QString &safariStyleSheet()
    {
        // Safari-15-style client chrome from the supplied reference: warm
        // light toolbar, restrained light-gray centered address field, thin
        // monochrome glyphs.  The native Windows frame/caption remains outside.
        static const QString qss = QStringLiteral(
            "QWidget#central { background: #F3ECEF; }"
            "QWidget#toolbar { background: #F3ECEF; border-bottom: 1px solid #DCD2D5; }"
            "QStackedWidget#pageStack { background: #ffffff; border: none; }"
            "QPushButton#toolBtn { border: none; background: transparent;"
            "  border-radius: 4px; padding: 0; }"
            "QPushButton#toolBtn:hover { background: #E7DEE1; }"
            "QPushButton#toolBtn:pressed { background: #D8CED2; }"
            "QPushButton#toolBtn:disabled { background: transparent; }"
            "QToolButton#glyphBtn { border: none; background: transparent;"
            "  border-radius: 4px; padding: 0; }"
            "QToolButton#glyphBtn:hover { background: #E7DEE1; }"
            "QToolButton#glyphBtn:pressed { background: #D8CED2; }"
            "#address { background: #E6E0E2; border: 1px solid #D5CDD0;"
            "  border-radius: 7px; padding: 1px 30px 1px 28px; color: #302C2F;"
            "  font-size: 12px; margin: 0; selection-background-color: #B9D7F7; }"
            "#address:focus { border: 1px solid #BFB4B8; background: #E6E0E2; }"
            "QPushButton#crashRetry { background: #ffffff; border: 1px solid #c5c5c5;"
            "  border-radius: 4px; padding: 5px 18px; color: #333; }"
            "QPushButton#crashRetry:hover { background: #f2f2f2; }"
            "QPushButton#crashRetry:pressed { background: #e8e8e8; }");
        return qss;
    }

    void buildChrome()
    {
        // One unified 58 logical top row (87 physical at dpr=1.5): tabs and
        // navigation/address/actions share the same toolbar row, matching the
        // reference's single top bar while keeping the historical page origin.
        QWidget *central = new QWidget(this);
        central->setObjectName(QStringLiteral("central"));
        QVBoxLayout *root = new QVBoxLayout(central);
        root->setContentsMargins(7, 0, 7, 4);
        root->setSpacing(0);

        DragStrip *header = new DragStrip(central);
        header->setObjectName(QStringLiteral("toolbar"));
        header->setFixedHeight(kUnifiedHeaderHeight);
        m_tabStrip = header;
        QHBoxLayout *tb = new QHBoxLayout(header);
        tb->setContentsMargins(8, 0, 8, 0);
        tb->setSpacing(0);

        // Back/forward stay at the same historical left edge; the integrated
        // 220-logical tab strip follows them in the same 58-logical row.  All
        // controls are vertically centered around the historical toolbar
        // center, so the page origin remains (11,87).
        m_back = mkToolButton(header, GlyphBack);
        m_fwd = mkToolButton(header, GlyphForward);
        tb->addWidget(m_back, 0, Qt::AlignVCenter);
        tb->addSpacing(5);
        tb->addWidget(m_fwd, 0, Qt::AlignVCenter);
        tb->addSpacing(8);

        SafariTabBar *bar = new SafariTabBar(header);
        bar->setObjectName(QStringLiteral("safariTabs"));
        bar->setFixedWidth(220);
        bar->setFixedHeight(26);
        bar->setElideMode(Qt::ElideRight);
        bar->onCloseTab = [this](int idx) { closeTab(idx); };
        m_tabBar = bar;
        tb->addWidget(bar, 0, Qt::AlignVCenter);

        tb->addStretch(1);

        m_addr = new AddressEdit(header);
        m_addr->setObjectName(QStringLiteral("address"));
        m_addr->setPlaceholderText(QStringLiteral("搜索或输入网站名称"));
        m_addr->setClearButtonEnabled(false);
        m_addr->setFixedHeight(28);
        m_addr->addAction(chromeGlyph(GlyphSearch, QColor(128, 121, 125)),
                          QLineEdit::LeadingPosition);
        m_reloadAction = m_addr->addAction(chromeGlyph(GlyphReload),
                                           QLineEdit::TrailingPosition);
        m_reloadAction->setToolTip(QStringLiteral("重新加载"));
        tb->addWidget(m_addr, 0, Qt::AlignVCenter);

        tb->addStretch(1);

        // Invisible balance equal to the tab-strip's extra left width keeps
        // the address field centered in the client, not merely centered in the
        // remaining gap.  Left fixed = back/fwd+tab=281.  The right side is
        // balance + share/plus (54) + the 93-logical caption-button reserve.
        QWidget *balance = new QWidget(header);
        balance->setFixedWidth(134);
        tb->addWidget(balance);

        // Right logical group: actual shell functions only.  Share copies the
        // current URL and + creates a new tab.  Sidebar/tab-overview have no
        // implemented action in this codebase, so they are not added.
        QWidget *rightGroup = new QWidget(header);
        rightGroup->setObjectName(QStringLiteral("navRight"));
        QHBoxLayout *rg = new QHBoxLayout(rightGroup);
        rg->setContentsMargins(0, 0, 0, 0);
        rg->setSpacing(5);
        m_shareBtn = mkToolButton(rightGroup, GlyphShare);
        m_shareBtn->setToolTip(QStringLiteral("复制链接"));
        m_newTabBtn = mkGlyphButton(rightGroup, GlyphPlus);
        m_newTabBtn->setFixedSize(24, 24);
        m_newTabBtn->setIconSize(QSize(16, 16));
        m_newTabBtn->setToolTip(QStringLiteral("新标签页"));
        rg->addWidget(m_shareBtn);
        rg->addWidget(m_newTabBtn);
        tb->addWidget(rightGroup, 0, Qt::AlignVCenter);

        // Reserved strip for the custom-drawn Win11 caption buttons.  The
        // buttons themselves are an overlay flush with the window's top-right
        // corner, so they are not clipped by the central widget's 7px inset.
        QWidget *captionSpacer = new QWidget(header);
        captionSpacer->setFixedWidth(kCaptionButtonsWidth);
        tb->addWidget(captionSpacer);

        m_captionButtons = new CaptionButtonsWidget(this);
        m_captionButtons->setFixedSize(kCaptionButtonsWidth, kUnifiedHeaderHeight);
        m_captionButtons->show();
        m_captionButtons->raise();
        root->addWidget(header);

        m_stack = new QStackedWidget(central);
        m_stack->setObjectName(QStringLiteral("pageStack"));
        root->addWidget(m_stack, 1);

        setCentralWidget(central);
        setStyleSheet(safariStyleSheet());
        updateAddressMetrics();

        connect(m_shareBtn, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(m_addr->text());
        });
        connect(m_newTabBtn, &QToolButton::clicked, this,
                [this] { addTab(QUrl(QStringLiteral("https://cn.bing.com"))); });
        connect(m_back, &QPushButton::clicked, this, &KHtmlLiteWindow::goBack);
        connect(m_fwd, &QPushButton::clicked, this, &KHtmlLiteWindow::goForward);
        connect(m_reloadAction, &QAction::triggered, this, [this] {
            TabPage *pg = currentPage();
            if (!pg) return;
            if (pg->loading && pg->render && pg->render->isRunning()) {
                pg->render->stop();
            } else if (pg->crashWidget) {
                retryCrashedTab(pg);
            } else if (pg->render && pg->render->isRunning()) {
                pg->loading = true;
                pg->loadProgress = 0;
                pg->render->reload();
                updateLoadIndicator();
            } else if (pg->suspended) {
                resumeTab(pg);
            }
        });
        connect(m_addr, &QLineEdit::returnPressed, this, &KHtmlLiteWindow::onAddressEntered);
        connect(m_tabBar, &QTabBar::currentChanged, this, [this](int idx) {
            fprintf(stderr, "[tomb] currentChanged idx=%d\n", idx);
            if (m_stack && idx >= 0 && idx < m_stack->count())
                m_stack->setCurrentIndex(idx);
            TabPage *pg = (idx >= 0 && idx < m_pages.size()) ? m_pages.at(idx) : nullptr;
            if (pg) pg->lastActive = QDateTime::currentMSecsSinceEpoch();
            for (TabPage *p2 : m_pages)
                if (p2->render) p2->render->setVisible(p2 == pg);
            if (pg && pg->suspended) resumeTab(pg);
            else {
                syncChrome();
                QTimer::singleShot(0, this, &KHtmlLiteWindow::applyPendingResize);
            }
        });
        connect(m_tabBar, &QTabBar::customContextMenuRequested, this, [this](const QPoint &pos) {
            QMenu menu(this);
            const int idx = m_tabBar->tabAt(pos);
            QAction *closeOne = menu.addAction(QStringLiteral("关闭标签页"));
            QAction *newOne = menu.addAction(QStringLiteral("新标签页"));
            QAction *chosen = menu.exec(m_tabBar->mapToGlobal(pos));
            if (chosen == closeOne) closeTab(idx >= 0 ? idx : currentPageTabIndex());
            else if (chosen == newOne) addTab(QUrl(QStringLiteral("https://cn.bing.com")));
        });
        connect(m_tabBar, &QTabBar::tabMoved, this, [this](int from, int to) {
            if (from == to || from < 0 || to < 0 || from >= m_pages.size() || to >= m_pages.size())
                return;
            TabPage *pg = m_pages.takeAt(from);
            m_pages.insert(to, pg);
            QWidget *w = m_stack->widget(from);
            if (w) {
                m_stack->removeWidget(w);
                m_stack->insertWidget(to, w);
            }
        });
    }

    // ---- page stack <-> Safari tab bar helpers ----
    int currentPageTabIndex() const
    {
        return m_tabBar ? m_tabBar->currentIndex() : -1;
    }

    void addPageTab(QWidget *w, const QString &title)
    {
        m_stack->addWidget(w);
        m_tabBar->addTab(title);
    }

    void insertPageTab(int idx, QWidget *w, const QString &title)
    {
        m_stack->insertWidget(idx, w);
        m_tabBar->insertTab(idx, title);
    }

    void removePageTab(int idx)
    {
        if (!m_stack || idx < 0 || idx >= m_stack->count())
            return;
        QWidget *w = m_stack->widget(idx);
        m_stack->removeWidget(w);
        m_tabBar->removeTab(idx);
    }

    void selectPageTab(int idx)
    {
        m_tabBar->setCurrentIndex(idx);
        if (m_stack && idx >= 0 && idx < m_stack->count())
            m_stack->setCurrentIndex(idx);
    }

    void setPageTabTitle(int idx, const QString &t)
    {
        if (m_tabBar && idx >= 0 && idx < m_tabBar->count())
            m_tabBar->setTabText(idx, t);
    }

    QString pageTabTitle(int idx) const
    {
        return (m_tabBar && idx >= 0 && idx < m_tabBar->count()) ? m_tabBar->tabText(idx) : QString();
    }

    void blockPageTabSignals(bool b)
    {
        if (m_tabBar) m_tabBar->blockSignals(b);
        if (m_stack) m_stack->blockSignals(b);
    }


    QPushButton *mkBtn(QStyle::StandardPixmap sp) {
        return new QPushButton(style()->standardIcon(sp), QString(), this);
    }

    TabPage *currentPage() const {
        const int idx = currentPageTabIndex();
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
        connect(retry, &QPushButton::clicked, this, [this, pg]() { retryCrashedTab(pg); });
        lay->addWidget(icon);
        lay->addWidget(msg);
        lay->addWidget(retry, 0, Qt::AlignCenter);
        return w;
    }

    QWidget *makeRecoveryWidget() {
        // A crashed tab first shows this neutral recovery status, not the final
        // crash page.  The tab is replaced with the live renderer container as
        // soon as the new renderer's socket is ready (RenderTab::ready()).
        QWidget *w = new QWidget;
        QVBoxLayout *lay = new QVBoxLayout(w);
        lay->setAlignment(Qt::AlignCenter);
        QLabel *msg = new QLabel(QStringLiteral("正在恢复页面…"), w);
        msg->setAlignment(Qt::AlignCenter);
        msg->setStyleSheet(QStringLiteral("color:#444;font-size:15px;font-weight:600;"));
        QLabel *sub = new QLabel(QStringLiteral("正在重新创建渲染进程并重载当前页面"), w);
        sub->setAlignment(Qt::AlignCenter);
        sub->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        lay->addWidget(msg);
        lay->addWidget(sub);
        return w;
    }

    // Replace the visible tab widget with the fresh renderer container once
    // the new renderer's IPC is ready.  No fixed delay is used: readiness is
    // the real signal.
    void finishRecovery(TabPage *pg) {
        if (!pg || !m_pages.contains(pg) || !pg->recoveryWidget || !pg->render)
            return;
        const int idx = m_pages.indexOf(pg);
        if (idx < 0 || !m_stack || idx >= m_stack->count()
                || m_stack->widget(idx) != pg->recoveryWidget)
            return;
        QWidget *container = pg->render->container();
        if (!container) return;
        const bool wasCurrent = (currentPage() == pg);
        QWidget *old = pg->recoveryWidget;
        pg->recoveryWidget = nullptr;
        blockPageTabSignals(true);
        removePageTab(idx);
        insertPageTab(idx, container,
                      pg->savedTitle.isEmpty() ? pg->current.host() : pg->savedTitle);
        blockPageTabSignals(false);
        if (old) old->deleteLater();
        if (wasCurrent) {
            selectPageTab(idx);
            m_addr->setText(pg->current.toString());
            updateNavButtons();
        } else {
            syncChrome();
        }
    }

    // Start a recovery attempt.  oldWidget is the currently displayed tab
    // widget (renderer container / crash page / previous recovery placeholder);
    // the caller must already have detached the old renderer.  clearCache only
    // removes PageLoader's regenerable HTTP/page mirrors — never cookies,
    // passwords, history or persistent site storage.
    void startRecoveryRetry(TabPage *pg, QWidget *oldWidget, const QString &title,
                            bool clearCache, bool jsDisabled) {
        const int idx = m_pages.indexOf(pg);
        if (!pg || idx < 0) return;
        const bool wasCurrent = (currentPage() == pg);
        if (!title.isEmpty()) pg->savedTitle = title;
        if (clearCache && !pg->cacheCleanupRetried) {
            fprintf(stderr, "[recovery] clear regenerable page/HTTP cache for tab %d\n", idx);
            PageLoader::clearCache();
            pg->cacheCleanupRetried = true;
        }
        pg->jsDisabled = jsDisabled;
        fprintf(stderr, "[recovery] restart tab=%d jsDisabled=%d clearCache=%d\n",
                idx, (int)jsDisabled, (int)clearCache);
        pg->recoveryWidget = makeRecoveryWidget();
        blockPageTabSignals(true);
        removePageTab(idx);
        insertPageTab(idx, pg->recoveryWidget, QStringLiteral("正在恢复…"));
        blockPageTabSignals(false);
        // Insert the placeholder first, then start the renderer so a ready()
        // arriving immediately can always find the displayed recovery widget.
        startRenderer(pg, pg->current);
        if (oldWidget) oldWidget->deleteLater();
        if (wasCurrent) {
            selectPageTab(idx);
            setWindowTitle(QStringLiteral("正在恢复… - KHtmlLite"));
        } else {
            syncChrome();
        }
    }

    void addTab(const QUrl &url) {
        TabPage *pg = new TabPage;
        pg->current = url;
        pg->lastActive = QDateTime::currentMSecsSinceEpoch();
        startRenderer(pg, url);
        addPageTab(pg->render ? pg->render->container() : new QWidget,
                       url.isEmpty() ? QStringLiteral("新标签页") : url.host());
        m_pages.append(pg);
        selectPageTab(m_pages.size() - 1);
        QTimer::singleShot(0, this, [this, pg]() {
            if (!pg || !pg->render || !pg->render->isRunning())
                return;
            QWidget *container = pg->render->container();
            if (!container)
                return;
            const QSize s = container->size();
            // Logical (CSS px) viewport: RenderTab::resize attaches the dpr.
            pg->render->resize(s.width(), s.height());
        });
    }

    // Drop a renderer without deleting it synchronously from its own
    // crashed()/finished() signal.  terminate() disconnects the QProcess
    // signals and kills the process; deleteLater keeps the QObject alive until
    // the current signal emission has unwound.
    void dropRenderer(TabPage *pg) {
        if (!pg || !pg->render) return;
        RenderTab *old = pg->render;
        pg->render = nullptr;
        old->terminate();
        old->deleteLater();
    }

    // Bug 1: a crashed tab is NOT a suspended tab.  resumeTab() intentionally
    // refuses !suspended tabs, so the crash widget must use this separate path.
    // It replaces the crash placeholder with a fresh renderer at the same tab
    // index, restores the current URL, and re-enables JS (a manual retry is a
    // new user action, not the one-shot automatic JS-disabled retry).
    void retryCrashedTab(TabPage *pg) {
        if (!pg || pg->suspended) return;
        const int idx = m_pages.indexOf(pg);
        if (idx < 0) return;
        // A new renderer is already starting: ignore duplicate clicks/stale
        // signals from the old crash widget.
        if (pg->render && pg->render->isRunning()) return;
        const QUrl url = pg->current;
        QString title = pg->savedTitle;
        if (title.isEmpty()) title = pageTabTitle(idx);
        if (title.isEmpty()) title = url.host();
        QWidget *oldWidget = pg->crashWidget;
        pg->crashWidget = nullptr;
        dropRenderer(pg);
        // Manual retry is a new user action: JS is enabled again.  Crash count
        // stays >=2 so a subsequent failure shows the error page again rather
        // than entering another automatic restart loop.
        pg->jsDisabled = false;
        if (pg->crashCount < 2) pg->crashCount = 2;
        startRecoveryRetry(pg, oldWidget, title, false, false);
    }

    void startRenderer(TabPage *pg, const QUrl &url) {
        pg->render = new RenderTab(this);
        connect(pg->render, &RenderTab::firstFrame, this, [this, pg]() {
            finishRecovery(pg);
        });
        connect(pg->render, &RenderTab::loadStarted, this, [this, pg]() {
            pg->loading = true;
            pg->loadProgress = 0;
            if (pg == currentPage()) updateLoadIndicator();
        });
        connect(pg->render, &RenderTab::loadProgress, this, [this, pg](int percent) {
            pg->loading = true;
            pg->loadProgress = qMax(pg->loadProgress, qBound(0, percent, 100));
            if (pg == currentPage()) updateLoadIndicator();
        });
        connect(pg->render, &RenderTab::loadFinished, this, [this, pg]() {
            pg->loading = false;
            pg->loadProgress = 100;
            if (pg == currentPage()) updateLoadIndicator();
        });
        connect(pg->render, &RenderTab::titleChanged, this, [this, pg](const QString &t) {
            fprintf(stderr, "[tomb] titleChanged title=%s cur=%d\n",
                    t.toUtf8().constData(), pg == currentPage());
            const int idx = m_pages.indexOf(pg);
            if (idx >= 0 && !t.isEmpty()) setPageTabTitle(idx, t.left(30));
            if (pg == currentPage())
                setWindowTitle(t.isEmpty() ? QStringLiteral("KHtmlLite")
                                           : QStringLiteral("%1 - KHtmlLite").arg(t));
        });
        connect(pg->render, &RenderTab::urlChanged, this, [this, pg](const QUrl &u) {
            // The renderer reports the final URL after HTTP redirects. Update
            // the tab's current URL and the address bar so the user sees the
            // real document location (e.g. bing.com → cn.bing.com).
            // Defence in depth: a PageLoader mirror path (file://…/khtml_cache/…)
            // is an internal prefetch copy, never a document location.  The
            // renderer already maps clicks back; never surface it here.
            if (u.isLocalFile() && u.toLocalFile().contains(QLatin1String("/khtml_cache/")))
                return;
            // Link clicks never went through navigate(pg, url, pushHistory):
            // the renderer reports the final URL here.  Without this push the
            // Back button had no history entry for clicked links.  Redirects
            // are included once (the loader only reports the final URL);
            // address-bar navigations already pushed in navigate(), hence
            // navPushed.
            if (u != pg->current && pg->current.isValid() && !pg->navPushed) {
                pg->back.append(pg->current);
                pg->fwd.clear();
            }
            pg->navPushed = false;
            pg->current = u;
            if (pg == currentPage()) {
                m_addr->setText(u.toString());
                updateNavButtons();
            }
        });
        connect(pg->render, &RenderTab::crashed, this, [this, pg]() {
            const int idx = m_pages.indexOf(pg);
            if (idx < 0) return;
            pg->loading = false;
            // Capture before detaching the renderer; also the tab may be in the
            // background, so preserve selection exactly as it was.
            const bool wasCurrent = (currentPage() == pg);
            const QString title = pg->render ? pg->render->title() : QString();
            QWidget *oldWidget = pg->recoveryWidget ? pg->recoveryWidget
                                : (pg->render ? pg->render->container() : nullptr);
            dropRenderer(pg);
            pg->crashCount++;
            // Attempt 1: automatic retry with JS disabled.
            if (pg->crashCount == 1 && !pg->jsDisabled) {
                startRecoveryRetry(pg, oldWidget, title, false, true);
                return;
            }
            // Attempt 2: clear only regenerable page/HTTP cache, then one more
            // automatic retry with JS enabled.  This is bounded; cookies,
            // passwords, history and persistent site data are never touched.
            if (pg->crashCount == 2 && !pg->cacheCleanupRetried) {
                startRecoveryRetry(pg, oldWidget, title, true, false);
                return;
            }
            // Repeated failure: final error page with a manual retry button.
            QWidget *oldRecovery = pg->recoveryWidget;
            pg->recoveryWidget = nullptr;
            QWidget *oldCrash = pg->crashWidget;
            pg->crashWidget = nullptr;
            if (oldWidget && oldWidget != oldRecovery && oldWidget != oldCrash)
                oldWidget->deleteLater();
            if (oldRecovery) oldRecovery->deleteLater();
            if (oldCrash) oldCrash->deleteLater();
            if (!title.isEmpty()) pg->savedTitle = title;
            pg->crashWidget = makeCrashWidget(pg);
            blockPageTabSignals(true);
            removePageTab(idx);
            insertPageTab(idx, pg->crashWidget,
                          title.isEmpty() ? QStringLiteral("已崩溃") : title);
            blockPageTabSignals(false);
            setPageTabTitle(idx, QStringLiteral("已崩溃"));
            pg->loading = false;
            if (wasCurrent) {
                selectPageTab(idx);
                updateLoadIndicator();
                setWindowTitle(QStringLiteral("已崩溃 - KHtmlLite"));
            } else {
                syncChrome();
            }
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
        pg->loading = true;
        pg->loadProgress = 0;
        if (pg == currentPage()) updateLoadIndicator();
        pg->render->start(url, 800, 560, pg->jsDisabled);
    }

    void closeTab(int idx) {
        if (idx < 0 || idx >= m_pages.size()) return;
        TabPage *pg = m_pages.takeAt(idx);
        removePageTab(idx);
        if (pg->render) pg->render->terminate();
        if (pg->crashWidget) delete pg->crashWidget;
        if (pg->recoveryWidget) delete pg->recoveryWidget;
        if (pg->tombstoneWidget) delete pg->tombstoneWidget;
        delete pg;
        if (m_pages.isEmpty()) addTab(QUrl(QStringLiteral("https://cn.bing.com")));
        syncChrome();
    }

    // Called by the coalescing resize timer once a page surface has changed
    // size.  ONLY the current tab's renderer is brought to the new size
    // right away; background tabs relayout lazily on activation instead.
    // (WebKit: hidden pages get no layout/display work.  A window resize
    // used to re-layout EVERY open tab's renderer in the same instant —
    // with N tabs that is N full-page relayout/render passes competing
    // for CPU exactly when the visible tab races to deliver its new-size
    // frame, which is the multi-tab resize "re-render" lag.)
    void applyPendingResize() {
        TabPage *cur = currentPage();
        TabPage *pg = cur;
        if (!pg || !pg->render || !pg->render->isRunning())
            return;
        QWidget *container = pg->render->container();
        if (!container)
            return;
        const QSize s = container->size();
        if (s.width() <= 0 || s.height() <= 0)
            return;
        // Logical (CSS px) viewport: RenderTab::resize attaches the dpr.
        pg->render->resize(s.width(), s.height());
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QMainWindow::resizeEvent(event);
        updateAddressMetrics();
        updateCaptionButtonsGeometry();
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
    bool eventFilter(QObject *watched, QEvent *event) override {
        // The extended client contains native Qt child windows that cover the
        // frame edges, so WM_NCHITTEST at the top level never sees edge mouse
        // presses.  Start the system resize from the Qt press instead.
        if (event->type() == QEvent::MouseButtonPress && !isMaximized()
                && !isFullScreen()) {
            auto *me = static_cast<QMouseEvent *>(event);
            QWidget *w = qobject_cast<QWidget *>(watched);
            if (me->button() == Qt::LeftButton && w && w->window() == this) {
                const QPoint p = mapFromGlobal(me->globalPos());
                const int e = 6;
                Qt::Edges edges;
                if (p.x() >= 0 && p.x() < e) edges |= Qt::LeftEdge;
                else if (p.x() < width() && p.x() >= width() - e) edges |= Qt::RightEdge;
                if (p.y() >= 0 && p.y() < e) edges |= Qt::TopEdge;
                else if (p.y() < height() && p.y() >= height() - e) edges |= Qt::BottomEdge;
                if (edges != Qt::Edges() && windowHandle()
                        && windowHandle()->startSystemResize(edges)) {
                    return true;
                }
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override {
        if (eventType == "windows_generic_MSG") {
            MSG *msg = reinterpret_cast<MSG *>(message);
            if (m_captionExtended && msg->message == WM_NCCALCSIZE
                    && msg->wParam == TRUE) {
                // Client covers the whole window, so the address/toolbar can
                // sit in the caption row.  Pinning the client to rcWork while
                // maximized avoids inheriting the invisible resize border.
                if (IsZoomed(msg->hwnd)) {
                    NCCALCSIZE_PARAMS *p =
                            reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
                    MONITORINFO mi = {sizeof(MONITORINFO)};
                    if (GetMonitorInfoW(
                                MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST),
                                &mi)) {
                        p->rgrc[0] = mi.rcWork;
                    }
                }
                *result = 0;
                return true;
            }
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

    void showEvent(QShowEvent *event) override {
        QMainWindow::showEvent(event);
        extendClientIntoCaption();
        updateCaptionButtonsGeometry();
    }

    void extendClientIntoCaption() {
        if (m_captionExtended) return;
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (!hwnd) return;
        // DwmExtendFrameIntoClientArea is loaded dynamically: the host does
        // not otherwise link dwmapi and remains runnable without DWM.
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (!dwm) return;
        using ExtendFn = HRESULT (WINAPI *)(HWND, const MARGINS *);
        ExtendFn extend = reinterpret_cast<ExtendFn>(
                GetProcAddress(dwm, "DwmExtendFrameIntoClientArea"));
        if (!extend) {
            FreeLibrary(dwm);
            return;
        }
        MARGINS margins = {0, 0, kUnifiedHeaderHeight, 0};
        const HRESULT hr = extend(hwnd, &margins);
        FreeLibrary(dwm);
        if (FAILED(hr)) return;
        m_captionExtended = true;
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        updateCaptionButtonsGeometry();
    }

    void updateCaptionButtonsGeometry() {
        if (!m_captionButtons) return;
        m_captionButtons->setGeometry(width() - kCaptionButtonsWidth, 0,
                                      kCaptionButtonsWidth, kUnifiedHeaderHeight);
        m_captionButtons->raise();
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
        pg->navPushed = pushHistory;
        pg->current = url;
        pg->lastActive = QDateTime::currentMSecsSinceEpoch();
        pg->loading = true;
        pg->loadProgress = 0;
        if (pg->suspended) { resumeTab(pg); return; }
        if (pg->render && pg->render->isRunning()) pg->render->navigate(url);
        else { startRenderer(pg, url); }
        m_addr->setText(url.toString());
        updateNavButtons();
        if (pg == currentPage()) updateLoadIndicator();
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

    void updateAddressMetrics() {
        if (!m_addr) return;
        // Reference: address width / window width = 497 / 1218 = 40.8%.
        // Keep a sane minimum at the narrow-window limit; the two equal side
        // groups + equal stretches keep the field centered in the client.
        const int target = qRound(width() * 0.408);
        m_addr->setFixedWidth(qBound(360, target, 760));
    }

    void updateLoadIndicator() {
        TabPage *pg = currentPage();
        const bool loading = pg && pg->loading;
        if (m_addr)
            loading ? m_addr->setLoadProgress(pg->loadProgress)
                    : m_addr->clearLoadProgress();
        if (m_reloadAction) {
            m_reloadAction->setIcon(chromeGlyph(loading ? GlyphStop : GlyphReload));
            m_reloadAction->setToolTip(loading ? QStringLiteral("停止加载")
                                               : QStringLiteral("重新加载"));
        }
    }

    void syncChrome() {
        TabPage *pg = currentPage();
        if (!pg) return;
        m_addr->setText(pg->current.isValid() ? pg->current.toString() : QString());
        updateNavButtons();
        updateLoadIndicator();
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
        pg->loading = false;
        if (pg == currentPage()) updateLoadIndicator();
        fprintf(stderr, "[tomb] suspendTab idx=%d\n", m_pages.indexOf(pg));
        pg->savedTitle = pageTabTitle(m_pages.indexOf(pg));
        if (pg->savedTitle.isEmpty()) pg->savedTitle = pg->current.host();
        // Snapshot BEFORE killing the renderer: keep the page's last pixels.
        if (pg->render) pg->snapshot = pg->render->canvasImage();
        // Kill the renderer process — this frees ALL engine memory (KHTML DLL,
        // JS heap, caches). Only URL + title + snapshot remain.
        if (pg->render) { pg->render->terminate(); pg->render = nullptr; }
        if (pg->crashWidget) { delete pg->crashWidget; pg->crashWidget = nullptr; }
        if (pg->recoveryWidget) { delete pg->recoveryWidget; pg->recoveryWidget = nullptr; }
        pg->suspended = true;
        pg->lastActive = QDateTime::currentMSecsSinceEpoch();
        const int idx = m_pages.indexOf(pg);
        if (idx >= 0) {
            // Tab-bar surgery must not re-route currentChanged onto the
            // tombstone mid-removal: removeTab shifts indices, Qt emits
            // currentChanged for the transient index, and the handler would
            // instantly resumeTab() the tab we just suspended — defeating
            // the tombstone one event after creating it (observed live:
            // suspend idx=0 -> currentChanged idx=0 -> resumeTab idx=0).
            // The current TAB never actually changes here, only its index
            // churns (1 -> 0 -> 1), so suppressing the signal across the
            // surgery is exactly right.
            blockPageTabSignals(true);
            removePageTab(idx);
            pg->tombstoneWidget = new TombstoneWidget(pg->snapshot, pg->savedTitle);
            insertPageTab(idx, pg->tombstoneWidget,
                              pg->savedTitle + QStringLiteral(" [挂起]"));
            blockPageTabSignals(false);
        }
    }

    void resumeTab(TabPage *pg) {
        if (!pg || !pg->suspended) return;
        fprintf(stderr, "[tomb] resumeTab idx=%d\n", m_pages.indexOf(pg));
        const int idx = m_pages.indexOf(pg);
        // Mark not-suspended BEFORE any tab-bar surgery: a currentChanged
        // emitted mid-surgery must not re-enter resumeTab.  Re-entrancy was
        // observed live (insertTab fired currentChanged while suspended was
        // still true -> second resumeTab -> double removeTab/insertTab ->
        // m_pages<->m_tabs index desync -> a live renderer's widget got
        // removed and its process crash-recovered with JS disabled).
        pg->suspended = false;
        if (pg->render) { delete pg->render; pg->render = nullptr; }
        if (pg->crashWidget) { delete pg->crashWidget; pg->crashWidget = nullptr; }
        if (pg->recoveryWidget) { delete pg->recoveryWidget; pg->recoveryWidget = nullptr; }
        startRenderer(pg, pg->current);
        // Restore tombstone scroll + zoom. Sent when the renderer socket
        // connects; applied by the renderer after the document completes.
        if (pg->scrollX != 0 || pg->scrollY != 0 || pg->zoom != 100)
            pg->render->setRestoreState(pg->scrollX, pg->scrollY, pg->zoom);
        // Visual continuity while the new renderer loads: seed the surface
        // with the tombstone snapshot until real tiles arrive.
        pg->render->seedFrame(pg->snapshot);
        if (idx >= 0) {
            // Same suppression rule as suspendTab: the surgery must not
            // re-route currentChanged onto the half-resumed tab.  The
            // deliberate setCurrentIndex afterwards fires the handler
            // exactly once with the final, consistent state.
            blockPageTabSignals(true);
            removePageTab(idx);
            if (pg->tombstoneWidget) { delete pg->tombstoneWidget; pg->tombstoneWidget = nullptr; }
            insertPageTab(idx, pg->render->container() ? pg->render->container() : new QWidget, pg->savedTitle);
            blockPageTabSignals(false);
            selectPageTab(idx);
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

    // === Load-adaptive tab eviction (Safari: MemoryPressureHandler) ===
    // Tombstoning follows SYSTEM load, never our own absolute footprint
    // (a 400 MB browser is harmless on an idle 32 GB machine and fatal on
    // a 4 GB one).  Two independent signals, mirroring WebKit's
    // MemoryPressureHandler design on Windows:
    //
    //  1. Memory pressure LEVELS, polled with the 10 s watchdog timer
    //     (GlobalMemoryStatusEx.dwMemoryLoad):
    //       normal  : < 70%      -> nothing
    //       pressure: 70-85%     -> purge disk cache
    //       warning : 85-93%     -> purge + suspend LRU background tab
    //       critical: >= 93%     -> purge + suspend ALL background tabs now
    //  2. The kernel's LowMemoryResourceNotification (same API WebKit's
    //     Windows MemoryPressureHandler waits on): a dedicated thread
    //     blocks on the event at zero CPU and escalates a check
    //     immediately, so we react when the SYSTEM declares pressure
    //     instead of waiting out the poll interval (see
    //     startMemoryPressureWatch).
    //
    // CPU-adaptive pillar: system CPU load (GetSystemTimes, whole machine)
    // modulates how aggressively idle tabs are reclaimed — under sustained
    // CPU pressure the idle timeout drops from 5 min to 60 s (Safari's
    // analogue: proactive suspension under thermal pressure).  Per-tab
    // renderer CPU is also sampled every tick and shown in the status bar
    // as a diagnostic (‰ of one core), but it is deliberately NOT an
    // eviction trigger: measured on this architecture a background tab
    // with a continuously animating GIF burns only ~1.6% of one core
    // (frame-clock throttling already enforces "background CPU ~ 0"; a
    // 50%-of-a-core rule would be dead code — tests/tombstone_probe.py).
    void checkMemoryBudget() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 wallMs = m_lastMemTickMs > 0 ? qMax<qint64>(1, now - m_lastMemTickMs)
                                                  : 10000;
        m_lastMemTickMs = now;

        // System CPU load over the last tick (all cores, 0-100); used only for
        // the CPU-adaptive idle policy, not as a RAM/GPU budget.
        const int sysCpuPct = sampleSystemCpuPct();

        auto makeUsage = [&](TabPage *pg) {
            khtml::resource::RendererUsage u;
            u.id = reinterpret_cast<std::uint64_t>(pg);
            u.foreground = (pg == currentPage());
            u.suspended = pg->suspended;
            u.idleMs = pg->lastActive > 0
                    ? std::uint64_t(qMax<qint64>(0, now - pg->lastActive)) : 0;
            if (!pg->suspended && pg->render) {
                const qint64 kb = qMax<qint64>(0, pg->render->memoryKB());
                u.workingSetBytes = std::uint64_t(kb) * 1024;
            }
            return u;
        };

        std::vector<khtml::resource::RendererUsage> usage;
        usage.reserve(size_t(m_pages.size()));
        const qint64 browserKB = currentMemKB();
        qint64 rendererKB = 0;
        int hottestPerMill = 0;
        for (TabPage *pg : m_pages) {
            if (!pg) continue;
            khtml::resource::RendererUsage u = makeUsage(pg);
            if (!pg->suspended) {
                rendererKB += qint64(u.workingSetBytes / 1024);
                if (pg->render) {
                    const qint64 cpu = pg->render->cpuTimeMs();
                    const qint64 dCpu = qMax<qint64>(0, cpu - pg->cpuTimeMs);
                    pg->cpuTimeMs = cpu;
                    pg->cpuPerMill = int(dCpu * 1000 / wallMs);
                    if (pg != currentPage())
                        hottestPerMill = qMax(hottestPerMill, pg->cpuPerMill);
                }
            }
            usage.push_back(u);
        }

        m_resourceMgr.updateRendererUsage(std::move(usage));
        const khtml::resource::ResourceSnapshot snap = m_resourceMgr.sample();
        const khtml::resource::RamSample &ram = snap.ram;
        const khtml::resource::GpuSample &gpu = snap.gpu;
        const khtml::resource::PressureLevel ramPressure = ram.pressure;
        const bool gpuWarning = gpu.pressure != khtml::resource::PressureLevel::Normal;
        const bool gpuCritical = gpu.pressure == khtml::resource::PressureLevel::Critical;
        const int ramLevel = ramPressure == khtml::resource::PressureLevel::Critical ? 3
                           : ramPressure == khtml::resource::PressureLevel::Warning ? 2 : 0;
        const qint64 memKB = browserKB + rendererKB;

        // The manager's cadence recommendation uses the worse of RAM/GPU
        // pressure and includes hysteresis (5s/2s/1s by default).
        const int sampleMs = int(snap.recommendedSampleIntervalMs);
        if (m_memTimer && sampleMs > 0 && m_memTimer->interval() != sampleMs)
            m_memTimer->start(sampleMs);

        static const bool tombDebug = qEnvironmentVariableIsSet("KHTML_TOMBSTONE_DEBUG");
        if (tombDebug) {
            const char *adapter = (gpu.selectedAdapterIndex >= 0
                                   && size_t(gpu.selectedAdapterIndex) < gpu.adapters.size())
                    ? gpu.adapters[size_t(gpu.selectedAdapterIndex)].name.c_str() : "-";
            fprintf(stderr,
                    "[res] ram=%u%% %s browserKB=%lld rendererKB=%lld totalKB=%lld "
                    "gpu=%s pressure=%s vramBudgetMB=%.0f vramUsageMB=%.0f adapter=%s pages=%d\n",
                    unsigned(ram.memoryLoadPercent),
                    khtml::resource::toString(ramPressure),
                    (long long)browserKB, (long long)rendererKB, (long long)memKB,
                    khtml::resource::toString(gpu.status),
                    khtml::resource::toString(gpu.pressure),
                    double(gpu.vramBudgetBytes) / (1024.0 * 1024.0),
                    double(gpu.vramUsageBytes) / (1024.0 * 1024.0),
                    adapter, m_pages.size());
        }

        // Regenerable page/HTTP cache is the first RAM/GPU release step.  The
        // engine's PageLoader owns only downloaded page mirrors; cookies,
        // passwords, history and persistent site storage are not touched.
        if (ramLevel >= 2 || gpuWarning)
            purgeCache();
        if ((ramPressure == khtml::resource::PressureLevel::Critical || gpuCritical)
                && m_lastPageCacheClearMs > 0
                && now - m_lastPageCacheClearMs > 30000) {
            fprintf(stderr, "[res] critical pressure: clear regenerable page/HTTP cache\n");
            PageLoader::clearCache();
            m_lastPageCacheClearMs = now;
        } else if ((ramPressure == khtml::resource::PressureLevel::Critical || gpuCritical)
                   && m_lastPageCacheClearMs == 0) {
            PageLoader::clearCache();
            m_lastPageCacheClearMs = now;
        }

        // Unified policy: soft budget for trim/suspend decisions, and the Job
        // Object commit ceiling (the only true hard limit) for each renderer.
        // The foreground tab is never suspended here.
        for (TabPage *pg : m_pages) {
            if (!pg || pg->suspended || !pg->render) continue;
            const khtml::resource::RendererUsage u = makeUsage(pg);
            const khtml::resource::RendererPolicy pol = m_resourceMgr.policyFor(u);
            if (pol.hardCeilingBytes > 0)
                pg->render->setMemoryCeilingKB(qint64(pol.hardCeilingBytes / 1024));
            if (pg == currentPage()) continue;
            const bool overSoft = pol.softBudgetBytes > 0
                    && u.workingSetBytes > pol.softBudgetBytes;
            const auto advice = m_resourceMgr.adviseBackground(u);
            if (overSoft || advice == khtml::resource::BackgroundAction::Suspend) {
                fprintf(stderr, "[res] suspend background tab (soft=%lldKB used=%lldKB)\n",
                        (long long)(pol.softBudgetBytes / 1024),
                        (long long)(u.workingSetBytes / 1024));
                suspendTab(pg);
            } else if (advice == khtml::resource::BackgroundAction::Trim) {
                purgeCache();
            }
        }

        // Pressure eviction still runs as a final backstop.  RAM and GPU are
        // separate domains; GPU pressure only reclaims renderer/page resources,
        // it never changes a CPU memory ceiling by a VRAM-derived amount.
        // Critical FIRST: gpuWarning includes Critical, and the old ordering
        // made the all-background branch unreachable.
        if (ramLevel >= 3 || gpuCritical) {
            for (TabPage *pg : m_pages) {
                if (pg != currentPage() && !pg->suspended)
                    suspendTab(pg);
            }
        } else if (ramLevel == 2 || gpuWarning) {
            TabPage *victim = nullptr;
            for (TabPage *pg : m_pages) {
                if (pg == currentPage() || pg->suspended)
                    continue;
                if (!victim || pg->lastActive < victim->lastActive)
                    victim = pg;
            }
            if (victim) suspendTab(victim);
        }

        // CPU-modulated idle eviction (unchanged behaviour): idle tabs are
        // discarded after 5 min on a relaxed machine, 60 s under CPU saturation.
        const qint64 idleMs = sysCpuPct >= 85 ? 60 * 1000 : 5 * 60 * 1000;
        for (TabPage *pg : m_pages) {
            if (pg != currentPage() && !pg->suspended && pg->lastActive > 0
                && (now - pg->lastActive) > idleMs)
                suspendTab(pg);
        }
        Q_UNUSED(hottestPerMill);
    }

    // Whole-system CPU utilization between watchdog ticks (0-100).
    // GetSystemTimes needs no privileges and costs nothing at 10 s
    // cadence; the first call only seeds the counters (returns 0).
    int sampleSystemCpuPct() {
        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user))
            return 0;
        const auto to64 = [](const FILETIME &ft) {
            return (qint64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        const qint64 i = to64(idle), k = to64(kernel), u = to64(user);
        if (m_lastIdleTime < 0) {   // first tick: seed only
            m_lastIdleTime = i; m_lastKernelTime = k; m_lastUserTime = u;
            return 0;
        }
        const qint64 dIdle = i - m_lastIdleTime;
        const qint64 dKernel = k - m_lastKernelTime;
        const qint64 dUser = u - m_lastUserTime;
        m_lastIdleTime = i; m_lastKernelTime = k; m_lastUserTime = u;
        const qint64 total = dKernel + dUser;   // kernel includes idle
        if (total <= 0)
            return 0;
        const qint64 busy = qMax<qint64>(0, total - dIdle);
        return int(busy * 100 / total);
    }


    // Whole-machine GPU utilization (0-100) from PDH GPU Engine counters —
    // the same data source Task Manager graphs.  Instances are per-engine
    // (3D, copy, video decode...); machine utilization is their sum,
    // clamped.  First call only opens the query; utilization needs two
    // collects, so the first tick reports 0.  On machines with no PDH
    // GPU counters (basic display drivers) this degrades to a flat 0 —
    // honest absence, never a fake number.
    int sampleGpuPct()
    {
        if (m_gpuUnavailable)
            return 0;                       // PDH GPU unavailable on this box
        if (!m_gpuQuery) {
            PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &m_gpuQuery);
            if (st != ERROR_SUCCESS) {
                m_gpuUnavailable = true;
                return 0;
            }
            st = PdhAddEnglishCounterW(m_gpuQuery,
                L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_gpuCounter);
            if (st != ERROR_SUCCESS) {
                PdhCloseQuery(m_gpuQuery);
                m_gpuQuery = nullptr;
                m_gpuCounter = nullptr;
                m_gpuUnavailable = true;
                return 0;
            }
        }
        if (PdhCollectQueryData(m_gpuQuery) != ERROR_SUCCESS)
            return m_lastGpuPct;
        DWORD bufSize = 0, count = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(m_gpuCounter, PDH_FMT_LARGE,
                                                    &bufSize, &count, nullptr);
        if (st == PDH_MORE_DATA && bufSize > 0) {
            QByteArray buf(int(bufSize), Qt::Uninitialized);
            auto *items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
            st = PdhGetFormattedCounterArrayW(m_gpuCounter, PDH_FMT_LARGE,
                                              &bufSize, &count, items);
            if (st == ERROR_SUCCESS && count > 0) {
                qint64 sum = 0;
                for (DWORD i = 0; i < count; ++i)
                    if (items[i].FmtValue.CStatus == ERROR_SUCCESS)
                        sum += items[i].FmtValue.largeValue;
                m_lastGpuPct = int(qMin<qint64>(100, sum));
            }
        }
        return m_lastGpuPct;
    }

    // Kernel low-memory event -> immediate pressure check (WebKit's
    // MemoryPressureHandler mechanism on Windows).  The thread spends its
    // whole life blocked in WaitForSingleObject: 0% CPU until the kernel
    // signals real system pressure, at which point the check runs on the
    // UI thread within one event-loop turn instead of at the next 10 s
    // poll.  The handle is never closed before process exit by design —
    // the wait thread dies with the process, exactly like WebKit's.
    void startMemoryPressureWatch() {
        m_lowMemEvent = CreateMemoryResourceNotification(LowMemoryResourceNotification);
        if (!m_lowMemEvent)
            return;  // pre-Vista? poll-only mode
        QThread *watch = QThread::create([this]() {
            // Boot grace: on an already-pressured system the kernel event
            // is signaled at creation, so an immediate wait would escalate
            // a cache purge while the first navigation's subresources are
            // still being fetched — the PageLoader re-feed then reads the
            // very files the purge just deleted and the page never heals
            // (observed live: bg_smoke stuck red-less for 18 s).  The
            // 10 s poll already covers steady-state reclaim; the event
            // path is for reacting to NEW pressure, which by definition
            // arrives after the first load has settled.
            QThread::sleep(15);
            for (;;) {
                if (WaitForSingleObject(m_lowMemEvent, INFINITE) != WAIT_OBJECT_0)
                    return;
                QMetaObject::invokeMethod(this, [this]() {
                    checkMemoryBudget();
                }, Qt::QueuedConnection);
                QThread::msleep(1000);  // batch repeated signals under sustained pressure
            }
        });
        watch->setObjectName(QStringLiteral("memory-pressure-watch"));
        watch->start(QThread::LowPriority);
    }

    QTimer *m_memTimer = nullptr;
    QTimer *m_resizeTimer = nullptr;
    bool m_liveResize = false;
    bool m_purgingCache = false;
    // Load-adaptive tombstone state: kernel low-memory event handle (see
    // startMemoryPressureWatch), the wall clock of the previous watchdog
    // tick (for per-tab CPU deltas), and the GetSystemTimes counters for
    // whole-system CPU load (negative = not seeded yet).
    HANDLE m_lowMemEvent = nullptr;
    qint64 m_lastMemTickMs = 0;
    // GPU monitor (PDH).  m_gpuUnavailable marks "no GPU counters on this
    // box" so the sampler stops retrying a path that can never exist.
    khtml::resource::ResourceManager m_resourceMgr;
    qint64 m_lastPageCacheClearMs = 0;
    PDH_HQUERY m_gpuQuery = nullptr;
    PDH_HCOUNTER m_gpuCounter = nullptr;
    bool m_gpuUnavailable = false;
    int m_lastGpuPct = 0;
    qint64 m_lastIdleTime = -1, m_lastKernelTime = 0, m_lastUserTime = 0;
    QTabBar *m_tabBar = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_tabStrip = nullptr;
    QToolButton *m_newTabBtn = nullptr;
    QPushButton *m_shareBtn = nullptr;
    bool m_captionTextHidden = false;
    bool m_captionColorSampled = false;
    HICON m_transparentCaptionIcon = nullptr;
    QList<TabPage *> m_pages;
    AddressEdit *m_addr = nullptr;
    QAction *m_reloadAction = nullptr;
    CaptionButtonsWidget *m_captionButtons = nullptr;
    bool m_captionExtended = false;
    QPushButton *m_back = nullptr, *m_fwd = nullptr;
};

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    STARTUP_MARK("main entry");
    // High-DPI clarity (Safari deviceScaleFactor model): Qt reasons in
    // device-independent pixels; the page pipeline keeps a CSS-px viewport in
    // the renderer and rasters it at the device ratio onto a physical frame
    // (PageSurface ships the LOGICAL size and composites the physical frame
    // 1:1 on the device grid), so chrome and page content stay crisp and
    // correctly sized at 125/150%.
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    // Exact fractional scaling.  Qt's default rounding policy snaps a 150%
    // display to a device pixel ratio of 2.0, so the whole chrome (and, since
    // deviceScaleFactor, the page rasters too) is laid out at 2x instead of
    // the OS's 1.5x — every window and every glyph ends up 33% larger than
    // the user asked for, and a 1024x720 default window becomes 1280x960
    // device pixels.  PassThrough keeps the real ratio (e.g. 1.5) end to end.
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("KHtmlLite"));
    app.setOrganizationName(QStringLiteral("KHtmlLite"));
    STARTUP_MARK("QApplication ready");
    const QString caPath = QCoreApplication::applicationDirPath()
                           + QStringLiteral("/ssl-ca-bundle.crt");
    if (QFile::exists(caPath))
        qputenv("SSL_CERT_FILE", QDir::toNativeSeparators(caPath).toUtf8());
    // Engine cache-clearing interface for UI callers.  The kernel never
    // clears the page cache on its own (Safari/WebKit model: the UI owns the
    // policy).  Programmatic API: PageLoader::clearCache().
    if (QCoreApplication::arguments().contains(QStringLiteral("--clear-cache"))) {
        PageLoader::clearCache();
        return 0;
    }
    QUrl start;
    if (argc > 1) start = QUrl(QString::fromLocal8Bit(argv[1]));
    KHtmlLiteWindow w(start);
    STARTUP_MARK("window constructed");
    w.show();
    STARTUP_MARK("window shown");
    return app.exec();
}

#include "khtml_browser.moc"
