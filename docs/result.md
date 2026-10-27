# 通用 `Result<T, E>` 设计文档

**文件名：** `result.hpp`  
**语言标准：** C++17  
**参考模型：** C++23 `std::expected`、TartanLlama/expected  
**状态：** **FROZEN — Final Implementation Baseline**  
**交付方式：** 单头文件，一次完整实现

---

# 1. 定位、目标与核心契约

## 1.1 定位

`Result<T, E>` 用返回值显式表达操作的两种互斥结果：

```text
Success(T)
Error(E)
```

对于没有成功返回值的操作，使用：

```cpp
Result<void, E>
```

本设计面向 C++17，核心类型模型、主要 API 命名和 monadic 语义与 C++23 `std::expected` 保持一致，同时针对项目需求作有限且明确的调整：

- 非法状态访问统一 fail-fast；
- Result 永不进入 valueless 状态；
- T/E 满足条件时正确传播 triviality；
- 错误状态通过 `Unexpected<E>` / `unexpect` 显式表达；
- T/E destructor 必须 `noexcept`；
- 不支持引用 value type；
- 不依赖 C++20/23；
- 不引入第三方依赖；
- Result 自身不进行动态内存分配。

---

## 1.2 核心不变量

一个已经成功构造完成的 `Result`，任意时刻必须且只能处于：

```text
Result<T,E>
    = Value(T)
    | Error(E)
```

两种状态之一。

不存在：

```text
Empty
Disengaged
Valueless
Valueless-by-exception
```

状态。

对于非 `void` 主模板始终满足：

```text
has_value_ == true
    => T 是唯一存活的 active member

has_value_ == false
    => E 是唯一存活的 active member
```

所有：

- copy / move assignment；
- value / error assignment；
- `emplace()`；
- 跨状态转换；
- rollback；
- 异常传播；

都不得破坏该不变量。

这是整个实现的最高优先级约束。

---

## 1.3 设计目标

| 目标 | 要求 |
|---|---|
| C++17 | 不依赖 concepts、`std::expected` 或 C++20/23 |
| 单头文件 | 仅提供 `result.hpp` |
| 零第三方依赖 | 仅依赖 C++17 标准库 |
| Expected 风格 | 核心 API 与状态传播语义对齐 |
| Never-valueless | 已构造 Result 始终保持 Value 或 Error |
| Triviality propagation | 正确传播底层类型的平凡属性 |
| Fail-fast | 非法状态访问统一 `std::terminate()` |
| 显式错误通道 | 使用 `Unexpected<E>` / `unexpect` |
| 无库级堆分配 | Result 自身不动态分配 |
| constexpr 友好 | C++17 合法范围内尽可能支持 |
| 类型属性可查询 | availability / deleted / noexcept / triviality 正确反映 |
| 一次交付 | 本文即完整实现规格 |

---

## 1.4 与 `std::expected` 的关系

核心组合语义：

```text
and_then:
    T -> Result<U,E>

transform:
    T -> U

or_else:
    E -> Result<T,G>

transform_error:
    E -> G
```

其中：

```text
U 可以是 void；
T 可以通过 Result<void,E> 表示 void；
G 必须满足 Result error type 约束。
```

本设计与 C++23 `std::expected` 的有意差异：

1. `value()` 在 Error 状态下不抛 `bad_expected_access`，而是 terminate；
2. `operator*()` / `operator->()` 非法访问同样 fail-fast；
3. `error()` 在 Success 状态下 fail-fast；
4. T/E 必须 `nothrow destructible`；
5. `emplace()` 只在目标构造为 `noexcept` 时提供；
6. 跨状态 assignment 必须满足 never-valueless 的安全重初始化条件；
7. 不实现完整异构 `Result<U,G>` converting-constructor 体系；
8. 不提供 comparison、`swap()`、`error_or()` 等非核心接口；
9. 不支持 `Result<T&,E>`；
10. 本设计规定的 constructors / assignments / `make_unexpected()` 按本文规则传播 conditional `noexcept`；
11. monadic API 不增加额外 conditional `noexcept` 公共契约；
12. 额外要求正确传播指定 triviality 属性。

### Fail-fast 的原因

将 unchecked invalid access 也改为 fail-fast，可以保证：

- Debug / Release 行为一致；
- 非法访问确定性失败；
- 不继续访问错误的 union active member；
- death test 可稳定验证；
- `value()`、`error()`、`operator*()`、`operator->()` 契约统一。

因此本库定位为：

> **C++17 下与 `std::expected` 核心类型模型和组合语义兼容的 fail-fast Result。**

---

## 1.5 非目标

本版本不提供：

```text
Result<T&,E>
array/function 类型作为 T 或 E
value_or_throw()
unwrap()
operator==
swap()
std::optional 自动转换
完整 Result<U,G> converting-constructor 集
RTTI
ABI 稳定保证
Result 自身动态分配
```

需要返回引用时使用：

```cpp
Result<std::reference_wrapper<T>, E>
```

---

# 2. 类型系统与公共 API

## 2.1 标签与 `Unexpected<E>`

成功原地构造复用：

```cpp
std::in_place_t
std::in_place
```

定义错误标签：

```cpp
struct unexpect_t {
    explicit constexpr unexpect_t() = default;
};

inline constexpr unexpect_t unexpect{};
```

定义：

```cpp
template<class E>
class Unexpected;
```

典型使用：

```cpp
Result<int, Error> a = 42;

Result<int, Error> b(
    unexpect,
    Error::invalid_argument);

Result<int, Error> c =
    make_unexpected(Error::timeout);
```

明确不提供：

```cpp
Result(E error);
```

因此：

```cpp
Result<int, int> a(42);            // Success
Result<int, int> b(unexpect, 42);  // Error
```

无歧义。

---

## 2.2 `Unexpected<E>`

`Unexpected<E>` 的 E 必须满足 Result error-type 约束。

接口：

```cpp
template<class E>
class Unexpected {
public:
    using error_type = E;

    constexpr explicit
    Unexpected(const E&)
        noexcept(
            std::is_nothrow_copy_constructible_v<E>);

    constexpr explicit
    Unexpected(E&&)
        noexcept(
            std::is_nothrow_move_constructible_v<E>);

    template<class... Args>
    constexpr explicit
    Unexpected(
        std::in_place_t,
        Args&&... args)
        noexcept(
            std::is_nothrow_constructible_v<
                E,
                Args&&...>);

    constexpr E& error() & noexcept;
    constexpr const E& error() const& noexcept;
    constexpr E&& error() && noexcept;
    constexpr const E&& error() const&& noexcept;
};
```

辅助函数：

```cpp
template<class E>
constexpr auto make_unexpected(E&& error)
    noexcept(
        std::is_nothrow_constructible_v<
            Unexpected<std::decay_t<E>>,
            E&&>)
    -> Unexpected<std::decay_t<E>>;
```

