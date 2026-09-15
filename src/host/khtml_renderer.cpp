// khtml_renderer.cpp — out-of-process rendering sandbox.
// Runs one KHTMLPart in a child process; the main shell embeds this window
// and monitors the process so a renderer crash never takes down the browser.
#include <QApplication>
#include <QWidget>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QLocalServer>
#include <QLocalSocket>
#include <QUrl>
#include <QTimer>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QSharedMemory>
#include <QImage>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QRegion>
#include <QScrollBar>
#include <QElapsedTimer>

#include <KHTMLPart>
#include <khtml_part.h>
#include <khtmlview.h>
#include <kparts/part.h>
#include <kparts/browserextension.h>
#include <kparts/openurlarguments.h>

#include "pageloader.h"
#include "perflog.h"

#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>

extern "C" int qjs_get_diag(char *buf, int bufSize);

// ---------------------------------------------------------------------------
// Crash capture: VEH + MiniDump + module resolution.
// Runs before Qt/KHTML so even early crashes are caught. The handler uses only
// kernel32 / dbghelp APIs — no heap, no Qt, no KHTML calls.
// ---------------------------------------------------------------------------
static HANDLE g_crashLog = INVALID_HANDLE_VALUE;
static char   g_crashUrl[1024] = {0};
static char   g_crashDir[MAX_PATH] = {0};

static void ccWrite(const char *s)
{
    if (g_crashLog == INVALID_HANDLE_VALUE || !s) return;
    DWORD n = (DWORD)strlen(s);
    DWORD written;
    WriteFile(g_crashLog, s, n, &written, NULL);
}

static void ccWriteHex(const char *prefix, ULONG64 v)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "%s0x%llx\n", prefix ? prefix : "",
             (unsigned long long)v);
    ccWrite(buf);
}

static void ccWriteDec(const char *prefix, ULONG64 v)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "%s%llu\n", prefix ? prefix : "",
             (unsigned long long)v);
    ccWrite(buf);
}

// Resolve which module an address belongs to. Returns module base via outBase.
static void ccResolveModule(ULONG64 addr, char *outName, int nameCap,
                            ULONG64 *outBase)
{
    outName[0] = '\0';
    *outBase = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi))
        return;
    HMODULE mod = (HMODULE)mbi.AllocationBase;
    *outBase = (ULONG64)(ULONG_PTR)mod;
    if (!GetModuleBaseNameA(GetCurrentProcess(), mod, outName, nameCap))
        strncpy(outName, "<unknown>", nameCap - 1);
}

