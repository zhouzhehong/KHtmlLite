// Probe: does Qt decode WebP via the imageformats plugin?
#include <cstdio>
#include <QCoreApplication>
#include <QImageReader>
#include <QImage>
#include <QPluginLoader>
#include <QDir>

int main(int argc, char **argv)
{
    QCoreApplication a(argc, argv);
    fprintf(stderr, "WEBPPROBE-START\n");

    // Supported formats
    fprintf(stderr, "FORMATS:");
    const QList<QByteArray> fmts = QImageReader::supportedImageFormats();
    for (const QByteArray &f : fmts)
        fprintf(stderr, " %s", f.constData());
    fprintf(stderr, "\n");

    // Try loading the plugin explicitly
    QDir plugDir(QCoreApplication::applicationDirPath() + "/imageformats");
    const QStringList entries = plugDir.entryList(QStringList() << "qwebp*");
    fprintf(stderr, "PLUGIN-DIR:%s entries=%d\n", plugDir.absolutePath().toLocal8Bit().constData(), entries.size());
    for (const QString &e : entries) {
        QPluginLoader l(plugDir.absoluteFilePath(e));
        fprintf(stderr, "LOAD:%s ok=%d err=%s\n", e.toLocal8Bit().constData(),
                l.isLoaded() || l.load(), l.errorString().toLocal8Bit().constData());
    }

    // Try to decode the file
    if (argc > 1) {
        QImageReader rd(QString::fromLocal8Bit(argv[1]));
        rd.setAutoTransform(true);
        fprintf(stderr, "CANREAD:%d FORMAT:%s ERR:%s\n",
                rd.canRead(), rd.format().constData(),
                rd.errorString().toLocal8Bit().constData());
        QImage img = rd.read();
        fprintf(stderr, "READ:%d SIZE:%dx%d\n", !img.isNull(), img.width(), img.height());
    }
    fprintf(stderr, "WEBPPROBE-END\n");
    return 0;
}
