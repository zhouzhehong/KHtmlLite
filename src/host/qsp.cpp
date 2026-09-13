#include <cstdio>
#include <cstdlib>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QStringList>

static void out(const char *tag, const QString &s)
{
    fprintf(stderr, "%s %s\n", tag, s.toLocal8Bit().constData());
    fflush(stderr);
}

int main(int c, char **v)
{
    fprintf(stderr, "QSP-START\n"); fflush(stderr);
    QCoreApplication a(c, v);
    const QStringList locs =
        QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString &l : locs) out("GENERIC-DATA:", l);
    out("LOCATE-html4:",
        QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                               "kf5/khtml/css/html4.css"));
    out("XDG_DATA_DIRS:",
        QString::fromLocal8Bit(qgetenv("XDG_DATA_DIRS")));
    fprintf(stderr, "QSP-END\n");
    return 0;
}