static LONG CALLBACK ccVehHandler(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    EXCEPTION_RECORD *er = ep->ExceptionRecord;

    // Only capture fatal exceptions. Ignore debug output (0x40010006/0x4001000a),
    // C++ EH (0xE06D7363), and other non-fatal first-chance exceptions.
    DWORD code = er->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */ &&
        code != 0xC000041D /* STATUS_FATAL_APP_EXIT */ &&
        code != 0xC000027B /* STATUS_APPLICATION_HANG */) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT *ctx = ep->ContextRecord;

    ccWrite("===== KHtmlLite renderer crash =====\n");

    SYSTEMTIME st;
    GetLocalTime(&st);
    char tbuf[64];
    snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d.%03d\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
    ccWrite(tbuf);

    ccWriteDec("PID: ", (ULONG64)GetCurrentProcessId());
    ccWriteDec("TID: ", (ULONG64)GetCurrentThreadId());
    ccWrite("URL: "); ccWrite(g_crashUrl[0] ? g_crashUrl : "(none)"); ccWrite("\n");

    ccWriteHex("ExceptionCode: ", (ULONG64)er->ExceptionCode);
    ccWriteHex("ExceptionAddress: ", (ULONG64)(ULONG_PTR)er->ExceptionAddress);
    ccWriteDec("ExceptionFlags: ", (ULONG64)er->ExceptionFlags);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er->NumberParameters >= 2) {
        ULONG64 kind = (ULONG64)er->ExceptionInformation[0];
        ccWrite("AccessType: ");
        if (kind == 0)      ccWrite("READ");
        else if (kind == 1) ccWrite("WRITE");
        else if (kind == 8) ccWrite("EXECUTE");
        else { ccWrite("UNKNOWN("); ccWriteDec("", kind); ccWrite(")"); }
        ccWrite("\n");
        ccWriteHex("AccessTarget: ", (ULONG64)er->ExceptionInformation[1]);
    }

    // Resolve faulting module
    char modName[256] = {0};
    ULONG64 modBase = 0;
    ccResolveModule((ULONG64)(ULONG_PTR)er->ExceptionAddress, modName,
                    sizeof(modName), &modBase);
    ccWrite("FaultingModule: "); ccWrite(modName); ccWrite("\n");
    ccWriteHex("ModuleBase: ", modBase);
    if (modBase) {
        ccWriteHex("RVA: ", (ULONG64)(ULONG_PTR)er->ExceptionAddress - modBase);
    }

    // CPU context
    if (ctx) {
#ifdef _M_X64
        ccWriteHex("RIP: ", (ULONG64)ctx->Rip);
        ccWriteHex("RSP: ", (ULONG64)ctx->Rsp);
        ccWriteHex("RBP: ", (ULONG64)ctx->Rbp);
        ccWriteHex("RAX: ", (ULONG64)ctx->Rax);
        ccWriteHex("RCX: ", (ULONG64)ctx->Rcx);
        ccWriteHex("RDX: ", (ULONG64)ctx->Rdx);
        ccWriteHex("R8:  ", (ULONG64)ctx->R8);
        ccWriteHex("R9:  ", (ULONG64)ctx->R9);
        ccWriteHex("R10: ", (ULONG64)ctx->R10);
        ccWriteHex("R11: ", (ULONG64)ctx->R11);
#else
        ccWriteHex("EIP: ", (ULONG64)ctx->Eip);
        ccWriteHex("ESP: ", (ULONG64)ctx->Esp);
        ccWriteHex("EBP: ", (ULONG64)ctx->Ebp);
#endif
    }

    // Resolve modules for RIP, RSP, RBP and the access target
    if (ctx) {
#ifdef _M_X64
        char nm[256]; ULONG64 mb;
        ccResolveModule((ULONG64)ctx->Rip, nm, sizeof(nm), &mb);
        ccWrite("RIP module: "); ccWrite(nm);
        if (mb) { ccWrite("+"); ccWriteHex("", (ULONG64)ctx->Rip - mb); }
        ccWrite("\n");
#endif
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er->NumberParameters >= 2) {
        char nm[256]; ULONG64 mb;
        ccResolveModule((ULONG64)er->ExceptionInformation[1], nm, sizeof(nm), &mb);
        ccWrite("AccessTarget module: "); ccWrite(nm);
        if (mb) { ccWrite("+"); ccWriteHex("", (ULONG64)er->ExceptionInformation[1] - mb); }
        ccWrite("\n");
    }

    // --- QuickJS prop corruption diagnostic ---
    {
        static char diagBuf[4096];
        int n = qjs_get_diag(diagBuf, sizeof(diagBuf));
        if (n > 0) {
            diagBuf[n] = 0;
            ccWrite(diagBuf);
        }
    }

    // --- MiniDump ---
    char dumpPath[MAX_PATH];
    SYSTEMTIME dst;
    GetLocalTime(&dst);
    snprintf(dumpPath, sizeof(dumpPath),
             "%s\\khtml_renderer_%04d%02d%02d_%02d%02d%02d_%lu.dmp",
             g_crashDir,
             dst.wYear, dst.wMonth, dst.wDay,
             dst.wHour, dst.wMinute, dst.wSecond,
             (unsigned long)GetCurrentProcessId());

    HANDLE hDump = CreateFileA(dumpPath, GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MINIDUMP_TYPE mdt = (MINIDUMP_TYPE)(
            MiniDumpWithDataSegs |
            MiniDumpWithUnloadedModules |
            MiniDumpWithProcessThreadData |
            MiniDumpWithIndirectlyReferencedMemory |
            MiniDumpWithTokenInformation);
        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                    hDump, mdt, &mei, NULL, NULL);
        ccWrite("MiniDump: ");
        ccWrite(ok ? "OK " : "FAIL ");
        ccWrite(dumpPath);
        ccWrite("\n");
        CloseHandle(hDump);
    } else {
        ccWrite("MiniDump: CreateFile FAILED\n");
    }

    ccWrite("===== end crash report =====\n\n");
    FlushFileBuffers(g_crashLog);

    // Let the normal unhandled-exception path run so the parent still detects
    // the crash via process exit.
    return EXCEPTION_CONTINUE_SEARCH;
}

static void ccInit(const char *dumpDir)
{
    strncpy(g_crashDir, dumpDir ? dumpDir : ".", MAX_PATH - 1);

    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%s\\khtml_renderer_crash.log", g_crashDir);
    g_crashLog = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_crashLog == INVALID_HANDLE_VALUE) {
        // Fallback: write to current directory
        g_crashLog = CreateFileA("khtml_renderer_crash.log", FILE_APPEND_DATA,
                                 FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
    }

    AddVectoredExceptionHandler(1, ccVehHandler);
}

static void ccSetUrl(const char *url)
{
    if (!url) { g_crashUrl[0] = '\0'; return; }
    strncpy(g_crashUrl, url, sizeof(g_crashUrl) - 1);
    g_crashUrl[sizeof(g_crashUrl) - 1] = '\0';
}

