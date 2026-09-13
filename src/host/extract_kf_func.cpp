// Extract @keyframes rules from CSS and return a JS object literal string.
static QString extractKeyframesJs(const QString &css)
{
    QString result = QStringLiteral("{");
    bool first = true;
    const QRegularExpression reKf(
        QStringLiteral("@(?:-webkit-)?keyframes\\s+([a-zA-Z0-9_-]+)\\s*\\{([^@]*)\\}"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = reKf.globalMatch(css);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString name = m.captured(1);
        const QString body = m.captured(2);
        const QRegularExpression reFrame(
            QStringLiteral("(from|to|[0-9]+%)\\s*\\{([^}]*)\\}"),
            QRegularExpression::CaseInsensitiveOption);
        auto fit = reFrame.globalMatch(body);
        QString frames;
        bool firstFrame = true;
        while (fit.hasNext()) {
            const QRegularExpressionMatch fm = fit.next();
            QString sel = fm.captured(1).toLower();
            double offset = 0.0;
            if (sel == QStringLiteral("from")) offset = 0.0;
            else if (sel == QStringLiteral("to")) offset = 1.0;
            else offset = sel.replace(QStringLiteral("%"), QString()).toDouble() / 100.0;
            QString props = fm.captured(2).trimmed();
            props.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
            props.replace(QChar('"'), QStringLiteral("\\\""));
            props.replace(QStringLiteral("\n"), QStringLiteral(" "));
            if (!firstFrame) frames += QLatin1Char(',');
            frames += QLatin1String("{\"offset\":") + QString::number(offset)
                   + QLatin1String(",\"cssText\":\"") + props + QLatin1String("\"}");
            firstFrame = false;
        }
        if (frames.isEmpty()) continue;
        if (!first) result += QLatin1Char(',');
        result += QLatin1String("\"") + name + QLatin1String("\":[") + frames + QLatin1Char(']');
        first = false;
    }
    result += QLatin1Char('}');
    return result;
}
