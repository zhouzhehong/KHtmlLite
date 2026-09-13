# KJS GC Architecture — KHtmlLite

> 基于 KF5JS 5.115.0 collector.h 实际审计。
> 在实现 Map/Set/Promise/Symbol/Proxy 之前必须理解的 GC 机制。

---

## 1. GC 类型

KJS 使用**保守式标记-清除（Conservative Mark-Sweep）**垃圾回收器。

- 非分代
- 非增量
- 非并发
- Stop-the-world
- 保守式栈扫描（不精确区分指针和整数）

---

## 2. 内存分配模型

### Cell 分配
- 所有 GC 管理的对象（JSCell 子类：JSObject, StringImp, NumberImp 等）通过 `Collector::allocate()` 分配
- 64 位系统上 cell 大小 = 64 字节
- 32 位系统上 cell 大小 = 32 字节
- cell 必须是 2 的幂，且能被 8 整除

### Block 管理
- Block 大小 = 64KB (16 * 4096)
- 每个 Block 包含：
  - `CollectorCell cells[CELLS_PER_BLOCK]` — 实际 cell 数组
  - `usedCells` — 已用计数
  - `freeList` — 空闲链表
  - `marked` bitmap — 标记位图
  - `allocd` bitmap — 已分配位图
  - `trailer` bitmap — 扩展位图

### 内存限制
- `KJS_MEM_LIMIT = 500000` cells（约 32MB on 64-bit）
- `Collector::isOutOfMemory()` 检查是否超限
- `reportExtraMemoryCost(size_t)` 报告外部内存成本（如 DOM 字符串、大数组），超过 256 字节会触发更频繁的 GC

---

## 3. Mark-Sweep 流程

### Mark 阶段
```
Collector::collect()
  ├── markProtectedObjects()        // 标记 protect() 注册的 root
  ├── markCurrentThreadConservatively()  // 保守扫描当前线程栈
  ├── markOtherThreadConservatively()    // 扫描其他线程
  └── markStackObjectsConservatively()   // 栈上可能的指针
```

标记通过 bitmap 实现：`Collector::markCell(cell)` 设置对应 bit。

### Sweep 阶段
- 遍历所有 block 的所有 cell
- 未标记的已分配 cell → 调用 operator delete → 加入 freeList
- 清除 marked bitmap

### 触发时机
- `Collector::allocate()` 中空间不足时自动触发
- `Interpreter::collect()` 显式调用（KJSProxy::clear() 中循环调用直到无对象可回收）
- `reportExtraMemoryCost()` 达到阈值时

---

## 4. 对象生命周期规则

### 可以安全保存的对象

| 类型 | 保存方式 | 注意 |
|------|----------|------|
| JSValue* (immediate) | 直接保存 | 数字/布尔/null/undefined 不是 cell，不受 GC 管理 |
| JSObject* | 必须 protect() 或确保在 scope chain / 属性中被引用 | 裸指针在 GC 后可能失效 |
| JSString* / StringImp* | 同上 | 同上 |
| DOM wrapper (DOMObject*) | 通过 getDOMNode() 缓存机制管理 | KJS binding 层有 forgetDOMObject |
| Identifier | 直接保存 | Identifier 是内部化字符串，有独立引用计数 |
| UString | 直接保存 | UString 是引用计数字符串，不受 GC 管理 |

### 必须注册 root 的场景

```cpp
// C++ 局部变量持有 JSObject*，跨 GC 安全
JSObject *obj = ...;
Collector::protect(obj);
// ... 可能触发 GC 的操作 ...
Collector::unprotect(obj);

// 或者使用 KJS 的 protect 工具
ProtectedPtr<JSObject> protectedObj = obj;
```

### 不安全的场景

```cpp
// ❌ 裸指针跨 GC
JSObject *obj = JSObject::create(exec);
exec->someMethodThatMayTriggerGC();
obj->doSomething();  // obj 可能已被回收！

// ❌ 在 C++ 容器中保存裸指针
QVector<JSObject*> objects;  // GC 不会扫描 QVector
objects.append(obj);
// GC 后 objects[0] 是悬空指针
```