语义：

```cpp
return Unexpected<std::decay_t<E>>(
    std::forward<E>(error));
```

### `constexpr` 语义

`Unexpected<E>` 与 `make_unexpected()` 保持普通 `constexpr` 模板声明。

**不根据：**

```cpp
std::is_literal_type_v<E>
```

建立两套 public API，也不引入 literal / non-literal 条件基类。

对于某个具体 E：

- 如果整个表达式满足 C++17 constant-expression 要求，则可以在常量求值中使用；
- 如果 E 不能在 C++17 常量表达式中完成对应操作，例如某些非字面量类型，则该 specialization 仍可正常用于运行时构造；
- 只是对应调用不能成为 constant expression。

因此：

```cpp
Unexpected<int>
```

可以在满足条件时用于常量表达式。

而：

```cpp
Unexpected<std::string>
```

仍是正常可用的运行时类型，不因为 public constructor 标记为 `constexpr` 而失去运行时可用性。

`make_unexpected()` 同理。

---

## 2.3 T/E 类型约束

### Error type

公开：

```cpp
template<class E>
inline constexpr bool is_result_error_v = ...;
```

`is_result_error_v<E>` 为 true，当且仅当：

```text
E 是 object type；
E 不是 array；
E 不带 const；
E 不带 volatile；
E 是 nothrow destructible；
E 不是 Unexpected<...> specialization。
```

object type 自然排除：

```text
void
reference
function
```

E 可以本身是：

```cpp
Result<U,G>
```

例如：

```cpp
Result<int, Result<void, Error>>
```

合法。

---

### Value type

非 `void` T 必须满足：

```text
T 是 object type；
T 不是 array；
T 不带 const；
T 不带 volatile；
T 是 nothrow destructible；
T 不是 std::in_place_t；
T 不是 unexpect_t；
T 不是 Unexpected<...> specialization。
```

T 可以是：

```cpp
Result<U,G>
```

例如：

```cpp
using Inner =
    Result<int, ParseError>;

using Outer =
    Result<Inner, AppError>;

Outer a(Inner{42});
Outer b(unexpect, AppError::failed);
```

均合法。

---

### 为什么要求 `nothrow destructible`

要求：

```cpp
std::is_nothrow_destructible_v<T>
std::is_nothrow_destructible_v<E>
```

因为 active member 的销毁本身属于状态转换协议。

以下操作都依赖 destructor 不失败：

```text
Value -> Error
Error -> Value
Value -> Value emplace
rollback
Result destructor
```

如果旧 active member 的 destructor 可以抛异常，就无法稳定维护：

```text
active member
discriminator
never-valueless
```

三者的一致性。

资源释放失败应通过显式：

```cpp
close()
shutdown()
flush()
commit()
```

等操作返回 Result，而不是通过 destructor 抛异常。

---

### 为什么禁止 `Unexpected<...>` 作为 T/E

`Unexpected` 是错误通道的 marker wrapper，而不是普通业务 payload。

因此禁止：

```cpp
Result<int, Unexpected<int>>
Result<Unexpected<int>, Error>
```

以避免：

- value/error marker 嵌套；
- 构造语义混淆；
- `Unexpected` 同时承担 marker 和业务 payload 两种职责。

---

## 2.4 Public traits 与类型别名

提供：

```cpp
template<class T>
inline constexpr bool is_result_v = ...;
```

对 cv-ref 归一化：

```cpp
is_result_v<Result<T,E>> == true;
is_result_v<const Result<T,E>&> == true;
```

C++17 内部定义：

```cpp
template<class T>
using remove_cvref_t =
    std::remove_cv_t<
        std::remove_reference_t<T>>;
```

内部可提供：

```cpp
is_unexpected_v<T>
is_valid_result_value_v<T>
```

等辅助 traits。

`Result<T,E>` 提供：

```cpp
using value_type      = T;
using error_type      = E;
using unexpected_type = Unexpected<E>;

template<class U>
using rebind = Result<U,E>;
```

---

## 2.5 默认构造

如果 T 可默认构造：

```cpp
Result()
    noexcept(
        std::is_nothrow_default_constructible_v<T>);
```

创建：

```text
Value(T{})
```

等价于：

```cpp
Result(std::in_place);
```

必须满足：

```cpp
std::is_default_constructible_v<Result<T,E>>
==
std::is_default_constructible_v<T>
```

具体通过：

```text
SFINAE
条件基类
其他等效 C++17 技术
```

实现，不冻结内部方式。

`Result<void,E>` 默认构造始终表示 Success。

---

## 2.6 Value 构造

提供：

```cpp
template<class U>
Result(U&& value)
    noexcept(
        std::is_nothrow_constructible_v<
            T,
            U&&>);
```

基本要求：

```cpp
std::is_constructible_v<T,U&&>
```

同时：

```text
1. remove_cvref_t<U> != 当前 Result<T,E>；

2. remove_cvref_t<U> != std::in_place_t；

3. remove_cvref_t<U> 不是 Unexpected<...>。
```

一般情况下：

> **不禁止 U 是其他 `Result<...>`。**

例如：

```cpp
using Inner = Result<int, Error1>;
using Outer = Result<Inner, Error2>;

Outer out(Inner{42});
```

必须合法。

只排除当前 `Result<T,E>` 本身，以避免 generic value constructor 抢占 copy/move constructor。

---

### `T == bool` 的精确保护

当且仅当：

```cpp
std::is_same_v<T, bool>
```

时，generic value constructor 额外要求：

```text
remove_cvref_t<U>
不是任意 Result<...> specialization。
```

例如：

```cpp
Result<int, Error1> src{42};

Result<bool, Error2> dst(src);
```

不能通过 generic value constructor 把：

```cpp
src.has_value()
```

包装成 bool payload。

---

### 为什么 bool guard 不扩大

不得扩大为：

```cpp
std::is_constructible_v<T,bool>
```

或“所有可由 bool 构造的 T”。

例如：

```cpp
struct BoolBox {
    explicit BoolBox(bool);
};
```

Result 提供的是：

```cpp
explicit operator bool() const noexcept;
```

普通函数参数的隐式转换不会形成：

```text
Result
 -> explicit operator bool()
 -> BoolBox(bool)
```

这种中间转换链。

因此：

```cpp
std::is_constructible_v<
    BoolBox,
    Result<int,Error1>>
```

应为 false。

另一方面：

```cpp
struct DirectWrapper {
    template<class U>
    explicit DirectWrapper(U&&);
};
```

若业务类型自身直接支持 Result 输入，则：

```cpp
DirectWrapper(Result<int,E1>&)
```

就是它明确声明允许的构造。

Result 层不得因为 bool guard 将这种合法行为禁止。

因此冻结：

