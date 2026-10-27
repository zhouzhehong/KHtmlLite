/*
    Unit test for simplifyUnsupportedCss() (src/host/css_unsupported.h).

    Every case asserts:
      * the unsupported property declaration is gone;
      * the surrounding rules are intact;
      * '{' and '}' counts are still balanced.

    Exit code 0 = PASS, 1 = FAIL.  tests/css_trim_probe.py wraps this for the
    regression suite.
*/
#include "css_unsupported.h"

#include <QString>
#include <cstdio>

static int failures = 0;

static int braceDelta(const QString &s)
{
    // Braces inside quoted strings are content, not structure.
    int n = 0;
    bool inStr = false;
    bool escaped = false;
    QChar quote;
    for (const QChar &c : s) {
        if (inStr) {
            if (escaped) escaped = false;
            else if (c == QLatin1Char('\\')) escaped = true;
            else if (c == quote) inStr = false;
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            inStr = true;
            quote = c;
        } else if (c == QLatin1Char('{')) {
            ++n;
        } else if (c == QLatin1Char('}')) {
            --n;
        }
    }
    return n;
}

static void expect(const char *name, const QString &in, const QString &want,
                   const QString &mustNotContain)
{
    const QString got = simplifyUnsupportedCss(in);
    bool ok = (got == want);
    if (!ok) {
        std::printf("FAIL %s\n  in  : %s\n  want: %s\n  got : %s\n",
                    name, qUtf8Printable(in), qUtf8Printable(want),
                    qUtf8Printable(got));
    }
    if (!mustNotContain.isEmpty() && got.contains(mustNotContain,
                                                  Qt::CaseInsensitive)) {
        std::printf("FAIL %s: output still contains \"%s\": %s\n", name,
                    qUtf8Printable(mustNotContain), qUtf8Printable(got));
        ok = false;
    }
    if (braceDelta(got) != 0) {
        std::printf("FAIL %s: brace imbalance (%d): %s\n", name,
                    braceDelta(got), qUtf8Printable(got));
        ok = false;
    }
    if (braceDelta(in) != braceDelta(want)) {
        std::printf("FAIL %s: test case itself is unbalanced\n", name);
        ok = false;
    }
    if (ok) {
        std::printf("OK   %s -> %s\n", name, qUtf8Printable(got));
    } else {
        ++failures;
    }
}

int main()
{
    // The four cases requested for the regression test.
    expect("filter-first", QStringLiteral(".a{filter:x;color:red}.b{color:blue}"),
           QStringLiteral(".a{color:red}.b{color:blue}"), QStringLiteral("filter"));
    expect("filter-last", QStringLiteral(".a{color:red;filter:x}.b{color:blue}"),
           QStringLiteral(".a{color:red;}.b{color:blue}"), QStringLiteral("filter"));
    expect("object-fit", QStringLiteral(".a{object-fit:cover}.b{color:blue}"),
           QStringLiteral(".a{}.b{color:blue}"), QStringLiteral("object-fit"));
    expect("media-block",
           QStringLiteral("@media (min-width:600px){.a{inset:0}.b{will-change:x}}.c{color:blue}"),
           QStringLiteral("@media (min-width:600px){.a{}.b{}}.c{color:blue}"),
           QStringLiteral("will-change"));

    // Known residual before the scanner: a ';' inside a data: URI used to
    // truncate the declaration and leave the rest of the value as garbage.
    expect("data-uri-value",
           QStringLiteral(".a{mask-image:url(data:image/png;base64,AAA);color:red}"),
           QStringLiteral(".a{color:red}"), QStringLiteral("mask-image"));
    // Same for quoted values.
    expect("quoted-semicolon",
           QStringLiteral(".a{filter:\"a;b\";color:red}"),
           QStringLiteral(".a{color:red}"), QStringLiteral("filter"));
    // Semicolon inside quotes + braces must stay balanced.
    expect("quoted-brace",
           QStringLiteral(".a{will-change:\"x}y\";color:red}"),
           QStringLiteral(".a{color:red}"), QStringLiteral("will-change"));

    if (failures == 0) {
        std::printf("CSS TRIM PROBE PASS\n");
        return 0;
    }
    std::printf("CSS TRIM PROBE FAIL: %d case(s)\n", failures);
    return 1;
}
