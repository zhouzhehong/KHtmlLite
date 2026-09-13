# KJS Architecture Audit — KHtmlLite

> 第一轮源码审计，只调查不修改。基于 KHTML 5.116 实际源码。
> 标记：[KEEP] 保留 / [ADAPTER] 需适配层 / [REPLACE] 需替换 / [UNKNOWN] 待确认

---

## 1. 总体架构

```
KHTMLPart (khtml_part.cpp)
  └── ChildFrame (khtml_childframe_p.h)
        └── KJSProxy *m_jscript          [KEEP] 唯一 JS 入口
              ├── KJS::ScriptInterpreter *m_script   [REPLACE] 旧 KJS 解释器
              └── khtml::QJsDomBridge *m_qjs         [ADAPTER] 当前 QuickJS 桥
```

KJSProxy 是 KHTML 与 JavaScript 之间的**唯一接口**。所有 `<script>` 执行、inline event handler、timer 回调都经过 KJSProxy。

---

## 2. KJS 初始化链路

### 2.1 KJSProxy 创建 [KEEP]

| 位置 | 触发时机 |
|------|----------|
| `khtml_part.cpp:1210` | KHTMLPart 初始化时 |
| `khtml_part.cpp:5537` | 子 frame 创建时 |

```cpp
d->m_frame->m_jscript = new KJSProxy(d->m_frame);
```

KJSProxy 构造时 `m_script = nullptr`，`m_qjs = nullptr`，**懒加载**。

### 2.2 KJS Interpreter 创建 [REPLACE]

`kjs_proxy.cpp:359 initScript()`：

```
KJSProxy::initScript()
  ├── JSGlobalObject *globalObject = new Window(m_frame)   // Window 是 global object
  ├── m_script = new KJS::ScriptInterpreter(globalObject, m_frame)
  ├── globalObject->setPrototype(m_script->builtinObjectPrototype())
  ├── globalObject->put("debug", new TestFunctionImp(), Internal)
  └── applyUserAgent()  // IE/Netscape 兼容模式
```

`Window`（kjs_window.cpp, 117KB）是 KJS 的全局对象，包含：
- setTimeout/setInterval/clearTimeout/clearInterval
- alert/confirm/prompt/open/close
- addEventListener/removeEventListener
- onload/onclick/... 全部事件 handler 属性
- location/history/navigator/screen/console
- parent/top/self/window/frames
- getComputedStyle

### 2.3 QuickJS Runtime 创建 [ADAPTER]

`qjs_dom_bridge.cpp:548 Impl()`：

```
QJsDomBridge::Impl()
  ├── rt = JS_NewRuntime()
  ├── ctx = JS_NewContext(rt)
  ├── JS_SetMemoryLimit(rt, 64MB)
  └── buildProtos()
        ├── JS_NewClassID(nodeClassId) + JS_NewClass(KHTMLNode)
        ├── 注册 Node/Element/Document 方法和 getter/setter
        ├── JS_NewClassID(styleClassId) + JS_NewClass(KHTMLStyle)
        └── 注册 52 个 CSS 属性 magic getter/setter
```

**注意**：QuickJS Runtime 在第一次 `evaluateQuickJS()` 时才创建（懒加载），与 KJS Interpreter 独立。

---

## 3. JavaScript 执行入口

### 3.1 `<script>` 执行 [KEEP 入口 / REPLACE 后端]

统一入口：`KHTMLPart::executeScript()` (khtml_part.cpp:1334)