// Same EngineView as the shell: intercepts http(s) navigation so PageLoader
// (which lives in this process too) can fetch and cache the page.
class RendererView : public KHTMLPart
{
    Q_OBJECT
public:
    explicit RendererView(bool jsEnabled, QWidget *parent = nullptr) : KHTMLPart(parent)
    {
        setJScriptEnabled(jsEnabled);
        setJavaEnabled(false);
        setPluginsEnabled(false);
        setEncoding(QStringLiteral("utf-8"), true);
        KParts::BrowserExtension *ext = browserExtension();
        if (ext) {
            connect(ext, QOverload<const QUrl &, const KParts::OpenUrlArguments &,
                                    const KParts::BrowserArguments &>::of(
                        &KParts::BrowserExtension::openUrlRequestDelayed),
                    this, [this](const QUrl &u, const KParts::OpenUrlArguments &,
                                 const KParts::BrowserArguments &) { emit navigateRequested(u); });
            connect(ext, QOverload<const QUrl &, const KParts::OpenUrlArguments &,
                                    const KParts::BrowserArguments &, const KParts::WindowArgs &,
                                    KParts::ReadOnlyPart **>::of(
                        &KParts::BrowserExtension::createNewWindow),
                    this, [this](const QUrl &u, const KParts::OpenUrlArguments &,
                                 const KParts::BrowserArguments &, const KParts::WindowArgs &,
                                 KParts::ReadOnlyPart **) { emit openNewTabRequested(u); });
        }
    }
    bool loadUrl(const QUrl &url) { return openUrl(url); }
Q_SIGNALS:
    void navigateRequested(const QUrl &url);
    void openNewTabRequested(const QUrl &url);
protected:
    bool openUrl(const QUrl &url) override
    {
        const QString s = url.scheme().toLower();
        if (s == QLatin1String("http") || s == QLatin1String("https")) {
            emit navigateRequested(url);
            return true;
        }
        return KHTMLPart::openUrl(url);
    }
};

class RendererHost : public QObject
{
    Q_OBJECT
public:
    explicit RendererHost(const QString &socketName, bool jsEnabled, QObject *parent = nullptr)
        : QObject(parent), m_loader(new PageLoader(this)), m_socketName(socketName)
    {
        m_server = new QLocalServer(this);
        if (!m_server->listen(socketName)) {
            QLocalServer::removeServer(socketName);
            m_server->listen(socketName);
        }
        connect(m_server, &QLocalServer::newConnection, this, &RendererHost::onClient);
        // PerfLog is a diagnostic tool that writes synchronously to disk on
        // every event (flush() after each PERF_MARK). Keep it disabled in
        // production to avoid main-thread disk I/O jank during page load.
        PerfLog::instance().setEnabled(false);

        // Software-frame transport: the renderer never owns a visible window.
        // KHTML renders to an offscreen widget; frames are captured and sent
        // to the browser via QSharedMemory + a small JSON "frame" message.
        // This eliminates the WS_POPUP drag/freeze problem entirely: the
        // browser owns the only visible surface and window movement never
        // touches the renderer process.
        m_view = new RendererView(m_jsEnabled, nullptr);
        m_view->widget()->setEnabled(true);
        m_view->widget()->setFocusPolicy(Qt::StrongFocus);
        // Do NOT show the widget.  It remains offscreen; grab() renders it
        // into a pixmap on demand.  Resize is applied via IPC from the browser.

        // Shared memory key derived from the socket name so both sides agree.
        m_shmKey = QStringLiteral("khtml_frame_%1").arg(socketName);
        m_shm = new QSharedMemory(m_shmKey, this);

        // Frame capture timer: ~30 fps.  Coalesced naturally — if a grab is
        // still in progress (unlikely for software render) the next tick simply
        // overwrites the shared-memory buffer.
        m_frameTimer = new QTimer(this);
        m_frameTimer->setInterval(33);
        connect(m_frameTimer, &QTimer::timeout, this, &RendererHost::grabFrame);

        connect(m_view, &RendererView::navigateRequested, this, [this](const QUrl &u) {
            m_pendingUrl = u;
            ccSetUrl(u.toString().toUtf8().constData());
            m_loader->load(u);
        });
        connect(m_view, &RendererView::openNewTabRequested, this, [this](const QUrl &u) {
            sendMessage(QStringLiteral("newtab"), u.toString());
        });
        connect(m_loader, &PageLoader::finished, this,
                [this](const QUrl &localFile, const QUrl &finalUrl, const QString &err) {
            if (!err.isEmpty()) {
                sendMessage(QStringLiteral("error"), err);
                return;
            }
            // Notify the browser of the final URL (after redirects) so the
            // address bar and history reflect the actual document location.
            if (finalUrl.isValid()) {
                m_pendingUrl = finalUrl;
                sendMessage(QStringLiteral("url"), finalUrl.toString());
            }
            PERF_MARK("khtmlpart_load_start");
            // Bypass KIO for the PageLoader-generated local cached document.
            // The in-process kio_file.dll worker intermittently stalls after
            // the first ~32 KB chunk on large cached HTML files (219 KB Bing
            // document: 1/10 completion via KIO vs 10/10 via direct QFile).
            // KHTMLPart's public begin()/write()/end() API feeds the same
            // bytes directly without going through KIO.  HTTP/HTTPS navigation
            // and other KIO paths are unaffected.
            const QString localPath = localFile.toLocalFile();
            QFile f(localPath);
            if (!f.open(QIODevice::ReadOnly)) {
                sendMessage(QStringLiteral("error"),
                    QStringLiteral("Failed to open cached document: %1").arg(localPath));
                return;
            }
            const QByteArray html = f.readAll();
            f.close();
            if (html.isEmpty()) {
                sendMessage(QStringLiteral("error"),
                    QStringLiteral("Cached document is empty: %1").arg(localPath));
                return;
            }
            m_view->begin(localFile);
            m_view->write(html.constData(), html.size());
            m_view->end();
        });
        connect(m_view, QOverload<>::of(&KParts::ReadOnlyPart::completed), this, [this]() {
            PERF_MARK("khtmlpart_completed");
            // Apply tombstone restore (scroll + zoom) now that the document
            // is loaded and layout is ready. Uses native KHTML APIs, not JS.
            if (m_hasPendingRestore) {
                m_view->setZoomFactor(m_pendingRestoreZoom);
                KHTMLView *v = m_view->view();
                if (v) v->setContentsPos(m_pendingRestoreX, m_pendingRestoreY);
                m_hasPendingRestore = false;
            }
            const QVariant t = m_view->executeScript(QStringLiteral("document.title"));
            sendMessage(QStringLiteral("title"), t.isValid() ? t.toString() : QString());
            sendState();
        });
        // Periodic state report so the browser always has fresh scroll/zoom
        // for tombstone. One small JSON message every 10 s per active tab.
        m_stateTimer = new QTimer(this);
        m_stateTimer->setInterval(10000);
        connect(m_stateTimer, &QTimer::timeout, this, &RendererHost::sendState);
        m_stateTimer->start();
    }

