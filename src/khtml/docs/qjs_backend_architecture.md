# QuickJS Backend Architecture — KHtmlLite

> 基于第一轮源码审计的 QuickJS 后端架构设计。
> 原则：保留 KHTML 5.116 核心架构，逐步替换 JS 执行后端，不删除 KJS。

---

## 1. 目标架构

```
KHTML 5.116
  ├── HTML Parser / DOM / CSS / Layout / Rendering / Event   [KEEP — 不修改]
  │
  └── JavaScript Layer
        │
        ├── KJSProxy  [KEEP — 唯一入口，作为 Adapter]
        │     │
        │     ├── QuickJS Backend (默认)  [EXPAND]
        │     │     ├── JSRuntime (per renderer/page)
        │     │     ├── JSContext (per page)
        │     │     ├── DOM Binding Layer (Node/Element/Document/Style/Event)
        │     │     ├── Web API Layer (Window/Location/Navigator/Console/Timer)
        │     │     └── Microtask/Promise queue
        │     │
        │     └── KJS Backend (fallback)  [KEEP]
        │           ├── ScriptInterpreter
        │           ├── Window global object
        │           └── 全部 kjs_*.cpp 绑定
        │
        └── Engine Switch: KHTML_JS_ENGINE=quickjs|kjs|auto
```

---

## 2. 与现有代码的关系

### 2.1 已有基础设施

| 组件 | 文件 | 状态 |
|------|------|------|
| JSEngine 抽象基类 | `src/ecma/jsengine.h` | 已存在，未接入 |
| QuickJSEngine 实现 | `src/ecma/quickjs_engine.cpp` | 已存在，未接入 |
| QJsDomBridge | `src/ecma/qjs_dom_bridge.cpp` | 已存在，生产使用 |
| KJSProxy | `src/ecma/kjs_proxy.cpp` | 已存在，生产使用 |

### 2.2 当前生产路径

```
KJSProxy::evaluate()
  ├── evaluateQuickJS() → QJsDomBridge::evaluate()
  │     └── 成功 → 返回结果
  └── 失败/回退 → initScript() → KJS::ScriptInterpreter::evaluate()
```

### 2.3 架构不一致

当前存在**两套 QuickJS 封装**：
1. `QJsDomBridge` — 生产使用，自带 JSRuntime/JSContext，有 DOM 绑定
2. `QuickJSEngine` / `JSEngine` — 未使用，更干净的抽象，无 DOM 绑定

**建议**：不立即合并。先在 `QJsDomBridge` 上扩展功能，因为它已经有 DOM 绑定和生产验证。`JSEngine` 抽象作为长期参考，待 QuickJS 后端成熟后再考虑统一。

---

## 3. QuickJS Runtime 生命周期

### 3.1 所有权

```
khtml_renderer.exe (一个进程)
  └── KHTMLPart (一个 page)
        └── ChildFrame
              └── KJSProxy
                    └── QJsDomBridge
                          └── Impl
                                ├── JSRuntime* rt    (一个 page 一个)
                                ├── JSContext* ctx    (一个 page 一个)
                                └── JSValue documentObj
```

**规则**：
- 每个 renderer/page session 拥有独立的 JSRuntime + JSContext
- 禁止跨 Runtime 保存 JSValue
- 页面导航时 resetRuntime() 销毁并重建

### 3.2 resetRuntime() 严格顺序（已实现）

```
1. JS_FreeValue(ctx, documentObj)   // 释放 root reference
2. JS_FreeContext(ctx)
3. JS_FreeRuntime(rt)
4. rt = JS_NewRuntime()
5. ctx = JS_NewContext(rt)
6. JS_SetMemoryLimit(rt, 64MB)
7. buildProtos()                     // 注册所有 class 和 prototype
```

### 3.3 setDocument() 触发条件

```cpp
void *h = doc.handle();
if (h && h != lastDocHandle) {
    lastDocHandle = h;
    resetRuntime();
}
```