> **bool guard 只针对精确 `T == bool`，不得扩展到一般 bool-constructible 类型。**

---

### `std::in_place_t` 的排除

value constructor 必须排除：

```cpp
std::in_place_t
```

因为：

```cpp
Result(std::in_place, ...)
```

已经承担专门的原地 value construction 语义。

generic value constructor 不得与 tag constructor 产生语义竞争。

---

### `unexpect_t` 作为普通输入

generic value constructor 不机械排除：

```cpp
unexpect_t
```

但由于 T 本身不能是 `unexpect_t`，只有普通 T 明确支持：

```cpp
T(unexpect_t)
```

时才可能作为普通 value 输入。

---

### Conditional implicit / explicit

若：

```cpp
std::is_convertible_v<U&&,T>
```

则允许隐式构造。

若仅：

```cpp
std::is_constructible_v<T,U&&>
```

成立，则必须为 `explicit`。

C++17 使用两个 SFINAE overload 实现。

---

## 2.7 原地 Value 构造

提供：

```cpp
template<class... Args>
explicit Result(
    std::in_place_t,
    Args&&... args)
    noexcept(
        std::is_nothrow_constructible_v<
            T,
            Args&&...>);
```

要求：

```cpp
std::is_constructible_v<
    T,
    Args&&...>
```

直接构造：

```cpp
T(std::forward<Args>(args)...)
```

---

## 2.8 Error 构造

### `unexpect`

提供：

```cpp
template<class... Args>
explicit Result(
    unexpect_t,
    Args&&... args)
    noexcept(
        std::is_nothrow_constructible_v<
            E,
            Args&&...>);
```

要求：

```cpp
std::is_constructible_v<
    E,
    Args&&...>
```

内部：

```cpp
E(std::forward<Args>(args)...)
```

---

### `Unexpected<G>`

提供：

```cpp
template<class G>
Result(const Unexpected<G>&);

template<class G>
Result(Unexpected<G>&&);
```

#### `const Unexpected<G>&`

要求：

```cpp
std::is_constructible_v<
    E,
    const G&>
```

conditional `noexcept`：

```cpp
std::is_nothrow_constructible_v<
    E,
    const G&>
```

若：

```cpp
std::is_convertible_v<
    const G&,
    E>
```

允许 implicit。

否则必须为 `explicit`。

#### `Unexpected<G>&&`

要求：

```cpp
std::is_constructible_v<
    E,
    G&&>
```

conditional `noexcept`：

```cpp
std::is_nothrow_constructible_v<
    E,
    G&&>
```

若：

```cpp
std::is_convertible_v<
    G&&,
    E>
```

允许 implicit。

否则必须为 `explicit`。

C++17 使用两个 SFINAE overload 表达 conditional explicit。

---

## 2.9 Copy / Move construction

提供：

```cpp
Result(const Result&);
Result(Result&&);
```

### Copy constructor

仅当：

```cpp
std::is_copy_constructible_v<T> &&
std::is_copy_constructible_v<E>
```

时存在。

否则：

```cpp
std::is_copy_constructible_v<
    Result<T,E>>
```

必须为 false。

### Move constructor

仅当：

```cpp
std::is_move_constructible_v<T> &&
std::is_move_constructible_v<E>
```

时存在。

conditional `noexcept`：

```cpp
std::is_nothrow_move_constructible_v<T> &&
std::is_nothrow_move_constructible_v<E>
```

移动后源 Result：

```text
保持原 Success/Error 逻辑状态；
不会进入 Empty；
内部 T/E 可以处于正常 moved-from 状态。
```

---

## 2.10 Assignment

提供：

```cpp
Result& operator=(const Result&);
Result& operator=(Result&&);

template<class U>
Result& operator=(U&& value);

template<class G>
Result& operator=(const Unexpected<G>&);

template<class G>
Result& operator=(Unexpected<G>&&);
```

所有 assignment 必须保持：

```text
never-valueless
```

---

### Copy assignment

要求：

```cpp
std::is_copy_constructible_v<T>
std::is_copy_assignable_v<T>

std::is_copy_constructible_v<E>
std::is_copy_assignable_v<E>
```

并且：

```cpp
std::is_nothrow_move_constructible_v<T> ||
std::is_nothrow_move_constructible_v<E>
```

至少一个成立。

否则：

```cpp
std::is_copy_assignable_v<
    Result<T,E>>
```

必须为 false。

---

### Move assignment

要求：

```cpp
std::is_move_constructible_v<T>
std::is_move_assignable_v<T>

std::is_move_constructible_v<E>
std::is_move_assignable_v<E>
```

且：

```cpp
std::is_nothrow_move_constructible_v<T> ||
std::is_nothrow_move_constructible_v<E>
```

至少一个成立。

conditional `noexcept`：

```cpp
std::is_nothrow_move_constructible_v<T> &&
std::is_nothrow_move_assignable_v<T> &&
std::is_nothrow_move_constructible_v<E> &&
std::is_nothrow_move_assignable_v<E>
```

---

### Value assignment `operator=(U&&)`

提供：

```cpp
template<class U>
Result& operator=(U&& value);
```

仅当以下全部成立：

```text
1. remove_cvref_t<U> != 当前 Result<T,E>；

2. remove_cvref_t<U> 不是 Unexpected<...>；

3. std::is_constructible_v<T,U&&>；

4. std::is_assignable_v<T&,U&&>；

5. 以下至少一个成立：
   - std::is_nothrow_constructible_v<T,U&&>
   - std::is_nothrow_move_constructible_v<T>
   - std::is_nothrow_move_constructible_v<E>
```

第 4 条保证：

```text
Value -> Value
```

第 3、5 条保证：

```text
Error -> Value
```

可以安全 reinit。

conditional `noexcept`：

```cpp
std::is_nothrow_assignable_v<T&,U&&> &&
std::is_nothrow_constructible_v<T,U&&>
```

---

### 为什么 assignment 不排除 `std::in_place_t`

constructor 与 assignment 在这里有意不对称。

constructor 已有：

```cpp
Result(std::in_place_t, Args&&...)
```

所以 generic value constructor 必须排除 `std::in_place_t`。

assignment 不存在：

```cpp
operator=(std::in_place_t, ...)
```

这种专用原地赋值 API。

因此若 T 自身确实支持：

```cpp
T(std::in_place_t)
T& operator=(std::in_place_t)
```

则：

```cpp
r = std::in_place_t{};
```

可作为普通 value assignment。

`unexpect_t` 同理。

---

### Value assignment 的编译期保守性

assignment overload availability 是**编译期类型属性**，不能依据 Result 当前运行时状态启用。

例如：

```cpp
Result<T,E> r(std::in_place);
```

即使当前确定是 Value：

```cpp
r = value;
```

运行时只需要：

```text
Value -> Value
```