    void showAt(int x, int y, int w, int h)
    {
        m_pendingX = x; m_pendingY = y; m_pendingW = w; m_pendingH = h;
        // Offscreen mode: no visible window.  Size is applied when the browser
        // connects and sends a resize, or immediately if non-default.
        if (w > 0 && h > 0 && m_view) {
            m_view->widget()->resize(w, h);
            ensureShm(w, h);
        }
    }

    // Capture the current KHTML view into shared memory and notify the
    // browser.  Called from a 30 fps timer and from scheduleFrame() on input.
    // Does not block the browser.  Uses dirty-region partial rendering when
    // the changed area is small (< 60% of viewport).
    void grabFrame()
    {
        m_framePending = false;
        if (!m_view || !m_shm) return;
        QWidget *w = m_view->widget();
        const int fw = w->width();
        const int fh = w->height();
        if (fw <= 0 || fh <= 0) return;

        // Flush coalesced mousemove/wheel before rendering this frame.
        flushPendingInput();

        ensureShm(fw, fh);
        if (!m_frameShmReady || !m_shm->isAttached()) return;
        if (!m_shm->lock()) return;

        // Decide full vs partial render.
        const qint64 viewArea = (qint64)fw * fh;
        QRect dirtyRect = m_dirtyRegion.boundingRect();
        dirtyRect = dirtyRect.intersected(QRect(0, 0, fw, fh));
        const bool partial = !dirtyRect.isEmpty() &&
            ((qint64)dirtyRect.width() * dirtyRect.height() < viewArea * 6 / 10);

        if (partial && !m_frameBuffer.isNull() &&
            m_frameBuffer.width() == fw && m_frameBuffer.height() == fh) {
            // Partial: render only the dirty sub-rect into a temp image, then
            // composite into the persistent frame buffer.
            QImage sub(dirtyRect.size(), QImage::Format_RGB32);
            sub.fill(Qt::white);
            QPainter sp(&sub);
            w->render(&sp, QPoint(), QRegion(dirtyRect));
            sp.end();
            QPainter fp(&m_frameBuffer);
            fp.drawImage(dirtyRect.topLeft(), sub);
            fp.end();
        } else {
            // Full frame render.
            m_frameBuffer = QImage(fw, fh, QImage::Format_RGB32);
            m_frameBuffer.fill(Qt::white);
            QPainter p(&m_frameBuffer);
            w->render(&p);
            p.end();
        }
        m_dirtyRegion = QRegion();

        // Copy to shared memory.
        const qsizetype bytes = qsizetype(fw) * qsizetype(fh) * 4;
        if (!m_shm || !m_shm->isAttached() || m_shm->size() < bytes) {
            m_shm->unlock();
            return;
        }
        uchar *dst = (uchar *)m_shm->data();
        const qint64 copyBytes = (qint64)fw * fh * 4;
        if (dst && m_shm->size() >= copyBytes) {
            const int stride = m_frameBuffer.bytesPerLine();
            if (stride == fw * 4) {
                memcpy(dst, m_frameBuffer.constBits(), copyBytes);
            } else {
                for (int y = 0; y < fh; y++)
                    memcpy(dst + y * fw * 4, m_frameBuffer.constScanLine(y), fw * 4);
            }
        }
        m_shm->unlock();
        sendMessage(QStringLiteral("frame"),
                    QStringLiteral("%1,%2,%3").arg(fw).arg(fh).arg(fw * 4));
    }