```
KHTMLPart::executeScript(filename, baseLine, node, script)
  └── KJSProxy::evaluate(filename, baseLine, script, node, &comp)
        ├── [优先] evaluateQuickJS(script, filename, node, &qjsResult)
        │     ├── 脚本 >1MiB → 直接回退 KJS
        │     ├── KHTML_QJS_RUNTIME=0 → 直接回退 KJS
        │     ├── m_qjs = new QJsDomBridge() (首次)
        │     ├── m_qjs->evaluate(script, filename, doc)
        │     │     ├── setDocument(doc) → 可能 resetRuntime()
        │     │     └── JS_Eval(ctx, ...)
        │     └── m_qjs->needsFallback()? → false=成功, true=回退
        └── [回退] initScript() + m_script->evaluate(filename, baseLine, code, thisNode)
```

调用点：
| 位置 | 场景 |
|------|------|
| `html_headimpl.cpp:591` | 外部 `<script src>` 加载完成后 |
| `khtml_part.cpp:1347` | `executeScript(filename, baseLine, node, script)` 通用入口 |
| `khtml_part.cpp:1395` | `executeScript(node, script)` inline code（filename 为 null） |

### 3.2 Inline Event Handler [REPLACE — 当前始终用 KJS]

`kjs_proxy.cpp:278 createHTMLEventHandler()`：

```cpp
DOM::EventListener *KJSProxy::createHTMLEventHandler(sourceUrl, name, code, node, svg)
{
    initScript();  // 强制初始化 KJS！
    return KJS::Window::retrieveWindow(m_frame->m_part)
        ->getJSLazyEventListener(code, sourceUrl, m_handlerLineno, name, node, svg);
}
```

**关键问题**：inline event handler（`onclick="..."`、`<a href="javascript:...">`）**始终使用 KJS**，QuickJS 完全不参与。这是 QuickJS 替代 KJS 的最大缺口之一。

### 3.3 setTimeout / setInterval [REPLACE — KJS Window 实现]

定义在 `kjs_window.cpp:444-450`，由 `Window::SetTimeout` / `Window::SetInterval` 实现。

内部机制（kjs_window.cpp:2552+）：
- `ScheduledAction` 类封装 code/function + nextTime + interval + timerId
- `Window::timerEvent()` 驱动定时器回调
- 回调执行时通过 `KJSProxy::evaluate()` 或直接调用 KJS function

**QuickJS 桥中没有 setTimeout/setInterval**。页面脚本如果在 QuickJS 中调用 `setTimeout`，会得到 `undefined`，不报错但不执行。

### 3.4 Event Dispatch [REPLACE — KJS 事件系统]

DOM 事件 → KJS 的调用链：
```
DOM Event dispatch
  └── NodeImpl::dispatchEventListener()
        └── KJS::JSEventListener::handleEvent()  (kjs_events.cpp)
              └── KJS function call via ScriptInterpreter
```

QuickJS 桥中 `addEventListener` 是 **no-op**（qjs_dom_bridge.cpp:283），不实际注册回调。

---

## 4. KJS ↔ DOM 绑定机制

### 4.1 KJS DOM 包装 [REPLACE]

`kjs_binding.h/cpp` 提供基类：
- `DOMObject : public JSObject` — 所有 DOM 包装对象的基类
- `DOMFunction : public InternalFunctionImp` — DOM 方法
- `getDOMNode(exec, handle)` — 从 DOM handle 获取/创建 KJS wrapper
- `ScriptInterpreter::forgetDOMObject(handle)` — 缓存管理

每个 DOM 类型有专门的 KJS 包装类：
| 文件 | 大小 | 覆盖 |
|------|------|------|
| kjs_dom.cpp | 93KB | Node, Element, Document, NodeList, NamedNodeMap |
| kjs_html.cpp | 155KB | HTMLElement, HTMLDocument, 所有 HTML 元素 |
| kjs_window.cpp | 118KB | Window, Location, History, Navigator, Screen, Console |
| kjs_css.cpp | 53KB | CSSStyleDeclaration, CSSValue, CSSRule |
| kjs_events.cpp | 45KB | Event, MouseEvent, KeyboardEvent, EventListener |
| kjs_navigator.cpp | 26KB | Navigator |
| kjs_range.cpp | 18KB | Range, Selection |
| kjs_traversal.cpp | 13KB | NodeIterator, TreeWalker |
| kjs_xmlhttprequest | 29KB | XMLHttpRequest |
| kjs_context2d.cpp | 31KB | Canvas 2D |
| kjs_webgl.cpp | 25KB | WebGL |