如果类型无法保证潜在：

```text
Error -> Value
```

路径的 safe reinit，则整个 `operator=(U&&)` 仍然不可用。

这是有意设计：

> 一个 public assignment operator 必须对调用时所有可能 Result 状态均有合法语义。

---

### Error assignment `operator=(Unexpected<G>)`

定义：

```text
GF = const G&
```

对应：

```cpp
operator=(const Unexpected<G>&)
```

以及：

```text
GF = G&&
```

对应：

```cpp
operator=(Unexpected<G>&&)
```

要求：

```cpp
std::is_constructible_v<E,GF> &&
std::is_assignable_v<E&,GF>
```

并且以下至少一个成立：

```cpp
std::is_nothrow_constructible_v<E,GF> ||
std::is_nothrow_move_constructible_v<T> ||
std::is_nothrow_move_constructible_v<E>
```

状态：

```text
Error -> Error
    E assignment

Value -> Error
    safe reinit
```

conditional `noexcept`：

```cpp
std::is_nothrow_assignable_v<E&,GF> &&
std::is_nothrow_constructible_v<E,GF>
```

---

### Assignment 能力限制

即使：

```text
T copyable
E copyable
```

如果没有 safe cross-state reinit 路径，Result assignment 仍可能不存在。

用户可以：

1. 为 T 或 E 提供语义正确的 `noexcept` move constructor；
2. 在目标 construction 为 `noexcept` 时使用 `emplace()`；
3. 在更高层重新构造 Result；
4. 重建拥有 Result 的外层对象或容器元素。

---

## 2.11 状态访问

提供：

```cpp
constexpr bool has_value() const noexcept;

constexpr explicit
operator bool() const noexcept;
```

Value：

```cpp
T& value() & noexcept;
const T& value() const& noexcept;
T&& value() && noexcept;
const T&& value() const&& noexcept;

T& operator*() & noexcept;
const T& operator*() const& noexcept;
T&& operator*() && noexcept;
const T&& operator*() const&& noexcept;

T* operator->() noexcept;
const T* operator->() const noexcept;
```

Error：

```cpp
E& error() & noexcept;
const E& error() const& noexcept;
E&& error() && noexcept;
const E&& error() const&& noexcept;
```

非法状态统一：

```cpp
detail::fail_fast();
```

---

## 2.12 `value_or`

提供：

```cpp
template<class U>
T value_or(U&& default_value) const&;

template<class U>
T value_or(U&& default_value) &&;
```

Success：

```text
const& -> copy T
&&     -> move T
```

Error：

```text
从 default_value 构造 T
```

对应 overload 只有在所需 copy / move / construct 条件满足时可用。

---

## 2.13 `emplace`

提供：

```cpp
template<class... Args>
T& emplace(Args&&... args) noexcept;
```

仅当：

```cpp
std::is_nothrow_constructible_v<
    T,
    Args&&...>
```

成立。

### Value → Value

```text
destroy old T
construct new T
保持 Success
```

### Error → Value

```text
destroy E
construct T
切换为 Success
```

由于目标 construction `noexcept`，不存在销毁旧 active member 后失败的窗口。

---

## 2.14 `Result<void,E>`

表示：

```text
Success
Error(E)
```

Success 状态没有 value object。

构造：

```cpp
Result() noexcept;

explicit Result(
    std::in_place_t) noexcept;

template<class... Args>
explicit Result(
    unexpect_t,
    Args&&... args)
    noexcept(
        std::is_nothrow_constructible_v<
            E,
            Args&&...>);
```

从 `Unexpected<G>` 的：

```text
constructibility
conditional explicit
conditional noexcept
```

规则与主模板一致。

状态：

```cpp
constexpr bool has_value() const noexcept;

constexpr explicit
operator bool() const noexcept;
```

访问：

```cpp
void value() const noexcept;
void operator*() const noexcept;

E& error() & noexcept;
const E& error() const& noexcept;
E&& error() && noexcept;
const E&& error() const&& noexcept;
```

`value()` 和 `operator*()`：

```text
Success -> no-op
Error   -> fail-fast
```

提供：

```cpp
void emplace() noexcept;
```

行为：

```text
Error -> Success:
    destroy E

Success -> Success:
    no-op
```

不提供：

```text
operator->
value_or
```

---

### `Result<void,E>` monadic API

必须完整提供：

```cpp
template<class F>
auto and_then(F&& f) &;

template<class F>
auto and_then(F&& f) const&;

template<class F>
auto and_then(F&& f) &&;

template<class F>
auto and_then(F&& f) const&&;
```

其中：

```text
F() -> Result<U,E>
```

U 可以为 void。

---

必须完整提供：

```cpp
template<class F>
auto transform(F&& f) &;

template<class F>
auto transform(F&& f) const&;

template<class F>
auto transform(F&& f) &&;

template<class F>
auto transform(F&& f) const&&;
```

其中：

```text
F() -> U
```

返回：

```cpp
Result<U,E>
```

U 可以为 void。

---

必须完整提供：

```cpp
template<class F>
auto or_else(F&& f) &;

template<class F>
auto or_else(F&& f) const&;

template<class F>
auto or_else(F&& f) &&;

template<class F>
auto or_else(F&& f) const&&;
```

要求：

```text
F(E-ref) -> Result<void,G>
```

---

必须完整提供：

```cpp
template<class F>
auto transform_error(F&& f) &;

template<class F>
auto transform_error(F&& f) const&;

template<class F>
auto transform_error(F&& f) &&;

template<class F>
auto transform_error(F&& f) const&&;
```

返回：

```cpp
Result<void,G>
```

---

### `Result<void,E>` assignment 矩阵

| 操作 | Availability | Conditional `noexcept` |
|---|---|---|
| Copy assignment | `is_copy_constructible_v<E> && is_copy_assignable_v<E>` | `is_nothrow_copy_constructible_v<E> && is_nothrow_copy_assignable_v<E>` |
| Move assignment | `is_move_constructible_v<E> && is_move_assignable_v<E>` | `is_nothrow_move_constructible_v<E> && is_nothrow_move_assignable_v<E>` |
| `Unexpected<G>` assignment | `is_constructible_v<E,GF> && is_assignable_v<E&,GF>` | `is_nothrow_constructible_v<E,GF> && is_nothrow_assignable_v<E&,GF>` |

其中：

```text
GF = const G&
或
GF = G&&
```

状态矩阵：

```text
Success -> Success
    no-op

Success -> Error
    construct E

Error -> Success
    destroy E

Error -> Error
    assign E
```

Success 没有 active value object，因此 `Success -> Error` construction 抛异常时 Result 仍然保持 Success。

---

# 3. 存储、生命周期与特殊成员函数

## 3.1 存储结构

概念布局：