    // (Re)allocate shared memory to fit the given frame size.
    // Invariant: on return, if m_frameShmReady is true,
    //   m_shm->size() >= (qint64)m_frameW * m_frameH * 4.
    // grabFrame() never memcpy's beyond the attached segment.
    void ensureShm(int w, int h)
    {
        if (!m_shm) { m_frameShmReady = false; return; }
        const qsizetype needed = qsizetype(w) * qsizetype(h) * qsizetype(4);
        if (m_shm->isAttached() && m_shm->size() >= needed &&
            m_frameW == w && m_frameH == h) {
            m_frameShmReady = true;
            m_sizeMismatchNotified = false;
            return;
        }
        if (m_shm->isAttached()) m_shm->detach();
        if (!m_shm->create(needed)) {
            // create() failed — on Windows the Browser may still hold the old
            // segment open with a smaller size.  Try to attach to it.
            if (!m_shm->attach()) {
                m_frameShmReady = false;
                m_frameW = 0;
                m_frameH = 0;
                m_frameBuffer = QImage();
                m_dirtyRegion = QRegion();
                return;
            }
            if (m_shm->size() < needed) {
                // Existing segment is too small for this frame.  Cannot
                // resize while the Browser holds it.  Mark not ready so
                // grabFrame() skips the memcpy instead of overrunning.
                // Tell the Browser once to detach its old segment so the
                // next tick's create(needed) can succeed.
                m_frameShmReady = false;
                m_frameW = 0;
                m_frameH = 0;
                m_frameBuffer = QImage();
                m_dirtyRegion = QRegion();
                if (!m_sizeMismatchNotified) {
                    m_sizeMismatchNotified = true;
                    sendMessage(QStringLiteral("framesize"),
                                QStringLiteral("%1,%2").arg(w).arg(h));
                }
                return;
            }
            // Attached to an existing segment that is large enough (e.g. the
            // Browser had pre-created it). Fall through and use it.
        }
        // create() succeeded, or we attached to a sufficiently large segment.
        // Defensive invariant check before trusting the segment.
        if (m_shm->size() < needed) {
            m_frameShmReady = false;
            m_frameW = 0;
            m_frameH = 0;
            m_frameBuffer = QImage();
            m_dirtyRegion = QRegion();
            return;
        }
        m_frameW = w;
        m_frameH = h;
        m_frameShmReady = true;
        m_sizeMismatchNotified = false;
        // Size change invalidates the persistent frame buffer.
        m_frameBuffer = QImage();
        m_dirtyRegion = QRegion();
    }

    // === Input dispatch (WebKit EventDispatcher, lightweight) ===
    // Events arrive from the Browser via QLocalSocket JSON.  Clicks/keys are
    // dispatched immediately; mousemove and wheel are coalesced.
    void dispatchInput(const QJsonObject &obj)
    {
        const QString type = obj.value(QStringLiteral("type")).toString();
        KHTMLView *view = m_view ? m_view->view() : nullptr;
        if (!view) return;
        QWidget *vp = view->viewport();
        if (!vp) return;

        const int x = obj.value(QStringLiteral("x")).toInt();
        const int y = obj.value(QStringLiteral("y")).toInt();
        const QPoint localPos(x, y);
        const QPoint globalPos = vp->mapToGlobal(localPos);
        const Qt::MouseButton button = (Qt::MouseButton)obj.value(QStringLiteral("button")).toInt();
        const Qt::MouseButtons buttons = (Qt::MouseButtons)obj.value(QStringLiteral("buttons")).toInt();
        const Qt::KeyboardModifiers mods = (Qt::KeyboardModifiers)obj.value(QStringLiteral("modifiers")).toInt();

        if (type == QLatin1String("mousepress") || type == QLatin1String("mouserelease")
            || type == QLatin1String("mousedblclick")) {
            // Deliver to the viewport widget (not the KHTMLView/QScrollArea
            // itself).  KHTMLView's eventFilter installed on the viewport
            // routes QMouseEvents into viewportMousePressEvent ->
            // prepareMouseEvent -> hit-test -> DOM dispatch.
            QEvent::Type et;
            if (type == QLatin1String("mousepress"))
                et = QEvent::MouseButtonPress;
            else if (type == QLatin1String("mousedblclick"))
                et = QEvent::MouseButtonDblClick;
            else
                et = QEvent::MouseButtonRelease;
            // sendEvent (synchronous) so KHTML's hit-test and DOM click
            // dispatch complete before invalidateFull()/scheduleFrame()
            // capture the post-click render state.
            QMouseEvent ev(et, localPos, globalPos, button, buttons, mods);
            // Ensure the viewport (or a focused child) has Qt focus. In
            // offscreen mode the toplevel window is never active, so
            // QApplication::focusWidget() may be stale.
            if (et == QEvent::MouseButtonPress && !vp->focusWidget())
                vp->setFocus();
            QCoreApplication::sendEvent(vp, &ev);
            invalidateFull();
            scheduleFrame();
        } else if (type == QLatin1String("mousemove")) {
            // Coalesce: keep only the latest pending mousemove.
            m_pendingMouseMove = obj;
            m_hasPendingMouseMove = true;
            scheduleFrame();
        } else if (type == QLatin1String("wheel")) {
            // Coalesce: accumulate delta, dispatch at most once per tick.
            m_pendingWheelDeltaX += obj.value(QStringLiteral("deltaX")).toInt();
            m_pendingWheelDeltaY += obj.value(QStringLiteral("deltaY")).toInt();
            m_hasPendingWheel = true;
            m_wheelMouseX = x;
            m_wheelMouseY = y;
            m_wheelModifiers = mods;
            scheduleFrame();
        } else if (type == QLatin1String("keypress") || type == QLatin1String("keyrelease")) {
            QEvent::Type et = (type == QLatin1String("keypress"))
                ? QEvent::KeyPress : QEvent::KeyRelease;
            const int key = obj.value(QStringLiteral("key")).toInt();
            const QString text = obj.value(QStringLiteral("text")).toString();
            QKeyEvent *ev = new QKeyEvent(et, key, mods, text);
            // In offscreen mode the toplevel window is never "active" in Qt's
            // sense, so QApplication::focusWidget() stays null even after a
            // child (the KLineEdit of a focused <input>) has taken focus. Use the
            // content widget's own focusWidget() instead, which tracks the
            // focused child regardless of active-window state.
            QWidget *content = view->widget();
            QWidget *target = content ? content->focusWidget() : nullptr;
            if (!target) {
                target = content;
            }
            if (target)
                QCoreApplication::postEvent(target, ev);
            else
                delete ev;
            invalidateFull();
            scheduleFrame();
        }
    }

