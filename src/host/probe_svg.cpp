// probe_svg — renders a 5-variant fill test matrix through KHTMLPart and
// samples the painted pixels, verifying whether CSS/attribute `fill` colors
// (esp. uppercase hex like #3E4757 and rgb(), used by the Bing logo)
// survive parsing.  A grayscale result (R==G==B and != expected) on a
// variant reproduces the Bing-logo "everything gray" bug at that variant.
//
// Test matrix (same shape, different fill spellings):
//   A: attribute lowercase hex   fill="#f25022"  (control)
//   B: attribute uppercase hex   fill="#F25022"
//   C: style attribute           style="fill:#3E4757"
//   D: class rule in <style>     .d { fill: #7FBA00 }
//   E: style attribute rgb()     style="fill:rgb(255,185,0)"
//
// Two loading paths are exercised, matching the two ways KHTML can see SVG:
//   1) DOC path  — the matrix as a standalone .svg document opened via
//      begin(fileUrl)+write()+end() (mimetype application/svg+xml → ksvg2).
//   2) IMG path  — same file referenced via <img> from an HTML page, which
//      is what pageloader.cpp produces for inline <svg> blocks (the Bing
//      logo path), either khtmlimage/QtSvg or an SVG part depending on the
//      platform plugins.
// Results with pixel samples go to probe_svg_result.txt; screenshots to
// probe_svg_doc.png / probe_svg_img.png next to the exe.
#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QImage>
#include <QStringList>
#include <QStandardPaths>
#include <QFileInfo>
#include <QLibraryInfo>
#include <KHTMLPart>
#include <KParts/OpenUrlArguments>