```text
Result<T,E>
├── union
│   ├── T value_
│   └── E error_
└── bool has_value_
```

Result 自身不得进行动态内存分配。

`Result<void,E>`：

```text
Result<void,E>
├── union
│   └── E error_
└── bool has_value_
```

Success 状态没有 active E。

---

## 3.2 跨状态 `reinit`

跨状态：

```text
Value -> Error
Error -> Value
```

必须通过统一安全 reinitialization 机制完成。

### Strategy A：New construction `noexcept`

若：

```cpp
std::is_nothrow_constructible_v<
    New,
    Args...>
```

则：

```text
destroy Old
construct New
update discriminator
```

---

### Strategy B：New 可 nothrow move

若目标 construction 可能抛，但：

```cpp
std::is_nothrow_move_constructible_v<New>
```

先：

```text
New tmp(args...)
```

Old 仍存活。

tmp construction 失败：

```text
Result 完全不变。
```

成功：

```text
destroy Old
move tmp -> union
update discriminator
```

---

### Strategy C：Old 可 nothrow move

若 A/B 不可用，但：

```cpp
std::is_nothrow_move_constructible_v<Old>
```

则：

```text
Old backup(std::move(old))
destroy old
```

然后：

```text
construct New(args...)
```

若目标 construction 抛：

```text
restore Old from backup
```

恢复不得抛。

原异常继续传播。

---

### 固定优先级

```text
Strategy A
    ↓
Strategy B
    ↓
Strategy C
```

1. A 可用必须选择 A；
2. A 不可用而 B 可用，选择 B；
3. A/B 不可用而 C 可用，选择 C；
4. 三种均不可用，对应跨状态 API 不存在。

---

### 禁止实现

严禁：

```text
destroy Old
construct New throws
Result 无 active member
```

任何外部可观察路径都不得产生 zero-active-member Result。

---

## 3.3 `reinit` 职责

必须保证：

```text
成功：
    New 是唯一 active member。

失败：
    Old 仍是唯一 active member。

任何 return / exception：
    Result 都有且只有一个合法状态。
```

冻结：

```text
语义
异常保证
A/B/C 优先级
```

不冻结：

```text
helper 名称
函数签名
内部模板拆分
```

---

## 3.4 同状态 assignment

Value → Value：

```cpp
value_ = ...
```

assignment 抛异常后：

```text
T 仍存活
Result 仍为 Success
```

Error → Error：

```cpp
error_ = ...
```

异常后：

```text
E 仍存活
Result 仍为 Error
```

Result 只承诺：

```text
basic guarantee
+
never-valueless
```

---

## 3.5 Triviality propagation

实现不得统一手写所有特殊成员。

推荐：

```text
Result
└── AssignBase
    └── ConstructBase
        └── DestructBase
            └── Storage
                └── StorageUnion
```

各层根据 traits 选择：

```text
default
delete
custom
```

### 核心保证

若：

```cpp
std::is_trivially_copyable_v<T> &&
std::is_trivially_copyable_v<E>
```

则必须：

```cpp
std::is_trivially_copyable_v<
    Result<T,E>>
```

对于 void：

```cpp
std::is_trivially_copyable_v<E>
```

必须推出：

```cpp
std::is_trivially_copyable_v<
    Result<void,E>>
```

---

### Eligible special members

不得把 trivially-copyable 错误解释为：

```text
trivial copy ctor
AND
trivial move ctor
AND
trivial copy assignment
AND
trivial move assignment
AND
trivial destructor
```

全部必须成立。

例如：

```cpp
struct MoveOnly {
    int value;

    MoveOnly() = default;

    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(
        const MoveOnly&) = delete;

    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(
        MoveOnly&&) = default;
};
```

即使 copy operation 被删除，该类型仍可能 trivially-copyable。

---

## 3.6 特殊成员属性

以下均属于 public type semantics：

```text
availability
deleted state
triviality
noexcept
```

type traits 查询必须准确。

---

## 3.7 `Result<void,E>` 特殊成员

copy constructor 只依赖：

```cpp
std::is_copy_constructible_v<E>
```

move constructor 只依赖：

```cpp
std::is_move_constructible_v<E>
```

assignment availability / noexcept 按第 2.14 节矩阵。

---

## 3.8 `sizeof`

不规定固定 ABI 或绝对 `sizeof`。

典型情况下：

```text
Result<int,ErrorCode>
≈ 8 bytes
```

但不属于 portable contract。

不得无条件：

```cpp
static_assert(
    sizeof(Result<int,E>) == 8);
```

公共存储要求：

```text
T/E 共用 union
Result 无自身 heap allocation
额外成本主要是 discriminator + padding
```

---

## 3.9 异常语义

Result 不使用异常表达正常业务失败。

非法状态访问：

```cpp
result.value();
```

直接 terminate。

但：

```text
T/E constructor
T/E assignment
用户 callable
```

仍可按普通 C++ 规则抛异常。

异常传播后已有 Result 必须仍为：

```text
Value
或
Error
```

---

# 4. Monadic API、约束与 Fail-fast

## 4.1 cv-ref overload 与传播

四个 operation：

```cpp
and_then
transform
or_else
transform_error
```

全部提供：

```text
&
const&
&&
const&&
```

参数传播：

```text
&        -> T& / E&
const&   -> const T& / const E&
&&       -> T&& / E&&
const&&  -> const T&& / const E&&
```

使用：

```cpp
std::invoke
std::invoke_result_t
```

分析 callable。

同时必须保证未被转换的一侧能够根据当前 cv-ref 正确传播。

例如：

```text
and_then / transform：
    Error 路径需要从当前 E cv-ref
    构造结果中的 E。

or_else / transform_error：
    Success 路径需要从当前 T cv-ref
    构造结果中的 T。
```

若对应 copy/move construction 不存在，则该 cv-ref overload 不可用。

---

## 4.2 Monadic API 的异常规范

本版本**不为 monadic API 提供 conditional `noexcept` 公共契约**。

即以下接口不要求根据 callable、T/E 传播或目标 Result construction 自动推导 `noexcept`：

```text
and_then
transform
or_else
transform_error
```

原因：

1. 每个 operation 都存在 Success / Error 两条运行时分支；
2. `noexcept` 必须同时覆盖 callable invocation 和未变更 alternative 的传播；
3. `transform` / `transform_error` 还涉及目标 U/G construction；
4. 为四个 cv-ref overload 建立完整 conditional-noexcept 矩阵会显著扩大实现和测试复杂度；
5. 该能力不是本版本 Result 的核心目标。

因此：

> monadic callable、T/E propagation 或目标 Result construction 产生的异常，按普通 C++ 异常规则传播。

实现不得向用户承诺本文未冻结的 monadic conditional `noexcept`。

---

## 4.3 `and_then`

语义：