    // Flush coalesced mousemove and wheel events.  Called from grabFrame()
    // so at most one of each is dispatched per frame tick.
    void flushPendingInput()
    {
        KHTMLView *view = m_view ? m_view->view() : nullptr;
        if (!view) return;
        QWidget *content = view->widget();
        if (!content) return;
        // Deliver coalesced mousemove to the viewport widget, matching the
        // mouse press/release path. sendEvent (synchronous) with a stack
        // event so KHTML processes the move before the frame is captured.
        QWidget *vp = view->viewport();

        if (m_hasPendingMouseMove) {
            const QJsonObject &obj = m_pendingMouseMove;
            const QPoint lp(obj.value(QStringLiteral("x")).toInt(),
                            obj.value(QStringLiteral("y")).toInt());
            const QPoint gp = vp ? vp->mapToGlobal(lp) : content->mapToGlobal(lp);
            const Qt::MouseButtons btns = (Qt::MouseButtons)obj.value(QStringLiteral("buttons")).toInt();
            const Qt::KeyboardModifiers mods = (Qt::KeyboardModifiers)obj.value(QStringLiteral("modifiers")).toInt();
            if (vp) {
                QMouseEvent ev(QEvent::MouseMove, lp, gp, Qt::NoButton, btns, mods);
                QCoreApplication::sendEvent(vp, &ev);
                // Report the webpage cursor back to the Browser. KHTML's own
                // hover hit-test (KHTMLView::mouseMoveEvent -> prepareMouseEvent
                // -> RenderStyle::cursor()) has just applied the effective CSS
                // cursor to this viewport (unsetCursor() == default arrow). We
                // read back the shape KHTML determined instead of re-implementing
                // cursor logic here. The Browser applies it to the visible
                // PageSurface widget, which owns the only on-screen HWND.
                sendCursor((Qt::CursorShape)vp->cursor().shape());
            }
            m_hasPendingMouseMove = false;
            invalidateFull();
        }

        if (m_hasPendingWheel) {
            // Dispatch the wheel event to the viewport widget (matching the
            // mouse press/move path) for both scrolling and JS listeners.
            // sendEvent (synchronous) with a stack event so KHTML's
            // QScrollArea::wheelEvent scrolls before the frame is captured.
            // Do NOT also manipulate the scrollbar directly — that caused
            // double-scrolling (fast-path + QScrollArea::wheelEvent).
            // Keep the full accumulated delta; do NOT quantize by /120.
            const int dx = m_pendingWheelDeltaX;
            const int dy = m_pendingWheelDeltaY;
            const QPoint lp(m_wheelMouseX, m_wheelMouseY);
            const QPoint gp = vp ? vp->mapToGlobal(lp) : content->mapToGlobal(lp);
            if (vp) {
                QWheelEvent ev(lp, gp, QPoint(dx, dy), QPoint(dx, dy),
                               Qt::NoButton, m_wheelModifiers,
                               Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(vp, &ev);
            }
            m_hasPendingWheel = false;
            m_pendingWheelDeltaX = 0;
            m_pendingWheelDeltaY = 0;
            invalidateFull();
        }
    }

    // === Frame scheduling (WebKit DisplayRefreshMonitor, lightweight) ===
    // Input-triggered changes request an immediate frame instead of waiting
    // for the next 33ms timer tick.  Multiple requests coalesce into one.
    void scheduleFrame()
    {
        if (m_framePending) return;
        m_framePending = true;
        QTimer::singleShot(0, this, &RendererHost::grabFrame);
    }

    void invalidateFull()
    {
        if (m_view) {
            QWidget *w = m_view->widget();
            m_dirtyRegion = QRegion(0, 0, w->width(), w->height());
        }
    }

    void invalidateRect(const QRect &r)
    {
        m_dirtyRegion += r;
    }

    // Report current scroll position + zoom factor to the browser. Used for
    // lightweight tab tombstone/restore so a suspended tab can resume at the
    // same scroll offset and zoom without serializing DOM or JS state.
    void sendState()
    {
        KHTMLView *v = m_view ? m_view->view() : nullptr;
        if (!v) return;
        const int x = v->contentsX();
        const int y = v->contentsY();
        const int z = m_view->zoomFactor();
        sendMessage(QStringLiteral("state"),
                    QStringLiteral("%1,%2,%3").arg(x).arg(y).arg(z));
    }

    void loadInitial(const QUrl &url)
    {
        if (!url.isValid()) return;
        PERF_BEGIN(url.toString().toUtf8().constData());
        PERF_MARK("khtmlpart_load_start");
        m_pendingUrl = url;
        ccSetUrl(url.toString().toUtf8().constData());
        const QString s = url.scheme().toLower();
        if (s == QLatin1String("about")) {
            loadBlankPage();
        } else if (s == QLatin1String("file")) {
            m_view->loadUrl(url);
        } else {
            m_loader->load(url);
        }
    }

    // Load a truly blank page for about:blank and other about: URLs.
    // Writes a minimal empty document to a temp file because KHTMLPart has
    // no setHtml() and does not route about: through KIO on this platform.
    void loadBlankPage()
    {
        static const char *kBlankHtml =
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title></title></head><body></body></html>";
        const QString path = QDir::tempPath() + QStringLiteral("/khtmllite_blank.html");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(kBlankHtml);
            f.close();
            m_view->loadUrl(QUrl::fromLocalFile(path));
        }
    }

private slots:
    void onClient()
    {
        m_socket = m_server->nextPendingConnection();
        connect(m_socket, &QLocalSocket::readyRead, this, &RendererHost::onMessage);
        connect(m_socket, &QLocalSocket::disconnected, this, []() {
            // Parent browser process gone or IPC severed: exit cleanly to
            // avoid orphan renderer processes lingering after browser close.
            QCoreApplication::quit();
        });
        // Software-frame mode: no native window embedding.  The renderer
        // paints offscreen and sends frames via shared memory.  Notify the
        // browser that we are ready so it can start sending resize/navigate.
        QWidget *view = m_view->widget();
        if (m_pendingW > 0 && m_pendingH > 0)
            view->resize(m_pendingW, m_pendingH);
        // Fully initialize the offscreen widget: WA_DontShowOnScreen makes
        // show() not create a visible window, but Qt still runs the full
        // show/visibility path so KHTML's event filters and viewport state
        // are initialized correctly.  Without this, synthetic mouse events
        // are not processed because the viewport is !isVisible().
        view->setAttribute(Qt::WA_DontShowOnScreen, true);
        view->show();
        view->winId();
        ensureShm(view->width() > 0 ? view->width() : m_pendingW,
                  view->height() > 0 ? view->height() : m_pendingH);
        sendMessage(QStringLiteral("ready"), QString());
        m_frameTimer->start();
    }

