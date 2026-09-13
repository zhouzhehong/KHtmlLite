// qjs_smoke.cpp — verify the QuickJS engine embedded in libKF5KHtml.dll
// executes ES2020 syntax (optional chaining, nullish coalescing, BigInt,
// Promise.allSettled). Links ONLY against the DLL exports; no separate
// libquickjs.a.
#include <cstdio>
#include <cstring>

extern "C" {
#include "quickjs.h"
}

int main()
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    if (!rt || !ctx) {
        printf("FAIL: runtime/context alloc\n");
        return 1;
    }
    JS_SetMemoryLimit(rt, 64 * 1024 * 1024);

    // ES2020 feature set
    const char *tests[] = {
        "const a = {b: {c: 42}}; a?.b?.c === 42 ? 'optional-chaining-ok' : 'fail'",
        "const v = null ?? 'default'; v === 'default' ? 'nullish-ok' : 'fail'",
        "const big = 123456789012345678901234567890n; typeof big === 'bigint' ? 'bigint-ok' : 'fail'",
        "Promise.allSettled([Promise.resolve(1), Promise.reject(2)]).then(r => r.length === 2 ? 'allsettled-ok' : 'fail')",
        "const m = new Map([['k', 'v']]); m.get('k') === 'v' ? 'map-ok' : 'fail'",
        "const s = new Set([1,2,3]); s.has(2) ? 'set-ok' : 'fail'",
        "[1,2,3].flatMap(x => [x, x*2]).length === 6 ? 'flatmap-ok' : 'fail'",
        "String.prototype.matchAll ? 'matchall-ok' : 'fail'",
        "Object.fromEntries([['a',1]])['a'] === 1 ? 'fromentries-ok' : 'fail'",
        "Promise.any([Promise.reject(1), Promise.resolve(2)]).then(v => v === 2 ? 'promiseany-ok' : 'fail')",
        "const {a = 1} = {}; a === 1 ? 'destructure-ok' : 'fail'",
        "class C { #x = 5; get() { return this.#x; } } new C().get() === 5 ? 'private-field-ok' : 'fail'",
    };

    int pass = 0, total = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < total; ++i) {
        JSValue r = JS_Eval(ctx, tests[i], strlen(tests[i]), "smoke", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, exc);
            printf("[%d] EXCEPTION: %s\n", i + 1, m ? m : "?");
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, exc);
        } else {
            const char *s = JS_ToCString(ctx, r);
            const bool ok = s && strstr(s, "-ok") != nullptr;
            printf("[%d] %s => %s\n", i + 1, ok ? "PASS" : "FAIL", s ? s : "(no str)");
            if (ok) ++pass;
            if (s) JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, r);
    }

    // Run pending promise jobs (allSettled/any resolve asynchronously)
    JSContext *ctx1;
    int err;
    while ((err = JS_ExecutePendingJob(rt, &ctx1)) > 0) {}
    if (err < 0) {
        JSValue exc = JS_GetException(ctx1);
        const char *m = JS_ToCString(ctx1, exc);
        printf("job drain exception: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx1, m);
        JS_FreeValue(ctx1, exc);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("\n%d/%d ES2020 features OK\n", pass, total);
    return pass == total ? 0 : 2;
}