---

## 5. 与 DOM 绑定的交互

### KJS DOM Wrapper 生命周期
```
DOM Node (handle) ←→ KJS DOMObject wrapper
     │
     ├── getDOMNode(exec, handle) — 获取或创建 wrapper
     ├── cacheDOMObject(exec, handle, wrapper) — 缓存
     └── forgetDOMObject(handle) — DOM 对象销毁时移除缓存
```

- DOM wrapper 是 GC cell，受 GC 管理
- DOM handle 是稳定的引用（类似 shared_ptr）
- 当 DOM 对象销毁时，`forgetDOMObject()` 清除缓存，但 wrapper 对象本身由 GC 回收
- wrapper 的 finalizer 不应该访问已销毁的 DOM 对象

### 页面导航时的清理
```
KHTMLPart::clear()
  └── KJSProxy::clear()
        ├── m_script->clear()  — 清除 interpreter
        ├── Window::clear()    — 清除 Window 属性
        └── Interpreter::collect() 循环 — 强制 GC
```

---

## 6. 实现新内置对象时的 GC 注意事项

### Map/Set
- 内部数据结构（WTF::HashMap）保存 JSValue* → 必须考虑 GC 标记
- **关键问题**：Map 的 key/value 如果是 JSObject*，GC 不会自动扫描 HashMap 内部
- 解决方案：Map 对象必须实现 `markChildren()` 方法，在 GC mark 阶段遍历内部 HashMap 并标记所有 key/value
- KJS 的 JSObject 有 `mark()` 虚方法，子类可以重写

### Promise
- Promise 持有 resolve/reject 回调（JSObject*）→ 必须 mark
- Promise 链（then 返回的新 Promise）→ 必须 mark
- microtask queue 中的 pending job → 必须作为 root protect

### Symbol
- Symbol 如果是对象类型，需要 GC 管理
- Symbol 作为属性 key 时，property map 需要支持非字符串 key
- 全局 Symbol registry（Symbol.for）→ 需要静态存储，不受 GC 回收

### Proxy
- Proxy 持有 target 和 handler 对象 → 必须 mark
- Proxy trap 调用时创建的临时对象 → 注意 protect

---

## 7. GC 风险评估

| 新功能 | GC 风险 | 原因 |
|--------|---------|------|
| Object/Array builtin 方法 | LOW | 不创建新对象类型，复用现有 GC |
| template literal | LOW | 创建 StringImp，已有 GC 支持 |
| let/const | MEDIUM | 块级作用域对象需要正确的 scope chain 引用 |
| arrow function | MEDIUM | 捕获的 this/scope 需要正确引用 |
| Map/Set | HIGH | 内部容器需要 markChildren |
| Promise | HIGH | 回调链 + microtask queue 需要 root 管理 |
| Symbol | MEDIUM | 新值类型或特殊对象 |
| Proxy | VERY HIGH | 透明转发导致的引用关系复杂 |
| async/await | VERY HIGH | generator + Promise 状态机 |
| WeakMap/WeakSet | VERY HIGH | 需要 GC weak reference，当前 GC 不支持 |

---

## 8. 关键结论

1. **KJS GC 是保守式的**，C++ 栈上的指针会被保守扫描，但 C++ 容器（QVector/HashMap）内部的指针不会被扫描。
2. **新对象类型必须实现 markChildren()** 来标记内部持有的 JSValue。
3. **microtask queue 必须注册为 GC root**，否则 pending job 中的回调可能被回收。
4. **WeakMap/WeakSet 在当前 GC 上几乎不可能实现**，因为没有 weak reference 机制。
5. **页面导航时的 GC 循环**（KJSProxy::clear 中 collect() 循环）是重要的内存回收机制，新对象必须能被正确回收。