    void onMessage()
    {
        // Coalesce: if multiple resize messages arrive in one batch, only
        // the last one matters — applying each intermediate size triggers a
        // synchronous KHTML relayout that can starve the event loop.
        int pendingW = -1, pendingH = -1;
        bool hasPendingResize = false;

        while (m_socket && m_socket->canReadLine()) {
            const QByteArray line = m_socket->readLine().trimmed();
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) continue;
            const QJsonObject obj = doc.object();
            const QString cmd = obj.value(QStringLiteral("cmd")).toString();
            if (cmd == QLatin1String("navigate")) {
                const QUrl u(obj.value(QStringLiteral("url")).toString());
                if (u.isValid()) {
                    PERF_BEGIN(u.toString().toUtf8().constData());
                    m_pendingUrl = u;
                    const QString sc = u.scheme().toLower();
                    if (sc == QLatin1String("about")) { PERF_MARK("khtmlpart_load_start"); loadBlankPage(); }
                    else if (sc == QLatin1String("file")) { PERF_MARK("khtmlpart_load_start"); m_view->loadUrl(u); }
                    else m_loader->load(u);
                }
            } else if (cmd == QLatin1String("resize")) {
                pendingW = obj.value(QStringLiteral("w")).toInt(800);
                pendingH = obj.value(QStringLiteral("h")).toInt(600);
                hasPendingResize = true;
            } else if (cmd == QLatin1String("reload")) {
                if (m_pendingUrl.isValid()) m_loader->load(m_pendingUrl);
            } else if (cmd == QLatin1String("restore")) {
                // Tombstone restore: store scroll/zoom, apply after page completes.
                m_pendingRestoreX = obj.value(QStringLiteral("x")).toInt();
                m_pendingRestoreY = obj.value(QStringLiteral("y")).toInt();
                m_pendingRestoreZoom = obj.value(QStringLiteral("z")).toInt(100);
                m_hasPendingRestore = true;
            } else if (cmd == QLatin1String("input")) {
                // Browser → Renderer input event.  Dispatched immediately for
                // click/key, coalesced for mousemove/wheel.
                dispatchInput(obj);
            }
        }