```text
Result<T,E>
+
F(T) -> Result<U,E>

=
Result<U,E>
```

Success：

```text
调用 F(value)
```

Error：

```text
不调用 F
传播 E
```

E 不允许改变。

U 可以为 void。

`Result<void,E>`：

```text
F() -> Result<U,E>
```

---

## 4.4 `transform`

语义：

```text
Result<T,E>
+
F(T) -> U

=
Result<U,E>
```

允许：

```text
T -> U
T -> void
void -> U
void -> void
```

例如：

```cpp
Result<void,E>{}
    .transform([] {
        return 42;
    });
```

得到：

```cpp
Result<int,E>
```

### 引用返回禁止

F 返回：

```text
U&
const U&
U&&
const U&&
```

必须编译失败。

不得产生：

```cpp
Result<U&,E>
```

---

## 4.5 `or_else`

语义：

```text
Result<T,E>
+
F(E) -> Result<T,G>

=
Result<T,G>
```

value type 保持 T。

G 必须满足：

```cpp
is_result_error_v<G>
```

非法包括：

```text
void
reference
array
cv-qualified invalid error type
Unexpected<...>
potentially-throwing-destructor type
```

对于 void：

```text
Result<void,E>
+
F(E) -> Result<void,G>
```

---

## 4.6 `transform_error`

语义：

```text
Result<T,E>
+
F(E) -> G

=
Result<T,G>
```

G 必须满足：

```cpp
is_result_error_v<G>
```

Success 传播 T，Error 调用 F。

---

## 4.7 Monadic 实现与诊断策略

constructors、assignment 和 special members 的 availability 必须通过：

```text
SFINAE
条件 base
= delete
```

等机制真实影响 traits。

monadic operation 使用：

> **`auto` return type deduction + 分阶段 trait 检查 + 函数体 `static_assert`**

例如：

```cpp
template<class F>
auto and_then(F&& f) &&
{
    using Arg = T&&;

    static_assert(
        std::is_invocable_v<F,Arg>,
        "Result::and_then requires "
        "an invocable callable");

    if constexpr (
        std::is_invocable_v<F,Arg>) {

        using FRet =
            std::invoke_result_t<F,Arg>;

        static_assert(
            is_result_v<FRet>,
            "Result::and_then requires "
            "F to return Result<U,E>");

        if constexpr (
            is_result_v<FRet>) {

            static_assert(
                std::is_same_v<
                    typename remove_cvref_t<
                        FRet>::error_type,
                    E>,
                "Result::and_then cannot "
                "change error_type");
        }
    }
}
```

必须避免：

```cpp
auto and_then(F&&)
    -> Result<
        typename std::invoke_result_t<
            F,T&&>::value_type,
        E>;
```

这种提前实例化非法成员类型的声明。

---

## 4.8 Fail-fast

统一：

```cpp
namespace detail {

[[noreturn]]
inline void fail_fast() noexcept {
    std::terminate();
}

}
```

固定行为：

| API | 非法状态 | 行为 |
|---|---|---|
| `Result<T,E>::value()` | Error | terminate |
| `Result<T,E>::operator*()` | Error | terminate |
| `Result<T,E>::operator->()` | Error | terminate |
| `Result<T,E>::error()` | Value | terminate |
| `Result<void,E>::value()` | Error | terminate |
| `Result<void,E>::operator*()` | Error | terminate |
| `Result<void,E>::error()` | Success | terminate |

Debug / Release 完全一致。

不得只使用：

```cpp
assert(...)
```

---

# 5. 文件组织、测试与最终冻结规则

## 5.1 文件组织

推荐：

```text
result.hpp

1. includes / namespace
2. tags
3. Unexpected<E>
4. forward declarations
5. public traits
6. detail
7. Result<T,E>
8. Result<void,E>
9. make_unexpected
```

detail 推荐包含：

```text
remove_cvref_t
type traits
fail_fast
StorageUnion
Storage
DestructBase
ConstructBase
AssignBase
reinit machinery
```

标准库依赖建议：

```cpp
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
```

确有需要时：

```cpp
#include <initializer_list>
```

不得引入第三方库。

---

## 5.2 Base hierarchy

推荐：

```text
Result
└── AssignBase
    └── ConstructBase
        └── DestructBase
            └── Storage
                └── StorageUnion
```

职责：

```text
StorageUnion
    实际 union storage

Storage
    construct / destroy
    discriminator

DestructBase
    trivial/custom destructor

ConstructBase
    copy/move constructor

AssignBase
    copy/move assignment
    same-state assignment
    reinit

Result
    public API
```

不得在 public Result 层再实现第二套生命周期协议。

---

## 5.3 `constexpr`

原则：

> 在 C++17 标准允许的位置尽可能声明 `constexpr`，但不得为了 constexpr 覆盖改变对象生命周期设计。

尤其：

```text
Unexpected<E> constructors
Unexpected<E>::error()
make_unexpected()
has_value()
operator bool()
```

可以按本文接口声明 `constexpr`。

对具体 specialization：

- constant evaluation 是否成立由实际 T/E 操作和 C++17 常量表达式规则决定；
- 不要求通过 `is_literal_type` 为 public API 建立两套声明；
- 非 constant-evaluable specialization 仍可正常用于运行时。

---

## 5.4 代码规模

预计：

```text
约 600～1000 行
```

不含测试。

不是验收指标。

---

## 5.5 编译期类型测试

至少覆盖：

```text
Result<int, enum>
Result<void, enum>
Result<std::string, enum>

nested Result as T
Result as E

T == E

T == bool + U is Result

move-only
copy-only
deleted-copy
deleted-move
non-trivial
trivial move-only
```

验证：

```cpp
std::is_default_constructible_v
std::is_copy_constructible_v
std::is_move_constructible_v
std::is_copy_assignable_v
std::is_move_assignable_v
std::is_constructible_v
std::is_nothrow_constructible_v
std::is_nothrow_move_constructible_v
std::is_nothrow_move_assignable_v
std::is_trivially_copyable_v
std::is_trivially_destructible_v
```

---

### `constexpr` regression

至少验证：

```cpp
constexpr auto u =
    make_unexpected(42);

static_assert(
    u.error() == 42);
```

同时验证非 constant-evaluable E：

```cpp
auto u =
    make_unexpected(
        std::string{"error"});
```

能够正常用于运行时。

不得要求：

```text
std::string
```

在 C++17 中进行常量求值。

---

### Bool guard

```cpp
using Source =
    Result<int, Error1>;

static_assert(
    !std::is_constructible_v<
        Result<bool, Error2>,
        Source&>);
```

普通 bool-wrapper：

```cpp
struct BoolBox {
    explicit BoolBox(bool);
};

static_assert(
    !std::is_constructible_v<
        BoolBox,
        Source&>);
```

直接接受 Result：

