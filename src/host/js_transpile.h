#ifndef JS_TRANSPILE_H
#define JS_TRANSPILE_H

#include <QString>

// Convert ES2015+ source into ES3 that KJS can parse, using an embedded
// QuickJS runtime + Buble. Returns the original string untouched when the
// input is already ES3 or the transform fails for any reason.
QString transpileJs(const QString &source);

// Quick check used to skip the (relatively expensive) transform path for
// scripts that contain no ES2015+ syntax at all.
bool looksLikeEs6(const QString &source);

#endif