        // Apply only the final resize from this batch.
        if (hasPendingResize && m_view) {
            m_view->widget()->resize(pendingW, pendingH);
            ensureShm(pendingW, pendingH);
        }
    }

private:
    void sendMessage(const QString &type, const QString &data)
    {
        if (!m_socket) return;
        QJsonObject obj;
        obj[QStringLiteral("type")] = type;
        obj[QStringLiteral("data")] = data;
        m_socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
        m_socket->flush();
    }

    // Report the webpage cursor shape to the Browser over the existing IPC
    // channel (same line protocol as frame/title/state). Coalesced: only sent
    // when KHTML's hover hit-test yields a shape different from the last one
    // reported. Qt::ArrowCursor (0) is the default.
    void sendCursor(Qt::CursorShape shape)
    {
        if (shape == m_lastCursorShape) return;
        m_lastCursorShape = shape;
        sendMessage(QStringLiteral("cursor"),
                    QString::number(static_cast<int>(shape)));
    }

    QLocalServer *m_server = nullptr;
    QLocalSocket *m_socket = nullptr;
    QString m_socketName;
    QString m_shmKey;
    QSharedMemory *m_shm = nullptr;
    QTimer *m_frameTimer = nullptr;
    int m_frameW = 0, m_frameH = 0;
    bool m_frameShmReady = false;
    bool m_sizeMismatchNotified = false;
    RendererView *m_view = nullptr;
    PageLoader *m_loader = nullptr;
    QTimer *m_stateTimer = nullptr;
    QUrl m_pendingUrl;
    bool m_jsEnabled = true;
    int m_pendingX = 0, m_pendingY = 0, m_pendingW = 800, m_pendingH = 600;
    // Tombstone restore state, applied when the next document completes.
    int m_pendingRestoreX = 0;
    int m_pendingRestoreY = 0;
    int m_pendingRestoreZoom = 100;
    bool m_hasPendingRestore = false;
    // === Input event queue (WebKit EventDispatcher, lightweight) ===
    QJsonObject m_pendingMouseMove;
    bool m_hasPendingMouseMove = false;
    // Last webpage cursor shape reported to the Browser (-1 = not yet sent).
    int m_lastCursorShape = -1;
    int m_pendingWheelDeltaX = 0;
    int m_pendingWheelDeltaY = 0;
    bool m_hasPendingWheel = false;
    int m_wheelMouseX = 0, m_wheelMouseY = 0;
    Qt::KeyboardModifiers m_wheelModifiers;
    // === Dirty region + frame scheduling ===
    QRegion m_dirtyRegion;
    QImage m_frameBuffer;   // persistent buffer for partial updates
    bool m_framePending = false;
};

int main(int argc, char **argv)
{
    // Suppress Windows error dialogs on crash so the parent can detect exit.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // Install VEH + MiniDump before any Qt/KHTML code runs.
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char *slash = strrchr(exePath, '\\');
    if (slash) *slash = '\0';
    ccInit(exePath);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("KHtmlLiteRenderer"));
    // Renderer has no visible windows in software-frame mode; prevent Qt from
    // auto-quitting when there are no top-level windows to close.
    app.setQuitOnLastWindowClosed(false);

    const QString caPath = QCoreApplication::applicationDirPath()
                           + QStringLiteral("/ssl-ca-bundle.crt");
    if (QFile::exists(caPath))
        qputenv("SSL_CERT_FILE", QDir::toNativeSeparators(caPath).toUtf8());

    QString urlStr, socketName;
    int w = 800, h = 600;
    bool jsEnabled = true;
    for (int i = 1; i < argc; i++) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--url") && i + 1 < argc) urlStr = QString::fromLocal8Bit(argv[++i]);
        else if (a == QLatin1String("--socket") && i + 1 < argc) socketName = QString::fromLocal8Bit(argv[++i]);
        else if (a == QLatin1String("--width") && i + 1 < argc) w = atoi(argv[++i]);
        else if (a == QLatin1String("--height") && i + 1 < argc) h = atoi(argv[++i]);
        else if (a == QLatin1String("--disable-js")) jsEnabled = false;
    }

    if (socketName.isEmpty())
        socketName = QStringLiteral("khtml_renderer_%1").arg(QCoreApplication::applicationPid());

    RendererHost host(socketName, jsEnabled);
    host.showAt(0, 0, w, h);
    host.loadInitial(QUrl(urlStr));

    return app.exec();
}

#include "khtml_renderer.moc"
