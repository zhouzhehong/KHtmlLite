// Extract @keyframes rules from CSS using brace-matching instead of regex.
// Regex backtracking on megabyte-scale CSS was causing stack overflow crashes.
static QString extractKeyframesJs(const QString &css)
{
    if (css.size() > 65536) return QStringLiteral("{}");
    QString result = QStringLiteral("{");
    bool first = true;
    int count = 0;
    int pos = 0;
    while (count < 30 && pos < css.size()) {
        // Find next @keyframes (optionally -webkit- prefix)
        int kfPos = css.indexOf(QStringLiteral("@keyframes"), pos, Qt::CaseInsensitive);
        int wkPos = css.indexOf(QStringLiteral("@-webkit-keyframes"), pos, Qt::CaseInsensitive);
        int atPos = -1;
        if (kfPos >= 0 && (wkPos < 0 || kfPos < wkPos)) atPos = kfPos;
        else if (wkPos >= 0) atPos = wkPos;
        if (atPos < 0) break;
        // Skip past the @keyframes keyword and whitespace to get the name
        int nameStart = atPos + (wkPos == atPos ? 17 : 10);
        while (nameStart < css.size() && (css.at(nameStart) == QLatin1Char(' ') || css.at(nameStart) == QLatin1Char('\t') || css.at(nameStart) == QLatin1Char('\n')))
            nameStart++;
        int nameEnd = nameStart;
        while (nameEnd < css.size() && css.at(nameEnd) != QLatin1Char('{') && css.at(nameEnd) != QLatin1Char(' ') && css.at(nameEnd) != QLatin1Char('\t'))
            nameEnd++;
        QString name = css.mid(nameStart, nameEnd - nameStart).trimmed();
        if (name.isEmpty()) { pos = atPos + 10; continue; }
        // Find opening brace
        int braceStart = css.indexOf(QLatin1Char('{'), nameEnd);
        if (braceStart < 0) break;
        // Brace-match to find the closing brace
        int depth = 1;
        int p = braceStart + 1;
        while (p < css.size() && depth > 0) {
            QChar ch = css.at(p);
            if (ch == QLatin1Char('{')) depth++;
            else if (ch == QLatin1Char('}')) depth--;
            p++;
        }
        if (depth != 0) break;
        QString body = css.mid(braceStart + 1, p - braceStart - 2);
        // Parse keyframe selectors within the body
        QString frames;
        bool firstFrame = true;
        int bp = 0;
        while (bp < body.size()) {
            int selStart = bp;
            while (selStart < body.size() && (body.at(selStart).isSpace() || body.at(selStart) == QLatin1Char(',')))
                selStart++;
            if (selStart >= body.size()) break;
            int selEnd = body.indexOf(QLatin1Char('{'), selStart);
            if (selEnd < 0) break;
            QString sel = body.mid(selStart, selEnd - selStart).trimmed().toLower();
            // Find matching close brace for this frame
            int fd = 1, fp = selEnd + 1;
            while (fp < body.size() && fd > 0) {
                if (body.at(fp) == QLatin1Char('{')) fd++;
                else if (body.at(fp) == QLatin1Char('}')) fd--;
                fp++;
            }
            if (fd != 0) break;
            QString props = body.mid(selEnd + 1, fp - selEnd - 2).trimmed();
            double offset = 0.0;
            if (sel == QStringLiteral("from")) offset = 0.0;
            else if (sel == QStringLiteral("to")) offset = 1.0;
            else {
                bool ok = false;
                double v = sel.replace(QStringLiteral("%"), QString()).toDouble(&ok);
                if (ok) offset = v / 100.0;
            }
            props.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
            props.replace(QChar('"'), QStringLiteral("\\\""));
            props.replace(QStringLiteral("\n"), QStringLiteral(" "));
            if (!firstFrame) frames += QLatin1Char(',');
            frames += QLatin1String("{\"offset\":") + QString::number(offset)
                   + QLatin1String(",\"cssText\":\"") + props + QLatin1String("\"}");
            firstFrame = false;
            bp = fp;
        }
        if (!frames.isEmpty()) {
            if (!first) result += QLatin1Char(',');
            result += QLatin1String("\"") + name + QLatin1String("\":[") + frames + QLatin1Char(']');
            first = false;
            count++;
        }
        pos = p;
    }
    result += QLatin1Char('}');
    return result;
}