```cpp
struct DirectWrapper {
    template<class U>
    explicit DirectWrapper(U&&);
};

static_assert(
    std::is_constructible_v<
        DirectWrapper,
        Source&>);
```

---

### Tag 边界

验证：

```text
value constructor 排除 std::in_place_t

value assignment 不机械排除 std::in_place_t

value assignment 不机械排除 unexpect_t

bool guard 不误伤非 bool 业务类型
```

---

## 5.6 Triviality 测试

至少：

```cpp
enum class ErrorCode : int {
    ok,
    failed
};

static_assert(
    std::is_trivially_copyable_v<
        Result<int,ErrorCode>>);

static_assert(
    std::is_trivially_copyable_v<
        Result<void,ErrorCode>>);

static_assert(
    !std::is_trivially_copyable_v<
        Result<std::string,ErrorCode>>);
```

并覆盖：

```text
trivial/trivial
trivial move-only
deleted copy
deleted move
non-trivial destructor
void variants
```

non-trivial destructor 仍必须：

```text
nothrow destructible
```

---

## 5.7 构造与 Assignment 测试

覆盖：

```text
default Success

implicit value
explicit value

std::in_place
unexpect

Unexpected<G>
    implicit
    explicit
    noexcept

make_unexpected
    lvalue
    rvalue
    constexpr-capable
    runtime-only E
    noexcept

copy/move Success
copy/move Error

T == E
nested Result
Result as E
bool guard
```

Assignment 矩阵：

```text
Value -> Value
Value -> Error
Error -> Value
Error -> Error
```

额外覆盖：

```text
value assignment
Unexpected assignment
assignment unavailable
conditional noexcept
runtime Value 但 compile-time assignment unavailable
void assignment matrix
self copy
self move
```

---

## 5.8 生命周期与异常注入

instrumented type 至少维护：

```text
live_count
construction_count
destruction_count
copy_count
move_count
instance id
alive flag
```

constructor 成功：

```text
live_count += 1
```

destructor：

```text
live_count -= 1
```

destructor 必须 `noexcept`。

注入异常于：

```text
constructor
conversion constructor
copy constructor
move constructor
assignment
```

不得从 destructor 注入异常。

---

### Error → Value rollback

目标 construction 抛异常后：

```text
has_value() == false
原 error 可访问
live_count 正确
Result 可继续使用
```

---

### Value → Error rollback

目标 construction 抛异常后：

```text
has_value() == true
原 value 可访问
live_count 正确
Result 可继续使用
```

最终：

```text
live_count == 0
```

并检测：

```text
double destruction
missing destruction
inactive-member access
zero active member
两个逻辑 active member
```

---

## 5.9 Reinit 策略测试

覆盖：

```text
A only
B only
C only
A+B
B+C
A+B+C
none
```

确认：

```text
A > B > C
```

并通过 constructor/move counter 验证实际路径。

---

## 5.10 `emplace` 测试

覆盖：

```text
Value -> Value
Error -> Value
```

验证：

```text
nothrow target
    -> emplace available

potentially throwing target
    -> emplace unavailable
```

void：

```text
Success.emplace() -> no-op
Error.emplace()   -> Success
```

---

## 5.11 Fail-fast death test

覆盖：

```text
Result<T,E>:
    Error.value()
    Error.operator*()
    Error.operator->()
    Success.error()

Result<void,E>:
    Error.value()
    Error.operator*()
    Success.error()
```

全部 terminate。

---

## 5.12 Monadic 测试

全部测试：

```text
&
const&
&&
const&&
```

### `and_then`

```text
Success invokes F
Error does not

T -> U
T -> void
void -> U
void -> void

E 保持一致
未变更 E 可按当前 cv-ref 传播
```

负测试：

```text
F 不可调用
F 不返回 Result
错误 error_type
E 无法传播
```

---

### `transform`

```text
T -> U
T -> void
void -> U
void -> void
Error propagation
```

负测试：

```text
reference result
E 无法传播
```

---

### `or_else`

```text
Error invokes F
Success does not

E -> G
T 保持一致
void specialization
```

负测试：

```text
错误 value_type
非法 G
T 无法传播
```

---

### `transform_error`

```text
E -> G
Success T propagation
void Success
```

负测试：

```text
非法 G
T 无法传播
```

---

### Monadic exception test

monadic API 不承诺 conditional `noexcept`。

需要验证：

```text
callable 抛异常
    -> 异常正常传播
    -> 原 Result 不被修改

传播 T/E 时构造抛异常
    -> 异常正常传播
    -> 原 Result 不被修改
```

---

## 5.13 工具链

最低：

```text
GCC 11 / C++17
Clang 14 / C++17
```

推荐额外验证：

```text
GCC 13+
Clang 18+
```

必须运行：

```text
ASan
UBSan
```

推荐组合执行。

---

## 5.14 交付物

一次性交付：

```text
result.hpp
result_test.cpp
README.md
LICENSE
```

README 至少说明：

```text
Result 定位
Success/Error 构造
Unexpected<E>
make_unexpected

constexpr 的实际含义

T == E

精确 bool guard
为何不扩大到 bool-constructible T

nested Result
Result<void,E>

never-valueless
nothrow destructor
fail-fast

conditional explicit
constructor/assignment conditional noexcept

monadic API 不承诺 conditional noexcept

assignment availability
value assignment 编译期保守性

std::in_place_t constructor/assignment
不对称处理原因

emplace

A/B/C reinit

triviality

monadic API

与 C++23 std::expected 的主要差异
```

README 必须包含 value-assignment 保守性示例：

```cpp
Result<T,E> r(std::in_place);
T value;

// 即使 r 当前是 Value，
// 如果类型无法安全执行潜在
// Error -> Value，
// operator=(U&&) 仍可能不存在。
//
// r = value;
```

---

## 5.15 最终冻结原则

以下为实现阶段不得重新设计的硬约束：