Document handle 变化 = 页面导航 = 重置整个 QuickJS Runtime。

---

## 4. JSValue 所有权规则

### 4.1 核心原则

| 操作 | 所有权 | 后续 |
|------|--------|------|
| `JS_NewString()`, `JS_NewObject()` | owned | 必须 Free 或传给消费 API |
| `JS_Eval()` 返回值 | owned | 必须 Free |
| `JS_GetGlobalObject()` | owned | 必须 Free |
| `JS_GetProperty()` | owned | 必须 Free |
| `JS_DupValue()` | owned (新引用) | 必须 Free |
| `JS_GetException()` | owned | 必须 Free |
| `JS_ToCStringLen()` 返回 | owned (C string) | 必须 JS_FreeCString |
| `JS_GetClassProto()` | owned | 必须 Free |
| `JS_DefinePropertyGetSet()` 的 getter/setter | **consumed** | 不要再次 Free |
| `JS_SetPropertyStr()` 的 val | **consumed** | 不要再次 Free |
| `JS_SetClassProto()` 的 proto | **consumed** | 不要再次 Free |
| `JSValueConst` 参数 | borrowed | 不要 Free |
| `JS_GetOpaque()` 返回 | borrowed | 不要 Free |

### 4.2 已验证的坑

`JS_DefinePropertyGetSet(ctx, obj, atom, getter, setter, flags)` 会接管 getter/setter 的所有权。调用后再次 `JS_FreeValue(ctx, getter)` 导致 double free → 0xC0000005。

---

## 5. DOM 绑定设计

### 5.1 Wrapper 结构

```cpp
struct NodeWrap {
    DOM::Node node;   // 稳定引用，不是裸指针
};

struct StyleWrap {
    DOM::Node node;   // 不保存 CSSStyleDeclaration*，按需获取
};
```

**原则**：QuickJS wrapper 保存稳定的 KHTML owner（Node），不保存容易被导航销毁的临时对象（CSSStyleDeclaration*）。

### 5.2 Class 注册

```cpp
static JSClassID nodeClassId;
static JSClassDef nodeClassDef = {
    "KHTMLNode",
    [](JSRuntime *rt, JSValue val) {  // finalizer
        NodeWrap *w = static_cast<NodeWrap*>(JS_GetOpaque(val, nodeClassId));
        delete w;
    },
};
```

### 5.3 当前已绑定的 API

| 类别 | 方法/属性 |
|------|-----------|
| Node | nodeName, parentNode, firstChild, lastChild, nextSibling, previousSibling, childNodes, nodeType |
| Element | id, className, style, textContent, tagName, innerHTML, setAttribute, getAttribute, appendChild |
| Document | getElementById, querySelector, createElement, documentElement, body, title |
| Style | 52 个 CSS 属性 + setProperty + getPropertyValue |
| Event | addEventListener (no-op), removeEventListener (no-op) |

### 5.4 需扩展的 API（按优先级）

**P0 — 页面基础功能**
- console.log/warn/error/info
- setTimeout/setInterval/clearTimeout/clearInterval
- location (href, assign, reload)
- navigator (userAgent)

**P1 — DOM 完整性**
- Event 对象 (type, target, currentTarget, preventDefault, stopPropagation)
- MouseEvent (clientX, clientY, button)
- KeyboardEvent (key, code, keyCode)
- addEventListener 实际注册回调
- querySelectorAll
- createTextNode
- insertBefore / removeChild / replaceChild
- classList (add, remove, contains, toggle)
- dataset

**P2 — 现代 Web**
- Promise microtask execution (JS_ExecutePendingJob)
- XMLHttpRequest
- fetch (基础)
- URL / URLSearchParams
- MutationObserver
- history (pushState, replaceState, back, forward)
- localStorage / sessionStorage

---

## 6. Event Handler 架构

### 6.1 当前问题

`KJSProxy::createHTMLEventHandler()` 始终使用 KJS：
```cpp
initScript();  // 强制 KJS
return Window::getJSLazyEventListener(code, ...);
```

