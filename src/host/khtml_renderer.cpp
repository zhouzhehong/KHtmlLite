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
#include "startuplog.h"

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
        STARTUP_MARK("RV: base KHTMLPart returned");
        setJScriptEnabled(jsEnabled);
        STARTUP_MARK("RV: setJScriptEnabled done");
        setJavaEnabled(false);
        setPluginsEnabled(false);
        STARTUP_MARK("RV: java/plugins disabled");
        setEncoding(QStringLiteral("utf-8"), true);
        STARTUP_MARK("RV: setEncoding done");
        KParts::BrowserExtension *ext = browserExtension();
        STARTUP_MARK("RV: browserExtension fetched");
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
        STARTUP_MARK("RV: ctor done");
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
        STARTUP_MARK("IPC server listening");
        connect(m_server, &QLocalServer::newConnection, this, &RendererHost::onClient);
        // PerfLog is a diagnostic tool that writes synchronously to disk on
        // every event (flush() after each PERF_MARK). Keep it disabled in
        // production to avoid main-thread disk I/O jank during page load.
        // PerfLog is a diagnostic tool that writes synchronously to disk on
        // every event (flush() after each PERF_MARK). Keep it disabled in
        // production to avoid main-thread disk I/O jank during page load;
        // opt in with KHTML_PERF=1.
        PerfLog::instance().setEnabled(qEnvironmentVariableIntValue("KHTML_PERF") != 0);

        // Orphan watchdog: a pre-warmed renderer only ever becomes useful
        // when a browser adopts it.  If the browser died (crash/kill), this
        // process would idle forever and leak a Qt+KHTML process; exit if no
        // client ever shows up.
        QTimer::singleShot(10 * 60 * 1000, this, [this]() {
            if (!m_socket) QCoreApplication::quit();
        });

        // Software-frame transport: the renderer never owns a visible window.
        // KHTML renders to an offscreen widget; frames are captured and sent
        // to the browser via QSharedMemory + a small JSON "frame" message.
        // This eliminates the WS_POPUP drag/freeze problem entirely: the
        // browser owns the only visible surface and window movement never
        // touches the renderer process.
        STARTUP_MARK("RendererHost ctor: creating KHTMLPart");
        m_view = new RendererView(m_jsEnabled, nullptr);
        STARTUP_MARK("KHTMLPart created");
        m_view->widget()->setEnabled(true);
        m_view->widget()->setFocusPolicy(Qt::StrongFocus);
        // Do NOT show the widget.  It remains offscreen; grab() renders it
        // into a pixmap on demand.  Resize is applied via IPC from the browser.

        // Shared memory key derived from the socket name so both sides agree.
        m_shmKey = QStringLiteral("khtml_frame_%1").arg(socketName);
        m_shm = new QSharedMemory(m_shmKey, this);

        // Frame capture timer: ~60 fps, matching the display refresh (WebKit
        // paints once per vsync).  Coalesced naturally — if a grab is
        // still in progress the next tick simply overwrites the shared
        // buffer; no-op frames are pure early-outs.
        m_frameTimer = new QTimer(this);
        m_frameTimer->setInterval(16);
        connect(m_frameTimer, &QTimer::timeout, this, &RendererHost::grabFrame);

        // Engine damage → frame invalidation.  The KHTML view is offscreen
        // (WA_DontShowOnScreen), so QWidget::update() is a no-op and without
        // this hook every content repaint would be invisible: pages land
        // white, later images never appear.  contentsUpdated() carries the
        // damaged rect in viewport coordinates — WebKit TileController
        // damage plumbing, not per-input full invaliens.
        connect(m_view->view(), &KHTMLView::contentsUpdated, this,
                [this](const QRect &r) {
            QWidget *w = m_view ? m_view->widget() : nullptr;
            if (!w) return;
            m_dirtyRegion += r.intersected(QRect(0, 0, w->width(), w->height()));
            scheduleFrame();
        });

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
        // Progressive delivery, phase 2: once PageLoader has every sub-resource
        // on disk and has rewritten index.html, re-feed the complete document.
        // The initial parse permanently failed images/CSS whose file did not
        // exist yet; re-feeding after all resources landed heals them
        // (engine-side DocLoader now evicts hadError() cache entries).
        // Scroll/zoom are preserved so the swap is visually lossless — this
        // is our "final consistency" step, same job as WebKit's incremental
        // subresource load replacing placeholders.
        connect(m_loader, &PageLoader::refreshed, this,
                [this](const QUrl &localFile, const QUrl &finalUrl) {
            Q_UNUSED(finalUrl);
            PERF_MARK("refreshed_received");
            if (!m_view || !m_view->view()) return;
            KHTMLView *v = m_view->view();
            m_pendingRestoreX = v->contentsX();
            m_pendingRestoreY = v->contentsY();
            m_pendingRestoreZoom = m_view->zoomFactor();
            m_hasPendingRestore = true;
            QFile f(localFile.toLocalFile());
            if (!f.open(QIODevice::ReadOnly)) return;
            const QByteArray html = f.readAll();
            f.close();
            if (html.isEmpty()) return;
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

    void showAt(int w, int h)
    {
        m_pendingW = w; m_pendingH = h;
        // Offscreen mode: no visible window.  Size is applied when the browser
        // connects and sends a resize, or immediately if non-default.
        if (w > 0 && h > 0 && m_view) {
            m_view->widget()->resize(w, h);
            ensureShm(w, h);
        }
    }

    // === Tiled frame transport (WebKit/Chromium-style tile compositing) ===
    // The shared segment holds a fixed-size tile grid: a small header, a
    // dirty-tile index list, then tilesX*tilesY tiles of 256x256 RGBA
    // pixels packed row-major.  Only tiles intersecting the accumulated
    // damage region are re-rendered and copied; clean tiles keep their
    // previous pixels, so the browser can composite from its own canvas.
    static constexpr int kTileSize = 256;
    static constexpr qsizetype kTileBytes = qsizetype(kTileSize) * kTileSize * 4;
    static constexpr int kHeaderBytes = 32;

    // Worst-case segment size for a frame of w x h (full dirty list).
    static qsizetype shmNeeded(int w, int h)
    {
        const int tx = (w + kTileSize - 1) / kTileSize;
        const int ty = (h + kTileSize - 1) / kTileSize;
        return qsizetype(kHeaderBytes) + qsizetype(tx) * ty * 4
             + qsizetype(tx) * ty * kTileBytes;
    }

    // Capture the current KHTML view into shared memory and notify the
    // browser.  Called from a 30 fps timer and from scheduleFrame() on
    // input.  Does not block the browser.  With no damage and no size
    // change, this is a pure no-op (no render, no memcpy, no IPC).
    void grabFrame()
    {
        if (!m_visible) return;  // hidden tab: throttled, damage accumulates
        static bool s_loggedFirstTick = false;
        if (!s_loggedFirstTick) {
            s_loggedFirstTick = true;
            STARTUP_MARK("frame timer first tick");
        }
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

        const int tilesX = (fw + kTileSize - 1) / kTileSize;
        const int tilesY = (fh + kTileSize - 1) / kTileSize;
        const QRect frameRect(0, 0, fw, fh);

        // Damage policy (see resize handler): a width change forces a KHTML
        // relayout, so every tile is dirty — this also covers segment
        // recreation (m_tilesX reset).  A pure height growth keeps all
        // existing rows at the same tile indices: only the newly exposed
        // bottom strip is painted.  A pure height shrink invalidates
        // nothing; the browser simply crops its canvas.
        if (m_relayoutPending || tilesX != m_tilesX)
            m_dirtyRegion = QRegion(frameRect);
        else if (fh > m_frameH)
            m_dirtyRegion += QRect(0, m_frameH, fw, fh - m_frameH);
        m_relayoutPending = false;

        // === Scroll fast path (blit) ===
        // If the wheel was the sole cause of this frame and KHTML's scroll
        // offset actually moved, the whole page content shifted rigidly:
        // tell the browser to shift its canvas and only upload the newly
        // exposed strip.  Any frame that ALSO carries other damage falls
        // back to the normal path below — shifted pixels plus independent
        // damage cannot be combined safely, so we repaint fully.
        int sdx = 0, sdy = 0;
        int cx = 0, cy = 0;  // absolute KHTML scroll offset at capture time
        bool scrollChecked = false;
        bool scrollFast = false;
        {
            KHTMLView *kv = m_view ? m_view->view() : nullptr;
            if (kv) {
                cx = kv->contentsX();
                cy = kv->contentsY();
                if (m_pendingScrollCheck) {
                    m_pendingScrollCheck = false;
                    scrollChecked = true;
                    if (m_haveScrollPos && m_dirtyRegion.isEmpty()
                            && !m_relayoutPending
                            && fw == m_frameW && fh == m_frameH) {
                        sdx = cx - m_lastCX;
                        sdy = cy - m_lastCY;
                        scrollFast = (sdx != 0 || sdy != 0);
                    }
                }
                m_lastCX = cx;
                m_lastCY = cy;
                m_haveScrollPos = true;
            }
        }
        if (m_pendingScrollCheck)           // no view (or first frame): drop
            m_pendingScrollCheck = false;

        if (scrollChecked && !scrollFast && m_dirtyRegion.isEmpty()) {
            // Wheel at the page edge, or wheel that produced no scroll: the
            // browser may have applied a predictive shift, so correct it
            // with a zero-delta authoritative scroll message.
            sendMessage(QStringLiteral("scroll"),
                        QStringLiteral("0,0,%1,%2").arg(cx).arg(cy));
            return;                         // nothing else changed
        }

        if (scrollFast) {
            // Damage = only the viewport strip freshly scrolled into view.
            // Everything else stays at its shifted pixels on the browser.
            QRegion strip;
            if (sdy > 0) strip += QRect(0, fh - qMin(sdy, fh), fw, qMin(sdy, fh));
            else if (sdy < 0) strip += QRect(0, 0, fw, qMin(-sdy, fh));
            if (sdx > 0) strip += QRect(fw - qMin(sdx, fw), 0, qMin(sdx, fw), fh);
            else if (sdx < 0) strip += QRect(0, 0, qMin(-sdx, fw), fh);
            m_dirtyRegion = strip;

            // Tell the browser to shift its canvas first; the strip tiles in
            // this frame's "tiles" message land on the shifted canvas.  The
            // absolute scroll offset rides along so the browser can reconcile
            // its predictive scroll (WebKit async-scrolling-style handoff).
            sendMessage(QStringLiteral("scroll"),
                        QStringLiteral("%1,%2,%3,%4")
                            .arg(sdx).arg(sdy).arg(cx).arg(cy));

            // Deferred convergence: re-render everything a short moment
            // later, in case scrolling also repainted fixed-position or
            // script-driven content outside the strip.  The suppression
            // cache makes this cheap whenever nothing else changed (tiles
            // render back identical → no upload, no IPC).  Postponed while
            // the user is still scrolling.
            const quint32 epoch = ++m_scrollEpoch;
            QTimer::singleShot(250, this, [this, epoch]() {
                if (epoch != m_scrollEpoch || !m_view) return;
                invalidateFull();
                scheduleFrame();
            });
        }

        const QRegion dirty = m_dirtyRegion.intersected(frameRect);
        const bool dimsChanged = (fw != m_frameW || fh != m_frameH);
        if (dirty.isEmpty() && !dimsChanged)
            return;  // nothing changed — browser keeps its existing canvas

        // Collect dirty tile indices (ordered, row-major).
        QVector<int> dirtyTiles;
        for (int ty = 0; ty < tilesY; ++ty)
            for (int tx = 0; tx < tilesX; ++tx) {
                const QRect tileRect(tx * kTileSize, ty * kTileSize,
                                     kTileSize, kTileSize);
                if (dirty.intersects(tileRect))
                    dirtyTiles.append(ty * tilesX + tx);
            }

        // Same-tile suppression cache: FNV-1a hashes of the pixels most
        // recently uploaded for each tile slot.  Damage granularity is
        // coarse (any input currently invalidates the full frame), so many
        // "dirty" tiles render back pixel-identical; those are skipped
        // below, before the memcpy and before entering the index list.  A
        // zero entry means "unknown slot content" and never suppresses.
        if (m_tileHashes.size() != tilesX * tilesY) {
            m_tileHashes.fill(0, tilesX * tilesY);
        }

        if (m_shm->size() < shmNeeded(fw, fh)) {
            // Capacity race — ensureShm() should have covered this.
            return;
        }

        // Pass 1: render the frame ONCE into a surface, then treat tiles as
        // CROPS of that surface.
        //
        // WebKit equivalence: WK renders the page into a display list once;
        // TileController/TileGrid then paint per-tile by replaying the list
        // with a clip — tiles never trigger independent page render passes.
        // The old code called widget->render() per tile (up to tilesX*tilesY
        // full render trees per frame), which was the dominant cost of every
        // frame.  Here we do one w->render() and crop.
        QImage frameImage(fw, fh, QImage::Format_RGB32);
        frameImage.fill(Qt::white);
        {
            QPainter fp(&frameImage);
            STARTUP_MARK("render pass begin");
            w->render(&fp, QPoint(), QRegion(dirty));
            fp.end();
            STARTUP_MARK("render pass done");
        }

        // Debug: KHTML_FRAME_DUMP=1 makes the renderer save its grabbed
        // frame to <appDir>/frame_dump.png roughly once per second, so a
        // headless/CI-style check can see what the renderer actually painted
        // without peer-reviewing tile plumbing.
        if (qEnvironmentVariableIsSet("KHTML_FRAME_DUMP")) {
            static QElapsedTimer s_dumpTimer;
            static int s_dumpCount = 0;
            static bool s_dumpStarted = [] { s_dumpTimer.start(); return true; }();
            if (s_dumpCount < 10 || s_dumpTimer.elapsed() >= 1000) {
                ++s_dumpCount;
                s_dumpTimer.restart();
                frameImage.save(QCoreApplication::applicationDirPath()
                                + QStringLiteral("/frame_dump.png"));
                // Diagnostic: is the document actually laid out?
                KHTMLView *kv = m_view->view();
                QVariant t = m_view->executeScript(QStringLiteral(
                    "document.title + '|' + document.URL + '|' + (document.body ? document.body.innerHTML.length : -1)"));
                STARTUP_MARK(QStringLiteral("dump cw=%1 ch=%2 info=%3")
                             .arg(kv ? kv->contentsWidth() : -1)
                             .arg(kv ? kv->contentsHeight() : -1)
                             .arg(t.toString()).toUtf8().constData());
            }
        }

        // FNV-1a over a normalized tile (frame pixels, white outside the
        // frame) mirroring what the browser will composite.
        QVector<QImage> uploadImages;
        QVector<int> uploadTiles;
        QVector<quint64> uploadHashes;
        uploadImages.reserve(dirtyTiles.size());
        uploadTiles.reserve(dirtyTiles.size());
        uploadHashes.reserve(dirtyTiles.size());
        for (int k = 0; k < dirtyTiles.size(); ++k) {
            const int t = dirtyTiles[k];
            const int tileX = (t % tilesX) * kTileSize;
            const int tileY = (t / tilesX) * kTileSize;

            // Hash directly over the frame-image rows (clip at edges,
            // white pad beyond the frame so hashes stay well-defined).
            quint64 h = 14695981039346656037ULL;
            for (int y = 0; y < kTileSize; ++y) {
                const int fy = tileY + y;
                if (fy >= 0 && fy < fh) {
                    const uchar *row = frameImage.constScanLine(fy);
                    const int xs = qBound(0, tileX, fw);
                    const int xe = qBound(xs, tileX + kTileSize, fw);
                    for (int b = xs * 4; b < xe * 4; ++b) {
                        h ^= row[b];
                        h *= 1099511628211ULL;
                    }
                    // Out-of-frame / left-of-crop region hashes as white.
                    static const uchar whiteTile[4] = {0xFF, 0xFF, 0xFF, 0xFF};
                    for (int b = 0; b < (kTileSize - (xe - xs)) * 4; ++b) {
                        h ^= whiteTile[b % 4];
                        h *= 1099511628211ULL;
                    }
                } else {
                    // Whole row is outside the frame: hashes as white.
                    for (int b = 0; b < kTileSize * 4; ++b) {
                        h ^= 0xFF;
                        h *= 1099511628211ULL;
                    }
                }
            }
            if (h == 0) h = 1;  // reserve 0 for "unknown slot content"
            if (h == m_tileHashes[t])
                continue;  // pixel-identical to the last upload: skip
            // Do NOT commit m_tileHashes yet: if the SHM lock below fails we
            // must not believe these pixels were uploaded.

            // Materialize the tile crop only when it is actually uploaded.
            QImage tile(kTileSize, kTileSize, QImage::Format_RGB32);
            tile.fill(Qt::white);
            const int copyW = qMin(kTileSize, fw - tileX);
            const int copyH = qMin(kTileSize, fh - tileY);
            if (copyW > 0 && copyH > 0) {
                QPainter tp(&tile);
                tp.drawImage(0, 0, frameImage, tileX, tileY, copyW, copyH);
                tp.end();
            }
            uploadTiles.append(t);
            uploadImages.append(tile);
            uploadHashes.append(h);
        }
        frameImage = QImage();  // release the frame surface before SHM write
        const int dirtyCount = uploadTiles.size();
        const int headerBytes = kHeaderBytes + dirtyCount * 4;

        // Pass 2: write the surviving index list and tile payloads.
        if (!m_shm->lock()) return;
        uchar *base = (uchar *)m_shm->data();
        qint32 *idx = (qint32 *)(base + kHeaderBytes);
        uchar *tileData = base + headerBytes;
        for (int k = 0; k < dirtyCount; ++k) {
            const int t = uploadTiles[k];
            idx[k] = t;
            const QImage &tile = uploadImages[k];
            uchar *dst = tileData + qsizetype(t) * kTileBytes;
            if (tile.bytesPerLine() == kTileSize * 4)
                memcpy(dst, tile.constBits(), kTileBytes);
            else
                for (int y = 0; y < kTileSize; y++)
                    memcpy(dst + y * kTileSize * 4, tile.constScanLine(y),
                           kTileSize * 4);
        }
        uploadImages.clear();

        // Header is written LAST so the reader never sees a header
        // describing tiles that are not yet copied.
        qint32 *hdr = (qint32 *)base;
        hdr[0] = fw;
        hdr[1] = fh;
        hdr[2] = tilesX;
        hdr[3] = tilesY;
        hdr[4] = dirtyCount;
        m_shm->unlock();

        // The upload is durable now — commit the suppression hashes.
        for (int k = 0; k < dirtyCount; ++k)
            m_tileHashes[uploadTiles[k]] = uploadHashes[k];

        if (dirtyCount == 0 && !dimsChanged) {
            // Every damaged tile rendered identical to the last upload — the
            // frame content is unchanged, so skip the IPC notification too.
        } else {
            sendMessage(QStringLiteral("tiles"),
                        QStringLiteral("%1,%2,%3,%4")
                            .arg(fw).arg(fh).arg(cx).arg(cy));
            if (!m_sentFirstFrame) {
                m_sentFirstFrame = true;
                STARTUP_MARK("first frame uploaded");
            }
        }

        m_frameW = fw;
        m_frameH = fh;
        m_tilesX = tilesX;
        m_tilesY = tilesY;
        m_dirtyRegion = QRegion();
    }

    // (Re)allocate shared memory to fit the tile grid for the given frame
    // size.  Invariant: on return, if m_frameShmReady is true,
    //   m_shm->size() >= shmNeeded(w, h).
    // Grow-only: the segment is kept as long as it can hold the grid, so a
    // maximize -> restore -> maximize cycle no longer destroys/recreates
    // it per transition.  A recreated segment starts uninitialized, so the
    // tile grid bookkeeping is reset to force a full tile re-write.
    void ensureShm(int w, int h)
    {
        if (!m_shm) { m_frameShmReady = false; return; }
        const qsizetype needed = shmNeeded(w, h);
        if (m_shm->isAttached() && m_shm->size() >= needed) {
            m_frameShmReady = true;
            m_sizeMismatchNotified = false;
            return;
        }
        if (m_shm->isAttached()) m_shm->detach();
        if (!m_shm->create(needed)) {
            // create() failed — on Windows the Browser may still hold the
            // old segment open with a smaller size.  Try to attach to it.
            if (!m_shm->attach()) {
                m_frameShmReady = false;
                m_frameW = 0;
                m_frameH = 0;
                m_tilesX = 0;
                m_tilesY = 0;
                m_dirtyRegion = QRegion();
                m_tileHashes.clear();
                return;
            }
            if (m_shm->size() < needed) {
                // Existing segment is too small for this frame.  Cannot
                // resize while the Browser holds it.  Mark not ready so
                // grabFrame() skips the copy instead of overrunning.
                // Tell the Browser once to detach its old segment so the
                // next tick's create(needed) can succeed.
                m_frameShmReady = false;
                m_frameW = 0;
                m_frameH = 0;
                m_tilesX = 0;
                m_tilesY = 0;
                m_dirtyRegion = QRegion();
                m_tileHashes.clear();
                if (!m_sizeMismatchNotified) {
                    m_sizeMismatchNotified = true;
                    sendMessage(QStringLiteral("framesize"),
                                QStringLiteral("%1,%2").arg(w).arg(h));
                }
                return;
            }
            // Attached to an existing segment that is large enough (e.g.
            // the Browser had pre-created it). Fall through and use it.
        }
        // Fresh segment — its tile payload is undefined.  Reset the grid
        // bookkeeping so grabFrame() marks every tile dirty once, and clear
        // the suppression cache so every tile is uploaded at least once.
        m_tilesX = 0;
        m_tilesY = 0;
        m_tileHashes.clear();
        m_frameShmReady = true;
        m_sizeMismatchNotified = false;
    }

    // === Input dispatch (WebKit EventDispatcher, lightweight) ===
    // Events arrive from the Browser via QLocalSocket JSON.  Clicks/keys are
    // dispatched immediately; mousemove and wheel are coalesced.
    void dispatchInput(const QJsonObject &obj)
    {
        const QString type = obj.value(QStringLiteral("type")).toString();
        KHTMLView *view = m_view ? m_view->view() : nullptr;
        if (!view) return;
        // Mouse input must be delivered to KHTML's content widget (widget()),
        // not to the viewport.  KHTMLView::eventFilter watches widget() and
        // forwards its events to widgetEvent(), which dispatches through
        // QFrame::event() into KHTMLView::mousePressEvent/mouseMoveEvent
        // (hit-test -> DOM dispatch).  Events sent to viewport() never reach
        // those handlers because QAbstractScrollArea::event() swallows mouse
        // events, so clicks/focus never got established.
        QWidget *mw = view->widget();
        if (!mw) return;

        const int x = obj.value(QStringLiteral("x")).toInt();
        const int y = obj.value(QStringLiteral("y")).toInt();
        const QPoint localPos(x, y);
        const QPoint globalPos = mw->mapToGlobal(localPos);
        const Qt::MouseButton button = (Qt::MouseButton)obj.value(QStringLiteral("button")).toInt();
        const Qt::MouseButtons buttons = (Qt::MouseButtons)obj.value(QStringLiteral("buttons")).toInt();
        const Qt::KeyboardModifiers mods = (Qt::KeyboardModifiers)obj.value(QStringLiteral("modifiers")).toInt();

        if (type == QLatin1String("mousepress") || type == QLatin1String("mouserelease")
            || type == QLatin1String("mousedblclick")) {
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
            // Ensure the content widget (or a focused child such as the
            // KLineEdit of an <input>) has Qt focus.  In offscreen mode the
            // toplevel window is never active, so QApplication::focusWidget()
            // may be stale.
            if (et == QEvent::MouseButtonPress && !mw->focusWidget())
                mw->setFocus();
            QCoreApplication::sendEvent(mw, &ev);
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
        // Deliver coalesced mousemove to the content widget, matching the
        // mouse press/release path (see dispatchInput). sendEvent
        // (synchronous) with a stack event so KHTML processes the move
        // before the frame is captured.
        QWidget *vp = view->viewport();

        if (m_hasPendingMouseMove) {
            const QJsonObject &obj = m_pendingMouseMove;
            const QPoint lp(obj.value(QStringLiteral("x")).toInt(),
                            obj.value(QStringLiteral("y")).toInt());
            const QPoint gp = content->mapToGlobal(lp);
            const Qt::MouseButtons btns = (Qt::MouseButtons)obj.value(QStringLiteral("buttons")).toInt();
            const Qt::KeyboardModifiers mods = (Qt::KeyboardModifiers)obj.value(QStringLiteral("modifiers")).toInt();
            {
                QMouseEvent ev(QEvent::MouseMove, lp, gp, Qt::NoButton, btns, mods);
                QCoreApplication::sendEvent(content, &ev);
                // Report the webpage cursor back to the Browser. KHTML's own
                // hover hit-test (KHTMLView::mouseMoveEvent -> prepareMouseEvent
                // -> RenderStyle::cursor()) has just applied the effective CSS
                // cursor to this viewport (unsetCursor() == default arrow). We
                // read back the shape KHTML determined instead of re-implementing
                // cursor logic here. The Browser applies it to the visible
                // PageSurface widget, which owns the only on-screen HWND.
                if (vp) {
                    sendCursor((Qt::CursorShape)vp->cursor().shape());
                }
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
            // Wheel = likely pure scroll.  Do NOT invalidateFull here; instead
            // ask grabFrame() to compare the scroll offset and, when nothing
            // else was damaged this frame, blit-shift the browser canvas and
            // re-render only the newly exposed strip.  Pages with
            // position:fixed / scroll listeners could change pixels outside
            // the strip; a deferred convergence frame heals those (see
            // grabFrame).
            m_pendingScrollCheck = true;
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
        STARTUP_MARK("browser connected");
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
        // Carries renderer-local elapsed ms so the browser can compute an
        // exact clock skew for startup-log alignment (report script uses it).
        sendMessage(QStringLiteral("ready"),
                    QString::number(StartupLog::clock().elapsed()));
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
                    StartupLog::mark(("navigate_received " + u.toString()).toUtf8().constData());
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
            } else if (cmd == QLatin1String("visible")) {
                // Background-tab throttling (WebKit: hidden pages get no
                // DisplayRefreshMonitor ticks).  The 60 Hz frame timer is
                // stopped while the tab is invisible; damage that arrives
                // while hidden is accumulated and flushed with one full
                // frame when the tab becomes visible again.
                const bool vis = obj.value(QStringLiteral("v")).toInt(1) != 0;
                m_visible = vis;
                if (vis) {
                    if (!m_frameTimer->isActive()) m_frameTimer->start();
                    invalidateFull();
                    scheduleFrame();
                } else {
                    m_frameTimer->stop();
                }
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
            // Damage policy for the tiled transport: a width change forces
            // a KHTML relayout, so the whole frame must be repainted.
            // A pure height change leaves the page layout untouched: the
            // existing tiles stay valid and only the newly exposed bottom
            // strip (if any) is painted — grabFrame() adds that strip from
            // the committed-vs-new frame height.
            if (pendingW != m_frameW)
                m_relayoutPending = true;
            // Request a frame immediately after the resize instead of
            // waiting for the next 33 ms timer tick.  The synchronous
            // relayout above is already paid; the sooner the first frame
            // at the new size is delivered, the sooner the browser stops
            // scaling the stale frame.
            scheduleFrame();
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
    int m_pendingW = 800, m_pendingH = 600;
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
    // === Dirty region + frame scheduling (tiled transport) ===
    QRegion m_dirtyRegion;
    // Tile grid dims of the segment's current payload (0 = uninitialized;
    // forces a full tile re-write on the next grabFrame).
    int m_tilesX = 0, m_tilesY = 0;
    // Same-tile suppression: FNV-1a hashes of the pixels most recently
    // uploaded per tile slot (0 = unknown, never suppresses).  Sized
    // tilesX*tilesY; cleared whenever the shared segment is recreated,
    // because a recreated segment's tile payload is undefined.
    QVector<quint64> m_tileHashes;
    // === Scroll fast path (canvas blit) ===
    // Set by the wheel handler instead of invalidateFull(): grabFrame()
    // compares the KHTML scroll offset against the last captured frame and,
    // when the wheel was the sole cause of the frame, shifts the browser's
    // canvas and uploads only the newly exposed strip.
    bool m_pendingScrollCheck = false;
    bool m_haveScrollPos = false;
    int m_lastCX = 0, m_lastCY = 0;
    // Debounce for the post-scroll convergence frame (see grabFrame).
    quint32 m_scrollEpoch = 0;
    // Set when a resize changed the viewport width: KHTML relayouts, so
    // all tiles are dirty regardless of tile-grid coincidence.
    bool m_relayoutPending = false;
    bool m_framePending = false;
    bool m_visible = true;  // hidden tabs skip the 60 Hz frame path entirely
    bool m_sentFirstFrame = false;  // startup timing: first tiles upload
};

int main(int argc, char **argv)
{
    // Suppress Windows error dialogs on crash so the parent can detect exit.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    STARTUP_MARK("main entry");
    // Install VEH + MiniDump before any Qt/KHTML code runs.
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char *slash = strrchr(exePath, '\\');
    if (slash) *slash = '\0';
    ccInit(exePath);
    STARTUP_MARK("crash handler installed");

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("KHtmlLiteRenderer"));
    STARTUP_MARK("QApplication ready");
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
    STARTUP_MARK("RendererHost constructed (KHTMLPart up)");
    host.showAt(w, h);
    host.loadInitial(QUrl(urlStr));
    STARTUP_MARK("initial load kicked");

    return app.exec();
}

#include "khtml_renderer.moc"