```text
1. Result 永不 valueless。

2. 已构造 Result 始终且只存在
   Value 或 Error 两种状态。

3. T/E 必须 nothrow destructible。

4. error 不通过 Result(E) 构造。

5. error 使用 Unexpected<E>
   或 unexpect 显式表达。

6. Unexpected<...> 不能作为 T/E。

7. T == E 合法。

8. T 可以是 Result。

9. E 可以是 Result。

10. generic value constructor 必须排除：
    当前 Result<T,E>、
    std::in_place_t、
    Unexpected<...>。

11. T == bool 时必须额外排除
    任意 Result specialization。

12. bool guard 不得扩大到
    一般 bool-constructible T。

13. 非 bool T 若直接支持 Result 输入，
    不得被 bool guard 误杀。

14. 除 bool 特例外，
    不得机械排除其他 Result 作为 value。

15. generic value assignment
    不得抢占当前 Result /
    Unexpected assignment。

16. value assignment 不机械排除
    std::in_place_t / unexpect_t。

17. constructor / assignment 对
    std::in_place_t 的不同处理
    是有意设计。

18. value assignment 必须满足
    constructible、assignable、
    safe-reinit。

19. value assignment availability
    是编译期属性。

20. Unexpected assignment 必须满足
    constructible、assignable、
    safe-reinit。

21. Unexpected<G> constructor
    支持 conditional implicit/explicit。

22. constructors、assignments、
    make_unexpected 按本文传播
    conditional noexcept。

23. Unexpected<E> 与 make_unexpected()
    保持普通 constexpr 模板接口；
    不通过 literal-type 特化
    创建两套 public API。

24. 具体 constexpr 调用是否能够
    constant-evaluate，
    由实际 E 和 C++17
    constant-expression 规则决定。

25. Result<void,E> copy assignment
    noexcept 按第 2.14 节矩阵。

26. Result<void,E> move assignment
    noexcept 按第 2.14 节矩阵。

27. Result<void,E> Unexpected assignment
    availability/noexcept
    按第 2.14 节矩阵。

28. and_then 只改变 value type；
    error type 保持 E。

29. and_then 的 U 可以是 void。

30. transform 可以产生 void。

31. Result<void,E> 必须完整支持
    transform 的四组 cv-ref overload。

32. or_else 可以改变 error type，
    value type 必须保持 T。

33. or_else 的 G 必须是合法 error type。

34. transform_error 只改变 error type，
    G 必须合法。

35. 所有 monadic API 提供：
    &
    const&
    &&
    const&&

36. monadic operation 必须验证
    未改变 alternative
    在当前 cv-ref 下能正确传播。

37. monadic API 不提供
    conditional noexcept 公共契约。

38. monadic callable /
    alternative propagation /
    target construction 的异常
    按普通 C++ 规则传播。

39. Result 不支持 reference value type。

40. transform 返回引用必须编译失败。

41. 所有非法 value/error/*/->
    状态访问统一 fail-fast。

42. Fail-fast Debug/Release 一致。

43. emplace 可处理
    Success 和 Error 状态。

44. 非 void emplace 仅允许
    nothrow target construction。

45. 所有跨状态操作使用
    safe reinit。

46. reinit 优先级固定：
    A > B > C。

47. 前序策略可用时不得选择
    后序策略。

48. A/B/C 均不可用时，
    相应跨状态 API 不存在。

49. 跨状态 construction 失败后
    必须恢复原逻辑状态。

50. 不允许 zero-active-member。

51. copy/move availability
    必须正确传播。

52. default constructibility
    必须正确传播。

53. delete/noexcept/triviality
    属于 public type semantics。

54. 不得通过降低异常安全
    恢复已删除 assignment。

55. 不得错误定义 trivially-copyable。

56. T/E trivially-copyable 时，
    Result 必须正确传播。

57. Base hierarchy 不得破坏
    special-member traits。

58. monadic 使用 auto return deduction
    + 分阶段诊断。

59. monadic 诊断不得提前访问
    非 Result 类型的 value_type/error_type。

60. lifetime tests 必须检测
    leak/double destruction/
    inactive-member access。

61. Result 自身无动态分配。

62. 单头文件。

63. C++17。

64. 零第三方依赖。

65. 正确性、生命周期、
    type traits、异常安全
    优先于代码行数。
```

满足上述契约并通过：

```text
compile-time trait tests
constexpr/runtime Unexpected tests
constructor tests
assignment tests
conditional explicit tests
conditional noexcept tests
bool guard tests
tag-boundary tests
reinit A/B/C tests
rollback exception tests
lifetime instrumentation
fail-fast death tests
monadic cv-ref tests
monadic propagation tests
monadic exception tests
ASan / UBSan
```

之后，`Result<T,E>` 才视为实现完成。

---

# 附录 A：实现最小清单

本附录只是冻结原则的分类索引，不增加新语义。

## A.1 状态与生命周期

```text
□ 永不 valueless
□ Value/Error 严格二态
□ discriminator 与 active member 一致
□ T/E nothrow destructible
□ 无 zero-active-member
□ 无 double destruction
□ 无 inactive-member access
```

## A.2 构造

```text
□ 默认构造传播
□ T == E
□ Unexpected / unexpect
□ nested Result
□ value constructor exclusions
□ 精确 bool guard
□ bool guard 不扩大
□ conditional implicit/explicit
□ constructor noexcept
□ Unexpected constexpr
□ make_unexpected constexpr/noexcept
```

## A.3 Assignment

```text
□ copy availability
□ move availability
□ value constructible
□ value assignable
□ safe-reinit
□ Unexpected assignment
□ compile-time conservatism
□ in_place/unexpect tag boundary
□ void assignment matrix
□ conditional noexcept
```

## A.4 Reinit

```text
□ Strategy A
□ Strategy B
□ Strategy C
□ A > B > C
□ none -> API unavailable
□ rollback 恢复原状态
```

## A.5 Triviality / Special Members

```text
□ copy traits
□ move traits
□ deleted traits
□ noexcept traits
□ trivial destructor
□ trivially-copyable
□ move-only trivial case
□ Base hierarchy 不破坏 traits
```

## A.6 Monadic API

```text
□ and_then
□ transform
□ or_else
□ transform_error

□ &
□ const&
□ &&
□ const&&

□ Result<void,E>::transform 完整存在
□ U=void 支持
□ G 合法
□ reference transform 禁止
□ unchanged alternative propagation
□ callable/type 分阶段诊断
□ 不承诺 conditional noexcept
□ callable 异常正常传播
```

## A.7 Fail-fast

```text
□ value()
□ error()
□ operator*()
□ operator->()
□ void specialization
□ Debug/Release 一致
```

## A.8 工程要求

```text
□ 单头文件
□ C++17
□ 零第三方依赖
□ 无自身 heap allocation
□ GCC 11
□ Clang 14
□ ASan
□ UBSan
□ 测试覆盖全部冻结契约
```

---

# 最终状态

**Design Review: PASSED**

**Specification Status: FROZEN**

**Next Phase: Implementation + Contract Verification**

实现阶段允许调整：

```text
detail helper 名称
内部模板拆分方式
base class 名称
私有函数签名
代码排版
测试辅助类型组织
```

但不得改变：

```text
public semantics
类型约束
constructor / assignment availability
conditional explicit
已冻结的 conditional noexcept
constexpr 接口语义
精确 bool guard
对象生命周期
never-valueless
A/B/C reinit 优先级
异常保证
特殊成员属性传播
monadic 语义
monadic noexcept policy
fail-fast 契约
```

如果实现阶段发现这些冻结契约在 C++17 类型系统下无法同时满足，应停止在实现层自行调整 public semantics，并将具体冲突重新提交设计评审。