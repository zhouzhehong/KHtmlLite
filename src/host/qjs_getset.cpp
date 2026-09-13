// qjs_getset.cpp -- minimal getter/setter registration test.
// Fixed: JS_DefinePropertyGetSet consumes getter/setter (JSValue, not
// JSValueConst), so no manual JS_FreeValue after it. Also expose obj to
// global so the eval script actually invokes the C getter.
#include <cstdio>
#include <cstring>

extern "C" {
#include "quickjs.h"
}

static JSValue myGetter(JSContext *ctx, JSValueConst this_val, int magic)
{
    return JS_NewInt32(ctx, magic * 10);
}

static JSValue mySetter(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    return JS_UNDEFINED;
}

int main()
{
    printf("getter_magic = %d, setter_magic = %d\n",
           (int)JS_CFUNC_getter_magic, (int)JS_CFUNC_setter_magic);
    fflush(stdout);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    if (!rt || !ctx) {
        printf("FAIL: alloc\n");
        return 1;
    }
    JS_SetMemoryLimit(rt, 64 * 1024 * 1024);

    JSValue obj = JS_NewObject(ctx);
    const char *props[] = { "alpha", "beta", "gamma" };
    for (int i = 0; i < 3; ++i) {
        JSValue getFn = JS_NewCFunction2(ctx, reinterpret_cast<JSCFunction *>(myGetter),
                                         props[i], 0, JS_CFUNC_getter_magic, i);
        JSValue setFn = JS_NewCFunction2(ctx, reinterpret_cast<JSCFunction *>(mySetter),
                                         props[i], 0, JS_CFUNC_setter_magic, i);
        JSAtom atom = JS_NewAtom(ctx, props[i]);
        JS_DefinePropertyGetSet(ctx, obj, atom, getFn, setFn, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, atom);
        // getFn and setFn are consumed by JS_DefinePropertyGetSet -- do NOT free.
        printf("prop %s registered\n", props[i]);
        fflush(stdout);
    }

    // Expose obj to global so eval can reach it.
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "obj", JS_DupValue(ctx, obj));
    JS_FreeValue(ctx, global);

    const char *script = "obj.alpha + ',' + obj.beta + ',' + obj.gamma";
    JSValue r = JS_Eval(ctx, script, strlen(script), "getset", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, exc);
        printf("EXCEPTION: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, exc);
    } else {
        const char *s = JS_ToCString(ctx, r);
        printf("RESULT: %s\n", s ? s : "(no str)");
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, obj);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("DONE\n");
    fflush(stdout);
    return 0;
}