### 4.2 QuickJS DOM 包装 [ADAPTER]

`qjs_dom_bridge.cpp` 当前覆盖：
- **Node**: nodeName, parentNode, firstChild, lastChild, nextSibling, previousSibling, childNodes, nodeType
- **Element**: id, className, style, textContent, tagName, innerHTML, setAttribute, getAttribute, appendChild
- **Document**: getElementById, querySelector, createElement, documentElement, body, title
- **Style**: 52 个 CSS 属性 getter/setter + setProperty + getPropertyValue
- **Event**: addEventListener (no-op), removeEventListener (no-op)

**缺失**：Window, Location, History, Navigator, Console, setTimeout, Event 对象, MouseEvent, XMLHttpRequest, Canvas, Promise, fetch, URL, MutationObserver

### 4.3 包装对象生命周期 [KEEP 原则]

KJS 方式：DOM handle → KJS wrapper 缓存，`forgetDOMObject()` 在 DOM 对象销毁时清理。

QuickJS 方式（已修复）：
- `NodeWrap { Node node; }` — 保存稳定的 Node 引用
- `StyleWrap { Node node; }` — 不保存 CSSStyleDeclaration*，按需从 node 获取
- `resetRuntime()` 在 document handle 变化时销毁整个 JSRuntime，避免 stale wrapper

---

## 5. 页面导航生命周期

### 5.1 页面切换时 KJS 清理 [KEEP]

`khtml_part.cpp:1593`（在 `KHTMLPart::clear()` 或 `begin()` 中）：
```cpp
if (d->m_frame && d->m_frame->m_jscript) {
    d->m_frame->m_jscript->clear();
}
```

`KJSProxy::clear()` (kjs_proxy.cpp:237)：
```
m_script->clear()                    // 清除 interpreter 状态
Window::clear(exec)                  // 清除 Window 属性
Interpreter::collect() 循环          // GC 直到无对象可回收
```

**注意**：`clear()` 只清 KJS，**不清 QuickJS bridge**（m_qjs 不 reset）。

### 5.2 QuickJS Runtime 重置 [ADAPTER]

`QJsDomBridge::Impl::setDocument()` (qjs_dom_bridge.cpp:584)：
```
void *h = doc.handle();
if (h && h != lastDocHandle) {
    lastDocHandle = h;
    resetRuntime();  // 销毁旧 JSRuntime/JSContext，创建新的
}
documentObj = wrapNode(ctx, doc);
global.document = documentObj;
global.window = documentObj;
```

`resetRuntime()` 严格顺序（已修复 documentObj 泄漏）：
1. JS_FreeValue(ctx, documentObj)
2. JS_FreeContext(ctx)
3. JS_FreeRuntime(rt)
4. JS_NewRuntime()
5. JS_NewContext()
6. buildProtos()

### 5.3 Frame/Window 与 KJS [KEEP]

每个 ChildFrame 有独立的 KJSProxy。主 frame 和 iframe 各有独立的 KJS Interpreter 和 QuickJS Runtime（如果创建了）。

---

## 6. KJS 对现代 JS 的支持程度

### 6.1 语法支持 [REPLACE]

KJS（KF5JS，基于 2008 年左右的 JavaScriptCore）：
- ES3 完整支持
- ES5 部分支持（strict mode 有限）
- ES6+：**基本不支持**（let/const/arrow/class/Promise/Map/Set/Symbol 均无或不完整）
- 无 async/await
- 无 BigInt

QuickJS 2026-06-04：
- ES2020 完整支持
- Promise, async/await, Map, Set, Symbol, BigInt
- 可选链、空值合并、动态 import