static const char kSvg[] = R"SVG(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="540" height="120"
     viewBox="0 0 540 120">
  <style>.d { fill: #7FBA00 }</style>
  <style type="text/css">.f { fill: #01BCD4 }</style>
  <style>.G { fill: #f25022 }</style>
  <rect x="10"  y="10" width="65" height="90" fill="#f25022"/>
  <rect x="85"  y="10" width="65" height="90" fill="#F25022"/>
  <rect x="160" y="10" width="65" height="90" style="fill:#3E4757"/>
  <rect x="235" y="10" width="65" height="90" class="d"/>
  <rect x="310" y="10" width="65" height="90" style="fill:rgb(255,185,0)"/>
  <rect x="385" y="10" width="65" height="90" class="f"/>
  <rect x="460" y="10" width="65" height="90" class="G"/>
</svg>
)SVG";

static const char kNames[7][32] = {
    "A attr #f25022 (ctrl)", "B attr #F25022 hex-up",
    "C style #3E4757 hex-up", "D class #7FBA00 (no type)",
    "E style rgb(255,185,0)", "F class #01BCD4 (typed)",
    "G class .G #f25022 caps"
};
static const QRgb kWant[7] = {
    qRgb(0xf2, 0x50, 0x22), qRgb(0xf2, 0x50, 0x22),
    qRgb(0x3e, 0x47, 0x57), qRgb(0x7f, 0xba, 0x00),
    qRgb(255, 185, 0),      qRgb(0x01, 0xbc, 0xd4),
    qRgb(0xf2, 0x50, 0x22)
};
static const int kCount = 7;

static bool g_allOk = true;

static void dumpColors(QTextStream &rec, const QString &tag, const QImage &img)
{
    bool rowOk = true;
    for (int i = 0; i < kCount; ++i) {
        QRgb p = img.pixel(42 + i * 75, 55) | 0xff000000;
        QRgb w = kWant[i] | 0xff000000;
        const bool gray = qRed(p) == qGreen(p) && qGreen(p) == qBlue(p) && p != w;
        const bool ok = (p == w);
        if (!ok) rowOk = false;
        rec << tag << " " << kNames[i] << "  got=#"
            << QString::number(p & 0xffffff, 16).rightJustified(6, '0')
            << " want=#" << QString::number(w & 0xffffff, 16)
            << (ok ? "  OK" : (gray ? "  FAIL(gray)" : "  FAIL")) << Qt::endl;
    }
    if (!rowOk) g_allOk = false;
    rec << tag << " " << (rowOk ? "PASS" : "FAIL") << Qt::endl;
    rec.flush();
}

static QImage renderPart(KHTMLPart *part)
{
    QImage img(part->widget()->size(), QImage::Format_RGB32);
    img.fill(Qt::white);
    part->widget()->render(&img, QPoint(), QRegion(),
                           QWidget::DrawWindowBackground | QWidget::DrawChildren);
    return img;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QString dir = QCoreApplication::applicationDirPath();

    QFile log(dir + "/probe_svg_result.txt");
    log.open(QIODevice::WriteOnly | QIODevice::Truncate);
    QTextStream rec(&log);

    // --- Task-5 diagnostics: where does this process look for
    // kf5/khtml/css/html4.css (QStandardPaths::GenericDataLocation)? ---
    rec << "env QT_DATA_DIRS=" << qgetenv("QT_DATA_DIRS") << Qt::endl;
    rec << "QLibraryInfo PrefixPath="
        << QLibraryInfo::location(QLibraryInfo::PrefixPath) << Qt::endl;
    rec << "QLibraryInfo DataPath="
        << QLibraryInfo::location(QLibraryInfo::DataPath) << Qt::endl;
    const auto dirs = QStandardPaths::standardLocations(
        QStandardPaths::GenericDataLocation);
    for (const QString &d : dirs)
        rec << "GenericDataLocation: " << d << Qt::endl;
    const QString located = QStandardPaths::locate(
        QStandardPaths::GenericDataLocation, "kf5/khtml/css/html4.css");
    rec << "locate(kf5/khtml/css/html4.css)="
        << (located.isEmpty() ? QStringLiteral("<NOT FOUND>")
                              : QFileInfo(located).absoluteFilePath())
        << Qt::endl;
    rec.flush();

    // Recreate the artifact pageloader produces for inline <svg> blocks.
    const QString svgPath = dir + "/probe_svg_test.svg";
    {
        QFile f(svgPath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(kSvg);
    }

    // --- Path 1: standalone SVG document (image/svg+xml → ksvg2 DOM) ---
    KHTMLPart *partDoc = new KHTMLPart();
    partDoc->setJScriptEnabled(false);
    partDoc->widget()->resize(560, 140);
    partDoc->widget()->move(100, 100);
    partDoc->widget()->show();
    // begin()+write() with the mimetype preset via KParts::OpenUrlArguments
    // so KHTML classifies the payload as image/svg+xml → ksvg2 document.
    // (openUrl() would go through KIO, which cannot spawn kioslave sockets
    // in the dev shell — begin+write is also what pageloader uses.)
    KParts::OpenUrlArguments svgArgs;
    svgArgs.setMimeType(QStringLiteral("image/svg+xml"));
    partDoc->setArguments(svgArgs);
    partDoc->begin(QUrl::fromLocalFile(svgPath));
    partDoc->write(kSvg, int(sizeof(kSvg)) - 1);
    partDoc->end();
    rec << "step=doc-written mimetype=" << partDoc->document().isNull()
        << Qt::endl;

    // --- Path 2: <img> reference (pageloader's inline-SVG rewrite) ---
    const QString imgHtml = QStringLiteral(
        "<!DOCTYPE html><html><head><style>body{margin:0}</style></head>"
        "<body><img src=\"%1\" width=\"540\" height=\"120\"></body></html>")
        .arg(QUrl::fromLocalFile(svgPath).toString());
    KHTMLPart *partImg = new KHTMLPart();
    partImg->setJScriptEnabled(false);
    partImg->setOnlyLocalReferences(false);
    partImg->widget()->resize(560, 140);
    partImg->widget()->move(100, 300);
    partImg->widget()->show();
    const QByteArray imgBytes = imgHtml.toUtf8();
    partImg->begin(QUrl::fromLocalFile(dir + "/probe_svg_page.html"));
    partImg->write(imgBytes.constData(), imgBytes.size());
    partImg->end();

    // --- Control: same matrix as HTML <div>s, distinguishing "capture is
    // broken" from "SVG pipeline is broken" when sampling. ---
    const QString ctrlHtml = QStringLiteral(
        "<!DOCTYPE html><html><head><style>"
        "body{margin:0}.d{width:65px;height:90px;position:absolute;top:10px}"
        ".f{background:#01BCD4}.g{background:#F25022}"
        "</style></head><body>"
        "<div class=\"d\" style=\"left:10px;background:#f25022\"></div>"
        "<div class=\"d\" style=\"left:85px;background:#F25022\"></div>"
        "<div class=\"d\" style=\"left:160px;background:#3E4757\"></div>"
        "<div class=\"d\" style=\"left:235px;background:#7FBA00\"></div>"
        "<div class=\"d\" style=\"left:310px;background:rgb(255,185,0)\"></div>"
        "<div class=\"f d\" style=\"left:385px\"></div>"
        "<div class=\"g d\" style=\"left:460px\"></div>"
        "</body></html>");
    KHTMLPart *partCtrl = new KHTMLPart();
    partCtrl->setJScriptEnabled(false);
    partCtrl->widget()->resize(560, 140);
    partCtrl->widget()->move(100, 500);
    partCtrl->widget()->show();
    const QByteArray ctrlBytes = ctrlHtml.toUtf8();
    partCtrl->begin(QUrl::fromLocalFile(dir + "/probe_svg_ctrl.html"));
    partCtrl->write(ctrlBytes.constData(), ctrlBytes.size());
    partCtrl->end();

    QTimer::singleShot(2500, [&]() {
        QImage dImg = renderPart(partDoc);
        dImg.save(dir + "/probe_svg_doc.png");
        dumpColors(rec, "[doc]", dImg);

        QImage iImg = renderPart(partImg);
        iImg.save(dir + "/probe_svg_img.png");
        dumpColors(rec, "[img]", iImg);

        QImage cImg = renderPart(partCtrl);
        cImg.save(dir + "/probe_svg_ctrl.png");
        dumpColors(rec, "[ctrl]", cImg);

        rec << (g_allOk ? "RESULT: PASS" : "RESULT: FAIL") << Qt::endl;
        rec.flush();
        QCoreApplication::quit();
    });
    QTimer::singleShot(30000, [&]() {
        rec << "global timeout" << Qt::endl;
        rec.flush();
        QCoreApplication::exit(3);
    });

    return app.exec();
}
