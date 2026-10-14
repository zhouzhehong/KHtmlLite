// probe_kconfig.cpp — micro-benchmark for the KConfig cold-read that
// dominates KHTMLPart construction (~800 ms measured inside the renderer).
//
// Times, in a fresh process:
//   1. QCoreApplication creation
//   2. first KConfig("khtmlrc", NoGlobals) open + full group/entry walk
//   3. first KSharedConfig::openConfig()
//   4. second KConfig("khtmlrc", NoGlobals) (warm, same process)
//   5. KIconLoader first icon lookup (image-missing) — the other known cost
// Run: probe_kconfig.exe [repeatCount]
// Needs KHTML_STARTUP_TIMING=0 (irrelevant) — prints plain numbers to stdout.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <QPixmap>
#include <QSettings>
#include <QFile>
#include <windows.h>
#include <QFile>
#include <kconfig.h>
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <kiconloader.h>
#include <QStandardPaths>

static void walkConfig(KConfig *cfg, const QString &label)
{
    QElapsedTimer t; t.start();
    int groups = 0, entries = 0;
    for (const QString &g : cfg->groupList()) {
        ++groups;
        KConfigGroup cg(cfg, g);
        entries += cg.entryMap().size();
    }
    qInfo("  %-28s %lld ms  (groups=%d entries=%d)",
          qPrintable(label), (long long)t.elapsed(), groups, entries);
}

int main(int argc, char **argv)
{
    QElapsedTimer t;

    t.start();
    QCoreApplication app(argc, argv);
    qInfo("QCoreApplication:             %lld ms", (long long)t.elapsed());

    t.restart();
    {
        KConfig cfg(QStringLiteral("khtmlrc"), KConfig::NoGlobals);
        qInfo("KConfig(khtmlrc) 1st open:    %lld ms", (long long)t.elapsed());
        walkConfig(&cfg, QStringLiteral("walk khtmlrc"));
        t.restart();
        KConfigGroup cg(&cfg, "HTML Settings");
        (void)cg.entryMap();
        qInfo("  read 'HTML Settings':       %lld ms", (long long)t.elapsed());
    }

    t.restart();
    {
        KSharedConfig::Ptr sc = KSharedConfig::openConfig();
        qInfo("KSharedConfig 1st open:       %lld ms (%s)",
              (long long)t.elapsed(), qPrintable(sc->name()));
    }

    t.restart();
    {
        KConfig cfg2(QStringLiteral("khtmlrc"), KConfig::NoGlobals);
        KConfigGroup cg(&cfg2, "MainView Settings");
        (void)cg.entryMap();
        qInfo("KConfig(khtmlrc) 2nd (warm):  %lld ms", (long long)t.elapsed());
    }

    t.restart();
    qInfo("GenericConfigLocation:        %lld ms -> %s", (long long)t.elapsed(),
          qPrintable(QStandardPaths::standardLocations(
              QStandardPaths::GenericConfigLocation).join(" ; ")));

    // Does a plain QSettings ini write into GenericCacheLocation work?
    {
        const QString p = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                          + QStringLiteral("/khtml_fonts.ini");
        qInfo("cache path: %s", qPrintable(p));
        QSettings s(p, QSettings::IniFormat);
        s.setValue(QStringLiteral("fonts/SerifFont"), QStringLiteral("PROBE"));
        s.sync();
        qInfo("QSettings sync status=%d fileExists=%s", (int)s.status(),
              QFile::exists(p) ? "YES" : "no");
        // Compare against a hand-rolled write of the same path.
        QFile f(p);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("[fonts]\nSerifFont=PROBE\n");
            f.close();
            qInfo("plain QFile write: OK, exists=%s", QFile::exists(p) ? "YES" : "no");
        } else {
            qInfo("plain QFile write: FAILED err=%d errno=%d (%s)",
                  (int)f.error(), errno, strerror(errno));
        }
        // Bare Win32 create to see the raw error.
        const std::wstring wp = p.toStdWString();
        HANDLE h = CreateFileW(wp.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            qInfo("CreateFileW: FAILED gle=%lu", GetLastError());
        } else {
            qInfo("CreateFileW: OK");
            CloseHandle(h);
            DeleteFileW(wp.c_str());
        }
    }

    // persisted-font-cache miss: write a key, sync, and report the file that
    // appeared (or state that none did).
    {
        KConfig wcfg(QStringLiteral("KCONFIG_PROBE"), KConfig::NoGlobals);
        KConfigGroup wg(&wcfg, "Probe");
        wg.writeEntry("k", "v");
        wg.sync();
        qInfo("probe write done; searching candidate dirs...");
        for (const QString &base : {
                 QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
                 QStandardPaths::writableLocation(QStandardPaths::ConfigLocation),
                 QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation),
                 QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}) {
            qInfo("  writable dir: %s  KCONFIG_PROBE exists=%s khtmlrc exists=%s",
                  qPrintable(base),
                  QFile::exists(base + QStringLiteral("/KCONFIG_PROBE")) ? "YES" : "no",
                  QFile::exists(base + QStringLiteral("/khtmlrc")) ? "YES" : "no");
        }
        // Also dump what readEntry sees for the font keys:
        KConfig rcfg(QStringLiteral("khtmlrc"), KConfig::NoGlobals);
        KConfigGroup rg(&rcfg, "HTML Settings");
        qInfo("khtmlrc SerifFont='%s' (hasKey=%s)",
              qPrintable(rg.readEntry("SerifFont", QString())),
              rg.hasKey("SerifFont") ? "YES" : "no");
    }


    t.restart();
    {
        KIconLoader *il = KIconLoader::global();
        qInfo("KIconLoader::global():        %lld ms", (long long)t.elapsed());
        t.restart();
        QPixmap pm = il->loadIcon(QStringLiteral("image-missing"),
                                  KIconLoader::Desktop, 16);
        qInfo("loadIcon(image-missing):      %lld ms (null=%d)",
              (long long)t.elapsed(), pm.isNull() ? 1 : 0);
    }
    return 0;
}