### 6.2 Microtask / Promise [REPLACE]

KJS：无 Promise，无 microtask queue。
QuickJS：有 Promise 内置，但 `runMicrotasks()` 需要在事件循环中手动调用。当前 `QJsDomBridge` **没有调用 `JS_ExecutePendingJob()`**，所以 Promise `.then()` 回调不会执行。

### 6.3 Timer [REPLACE]

KJS：完整的 setTimeout/setInterval，由 Window::timerEvent 驱动。
QuickJS：无 timer 实现。

---

## 7. 已存在但未接入的 Adapter 基础设施 [ADAPTER]

### 7.1 JSEngine 抽象基类 (jsengine.h)

```cpp
class JSEngine {
    enum Backend { QuickJS, KJS };
    static std::unique_ptr<JSEngine> create(Backend);
    virtual std::unique_ptr<JSContext> createContext() = 0;
    virtual void setMemoryLimit(size_t) = 0;
    virtual size_t memoryUsage() const = 0;
    virtual size_t objectCount() const = 0;
    virtual Backend backend() const = 0;
};

class JSContext {
    virtual evaluate(source, sourceUrl) = 0;
    virtual runMicrotasks() = 0;
    virtual defineGlobalFunction(name, fn) = 0;
    virtual globalProperty(name) = 0;
    virtual throwError(message) = 0;
    virtual lastException() const = 0;
    virtual collectGarbage() = 0;
};
```

### 7.2 QuickJSEngine 实现 (quickjs_engine.cpp)

已完整实现：QuickJSValue, QuickJSContext, QuickJSEngine。

### 7.3 缺失：KJSEngine

`JSEngine::create(Backend::KJS)` 没有对应实现。`JSEngine::create()` 只返回 QuickJSEngine。

### 7.4 未接入

`JSEngine`/`QuickJSEngine` **完全没有被 KJSProxy 使用**。生产路径是 KJSProxy 直接管理 `KJS::ScriptInterpreter` 和 `QJsDomBridge`。这是一个已写好但未接入的并行抽象层。

---

## 8. KHTML 直接依赖 KJS 类型的位置

| 文件 | 依赖 | 说明 |
|------|------|------|
| kjs_proxy.h | KJS::Interpreter, KJS::ScriptInterpreter, KJS::Completion, KJS::List | 核心接口 |
| kjs_proxy.cpp | KJS::Window, KJS::JSLock, KJS::Interpreter::collect() | 初始化/清理/执行 |
| khtml_part.cpp | KJSProxy, KJS::Completion | executeScript |
| khtml_childframe_p.h | KJSProxy* | 成员变量 |
| kjs_binding.h | kjs/interpreter.h, kjs/global.h, kjs/lookup.h, kjs/function.h | DOM 绑定基类 |
| kjs_events.cpp | KJS::ExecState, KJS::JSValue | 事件回调 |
| kjs_window.cpp | KJS::ExecState, KJS::JSObject, KJS::FunctionImp | Window 全局对象 |
| 所有 kjs_*.cpp | KJS 类型 | DOM 绑定实现 |

**结论**：KHTML 上层（KHTMLPart, ChildFrame）只依赖 `KJSProxy`，不直接依赖 KJS 类型。真正直接依赖 KJS 类型的是 `src/ecma/kjs_*.cpp` 这一层绑定代码。这意味着替换边界在 **KJSProxy 接口**。

---

## 9. 替换边界分析

### 核心问题

> KHTML 需要的是 KJS 本身，还是 KJS 提供的 JavaScript 抽象接口？

**答案**：KHTML 上层只需要 `KJSProxy` 提供的抽象接口。KJS 类型只在 `src/ecma/kjs_*.cpp` 绑定层内部使用。

### 最小替换边界

