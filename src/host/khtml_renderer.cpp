// khtml_renderer.cpp — out-of-process rendering sandbox.
// Runs one KHTMLPart in a child process; the main shell embeds this window
// and monitors the process so a renderer crash never takes down the browser.
#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QLocalServer>
#include <QLocalSocket>
#include <QUrl>
#include <QTimer>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QCoreApplication>

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
        PerfLog::instance().setEnabled(true);

        m_container = new QWidget(nullptr, Qt::FramelessWindowHint);
        m_container->setWindowTitle(QStringLiteral("KHtmlLite-Renderer"));
        m_container->setEnabled(true);
        m_container->setFocusPolicy(Qt::StrongFocus);
        m_container->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        // KHTML view widget is created top-level (no widget parent) so its
        // native HWND can be embedded directly by the browser process.
        m_view = new RendererView(m_jsEnabled, nullptr);
        m_view->widget()->setEnabled(true);
        m_view->widget()->setFocusPolicy(Qt::StrongFocus);
        m_view->widget()->setAttribute(Qt::WA_TransparentForMouseEvents, false);

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
            m_view->loadUrl(localFile);
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
        // Window is shown in onClient() once the parent connects, so we can
        // send the HWND through the socket (GUI apps have no stdout).
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
        // Embed the actual KHTML widget directly. It was created top-level
        // (no widget parent), so its native HWND is ready for the browser to
        // Win32-SetParent into its tab container.
        QWidget *view = m_view->widget();
        view->setGeometry(m_pendingX, m_pendingY, m_pendingW, m_pendingH);
        view->show();
        view->winId(); // force native window creation
        const QString hwndStr = QString::number((quint64)(uintptr_t)view->winId());
        sendMessage(QStringLiteral("hwnd"), hwndStr);
        // Also write HWND to a temp file as a fallback (socket can be flaky).
        QFile hf(QDir::tempPath() + QStringLiteral("/khtml_hwnd_%1.txt").arg(m_socketName));
        if (hf.open(QIODevice::WriteOnly)) {
            hf.write(hwndStr.toUtf8());
            hf.close();
        }
    }

    void onMessage()
    {
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
                m_view->widget()->resize(obj.value(QStringLiteral("w")).toInt(800),
                                         obj.value(QStringLiteral("h")).toInt(600));
            } else if (cmd == QLatin1String("reload")) {
                if (m_pendingUrl.isValid()) m_loader->load(m_pendingUrl);
            } else if (cmd == QLatin1String("restore")) {
                // Tombstone restore: store scroll/zoom, apply after page completes.
                m_pendingRestoreX = obj.value(QStringLiteral("x")).toInt();
                m_pendingRestoreY = obj.value(QStringLiteral("y")).toInt();
                m_pendingRestoreZoom = obj.value(QStringLiteral("z")).toInt(100);
                m_hasPendingRestore = true;
            }
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

    QLocalServer *m_server = nullptr;
    QLocalSocket *m_socket = nullptr;
    QString m_socketName;
    QWidget *m_container = nullptr;
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