### 6.2 目标设计

```
KJSProxy::createHTMLEventHandler(code, node)
  ├── if (engine == QuickJS)
  │     └── 创建 QuickJSEventListener
  │           ├── 保存 code + node
  │           └── handleEvent() → QJsDomBridge::evaluate(code, "inline", doc)
  └── if (engine == KJS)
        └── KJS::JSLazyEventListener (现有)
```

QuickJSEventListener 实现 `DOM::EventListener` 接口，在事件触发时通过 QuickJS 执行代码。

---

## 7. Timer 架构

### 7.1 当前 KJS 实现

`Window::SetTimeout` → `ScheduledAction` → `Window::timerEvent()` 驱动。

### 7.2 QuickJS 实现设计

在 QJsDomBridge 的 global object 上注册：
```cpp
setTimeout(fn, delay) → QTimer::singleShot(delay, [ctx, fn]{
    JSValue argv[] = {};
    JS_Call(ctx, fn, JS_UNDEFINED, 0, argv);
    JS_FreeValue(ctx, fn);
    JS_ExecutePendingJob(ctx);  // 执行 microtask
});
```

需要管理 timer ID 和 QTimer 生命周期，页面导航时取消所有 timer。

---

## 8. Microtask / Promise

### 8.1 QuickJS 内置

QuickJS 有完整的 Promise 实现。但 `JS_Eval()` 不会自动执行 pending job。

### 8.2 执行时机

在以下时机调用 `JS_ExecutePendingJob(ctx)`：
1. 每次 `JS_Eval()` 完成后
2. 每个 timer 回调执行后
3. 每个 event handler 执行后
4. 网络请求回调执行后

```cpp
while (JS_ExecutePendingJob(ctx) > 0) {
    // 持续执行直到没有 pending job
}
```

---

## 9. Engine 开关设计

### 9.1 运行时开关

环境变量：`KHTML_JS_ENGINE=quickjs|kjs|auto`

| 值 | 行为 |
|----|------|
| `auto` (默认) | QuickJS 优先，失败回退 KJS（当前行为） |
| `quickjs` | 仅 QuickJS，不回退（用于测试覆盖率） |
| `kjs` | 仅 KJS（用于回归对比） |

### 9.2 实现位置

在 `KJSProxy` 中读取环境变量，控制 `evaluateQuickJS()` 的行为：
- `auto`：当前逻辑（QuickJS 优先，needsFallback 时回退）
- `quickjs`：跳过 KJS 回退，QuickJS 失败时记录错误但不崩溃
- `kjs`：跳过 QuickJS，直接用 KJS

---

## 10. 与 KJS 的共存策略

### 10.1 短期（第一阶段）

- QuickJS 和 KJS 共存
- QuickJS 优先，KJS 回退
- 不删除任何 KJS 代码
- 逐步扩展 QuickJS DOM 绑定覆盖度

### 10.2 中期（第二阶段）

- QuickJS 覆盖主要 DOM API 后，减少 KJS 回退频率
- 实现 QuickJS event handler 和 timer
- 实现 microtask 执行
- 大部分页面脚本在 QuickJS 中完成

### 10.3 长期（第三阶段）

- QuickJS 覆盖 KJS 的全部功能后
- `KHTML_JS_ENGINE=quickjs` 成为默认
- KJS 变为 optional fallback
- 最终才考虑移除 KJS

---

## 11. 不做的事情

1. 不删除 KJS 目录
2. 不把 QuickJS 源码复制覆盖 KJS
3. 不修改大量 KHTML DOM 类
4. 不写 Bing 专用 hack
5. 不禁用 JavaScript
6. 不把 SVG/CSS 问题与 JS 引擎替换混合
7. 不复制 Chromium/WebKit 架构
8. 不一次修改几十个文件
9. 不重新引入 parseToken 绕过 blockStack
10. 不在 ctx 已 Free 后使用 JSValue