```
KHTMLPart / ChildFrame / html_headimpl  [不需要修改]
        │
        ▼
   KJSProxy  [ADAPTER — 后端切换点]
        │
        ├── KJS Backend (kjs_*.cpp + ScriptInterpreter)  [KEEP as fallback]
        │
        └── QuickJS Backend (qjs_dom_bridge.cpp + QuickJSEngine)  [EXPAND]
```

KJSProxy 是天然的 Adapter 插入点。不需要修改 KHTMLPart、DOM、CSS、Layout、Rendering。

### 已有但未使用的 JSEngine 抽象

`jsengine.h` 的 `JSEngine`/`JSContext` 是一个更干净的抽象，但它缺少 DOM 绑定能力（只有 evaluate/defineGlobalFunction/globalProperty）。可以考虑：
- 方案 A：扩展 KJSProxy，让它内部使用 JSEngine 抽象
- 方案 B：直接在 KJSProxy 中管理两个后端，用运行时开关切换
- 方案 C：将 QJsDomBridge 迁移到 JSEngine/JSContext 抽象上

---

## 10. 标记汇总

| 组件 | 标记 | 说明 |
|------|------|------|
| KJSProxy | [KEEP] | 唯一 JS 入口，作为 Adapter |
| KJSProxy::evaluate() | [ADAPTER] | 已支持 QuickJS 优先 + KJS 回退 |
| KJSProxy::createHTMLEventHandler() | [REPLACE] | 当前始终用 KJS，需支持 QuickJS |
| KJSProxy::clear() | [ADAPTER] | 需同时清理 QuickJS bridge |
| KJS::ScriptInterpreter | [REPLACE] | 旧解释器，长期目标替换 |
| KJS Window (kjs_window.cpp) | [REPLACE] | 全局对象，需在 QuickJS 中重建 |
| kjs_dom.cpp/h | [REPLACE] | DOM 绑定，需在 QuickJS 中重建 |
| kjs_html.cpp/h | [REPLACE] | HTML 元素绑定 |
| kjs_css.cpp/h | [ADAPTER] | CSS 绑定，QuickJS 已有部分 |
| kjs_events.cpp/h | [REPLACE] | 事件绑定 |
| kjs_navigator.cpp | [REPLACE] | Navigator |
| xmlhttprequest.cpp | [REPLACE] | XHR |
| QJsDomBridge | [ADAPTER] | 扩展而非重写 |
| JSEngine/JSContext (jsengine.h) | [ADAPTER] | 已有抽象，可利用 |
| QuickJSEngine | [ADAPTER] | 已有实现，可利用 |
| KJSEngine | [UNKNOWN] | 不存在，如需 KJS 后端需新建 |
| setTimeout/setInterval | [REPLACE] | 需在 QuickJS 中实现 |
| Promise/microtask | [REPLACE] | 需调用 JS_ExecutePendingJob |
| Console | [REPLACE] | 需在 QuickJS global 中实现 |
| inline event handler | [REPLACE] | 需支持 QuickJS 执行 |

---

## 11. 当前 QuickJS Bridge 的已知问题

1. **QuickJS 和 KJS 全局对象分离**：QuickJS 中设置的变量，KJS 看不到，反之亦然。如果一个脚本在 QuickJS 中执行，另一个在 KJS 中执行，状态不共享。
2. **QuickJS 中 `window === document`**：setDocument 把 global.window 和 global.document 都设为同一个 document wrapper。
3. **addEventListener 是 no-op**：QuickJS 注册的事件回调不会被调用。
4. **无 setTimeout**：QuickJS 中调用 setTimeout 返回 undefined。
5. **无 console**：console.log 不工作。
6. **无 Promise microtask 执行**：Promise.then 回调不会触发。
7. **QuickJS bridge 不在 KJSProxy::clear() 中重置**：依赖 setDocument 的 document handle 变化。
8. **脚本 >1MiB 直接回退 KJS**：大 bundle 始终用 KJS。
