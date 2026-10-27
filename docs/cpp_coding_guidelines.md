# C++17 Coding Guidelines

**Language:** C++17
**Reference Compiler:** GCC 11.x
**Reference Formatter:** clang-format 14.x
**Reference Static Analyzer:** clang-tidy 14.x
**Scope:** Library、Runtime、Middleware、Driver、Application、Tool、Test
**Status:** Development Baseline

本文档定义通用 C++17 项目的代码格式、命名规则、文件组织、API 设计、Ownership、生命周期、错误处理、异常边界、并发设计、注释、测试、静态分析以及代码质量要求。

本文档是**项目无关的通用规范**。具体项目可以在此基础上增加自己的 Project Conventions，例如：

```text
namespace 约束
项目专用错误类型
Public API 组织
特定生命周期
特定 Middleware 边界
特定 ABI / Protocol 规则
```

但项目级规范不应无理由重新定义与本文冲突的基础编码风格。

本文档明确区分四类规则：

```text
Formatting Rules
→ clang-format 自动执行

Static Code Rules
→ compiler / clang-tidy / compile tests 执行

Runtime Correctness Rules
→ unit / contract / sanitizer / stress tests 验证

Architecture / Semantic Rules
→ code review + design contract 验证
```

不能把无法由 clang-format 保证的规则误写成格式化规则；也不能把需要设计证明的并发、Ownership 或错误语义问题假装成单纯静态检查问题。

---

# 1. 基础原则与格式规范

## 1.1 总体设计原则

所有 C++17 项目统一遵循以下原则：

> **Correctness before cleverness.**

> **Clarity before terseness.**

> **Explicit ownership before implicit lifetime assumptions.**

> **Explicit state before combinations of unrelated boolean flags.**

> **Rule of Zero before manual special-member management.**

> **Measured optimization before speculative optimization.**

> **RAII before manual resource management.**

> **Small stable APIs before convenience API proliferation.**

> **Tests verify contracts, not merely implementation lines.**

> **Tooling enforces mechanical rules; review verifies semantics.**

代码首先应该让阅读者能够较快回答：

```text
这段代码完成什么职责？

它为什么存在？

谁拥有这个对象？

谁只是借用这个对象？

这个对象能够存活多久？

失败以后对象处于什么状态？

调用者应该如何处理失败？

这里有哪些线程可以访问？

哪些状态由锁保护？

哪个操作具有线性化点？

Shutdown 时正在执行的操作怎么办？

Timeout 是操作终态还是调用方停止等待？

析构时会不会阻塞？

第三方异常能否穿过本模块边界？
```

如果这些问题必须依赖作者口头解释才能回答，通常说明接口、数据结构或实现仍然不够清晰。

---

## 1.2 C++ 标准固定为 C++17

所有采用本规范的项目统一使用：

```text
C++17
```

不以：

```text
C++17 or later
```

作为兼容目标。

推荐 CMake：

```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

`CMAKE_CXX_EXTENSIONS OFF` 用于避免无意依赖：

```text
gnu++17
```

扩展能力。

代码不得依赖 C++20 或后续标准才提供的能力，例如：

```text
concepts
requires
std::span
std::source_location
coroutines
std::format
std::jthread
std::stop_token
std::atomic_ref
```

如果某项能力确实需要，应选择：

```text
C++17 可实现的明确替代方案

或

经过评审、边界清晰的第三方组件
```

不能在个别 target 中偷偷切换到：

```text
C++20
C++23
GNU extension mode
```

形成语言标准碎片化。

---

## 1.3 标准 C++ 优先

优先使用：

```text
ISO C++17
标准库
清晰的平台适配层
```

避免无必要使用：

```text
GNU language extension
compiler-specific syntax
非标准 VLA
statement expression
未隔离的 compiler builtin
平台私有 API 泄漏到通用代码
```

确实需要平台能力时，应形成明确边界，例如：

```text
platform/linux/
platform/windows/
platform/posix/
```

或者项目定义的对应 abstraction。

平台差异应该：

```text
局部化
可测试
不污染核心 API
```

---

## 1.4 工具版本基线

代码格式具有版本相关性。

本规范参考：

```text
clang-format 14.x
clang-tidy 14.x
GCC 11.x
```

其中：

```text
clang-format 14.x
```

是格式输出的参考基线。

项目 CI 应尽量固定 formatter 的具体 package/version。

不要出现：

```text
开发机 clang-format 14
CI clang-format 18
另一个开发者 clang-format 22
```

然后因为版本差异反复产生格式变更。

静态检查参考：

```text
clang-tidy 14.x
```

主要编译验证：

```text
GCC 11.x
```

项目可以增加：

```text
Clang
其他 GCC minor version
其他目标平台 compiler
```

作为额外验证，但不能改变 C++17 基线。

---

## 1.5 clang-format 是唯一格式化标准

统一 `.clang-format`：

```yaml
---
Language: Cpp
BasedOnStyle: Google

ColumnLimit: 100
IndentWidth: 4
ContinuationIndentWidth: 4
AccessModifierOffset: -4
AlignAfterOpenBracket: AlwaysBreak
BreakBeforeBraces: Attach
ConstructorInitializerIndentWidth: 0
DerivePointerAlignment: false
PointerAlignment: Left
ReflowComments: true
IncludeBlocks: Preserve
SortIncludes: false
```

格式最终以：

```text
clang-format 14.x
```

实际结果为准。

不要再建立一套“人工视觉格式规范”。

例如不要规定：

```text
每个参数必须一行
所有函数声明必须换行
所有 initializer 必须一行一个
```

如果 clang-format 本身不会稳定产生这种格式。

---

## 1.6 ColumnLimit

统一：

```text
ColumnLimit: 100
```

100 列是 formatter 的主要换行目标，不是绝对语义限制。

以下内容允许自然超过：

```text
无法合理拆开的 URL
长测试数据
外部协议字符串
编译期 diagnostic
第三方要求的固定文本
```

不要为了机械满足 100 列制造：

```text
难以阅读的字符串拼接
奇怪的临时变量
无意义的函数拆分
```

---

## 1.7 `AlignAfterOpenBracket: AlwaysBreak` 的准确含义

该选项并不表示：

```text
所有参数逐个换行
```

也不表示：

```text
左括号后无条件换行
```

如果代码能够放入 100 列，clang-format 14 可以保持：

```cpp
Status create_connection(std::string_view address, std::uint16_t port, Duration timeout);
```

对于较长声明，formatter 才会进行拆分。

例如：

```cpp
Status create_connection_with_a_long_descriptive_name(
    std::string_view address, std::uint16_t port, Duration timeout);
```

或者根据具体 token 长度产生其他合法布局。

因此：

> **参数换行完全以 clang-format 14.x 实际结果为准。**

文档示例不应反过来创建额外人工格式要求。

---

## 1.8 缩进

统一：

```text
IndentWidth: 4
ContinuationIndentWidth: 4
```

即：

```text
4 spaces
```

禁止 Tab。

例如：

```cpp
if (ready) {
    process();
}
```

---

## 1.9 Access Modifier

配置：

```yaml
AccessModifierOffset: -4
```

形成：

```cpp
class Worker {
public:
    void run();

private:
    int state_ = 0;
};
```

不要手工把：

```text
public:
private:
protected:
```

再向右缩进。

---

## 1.10 大括号规则

所有控制语句必须显式使用大括号。

正确：

```cpp
if (ready) {
    start();
}

for (const auto& item : items) {
    process(item);
}

while (running) {
    poll();
}
```

错误：

```cpp
if (ready)
    start();
```

错误：

```cpp
if (ready) start();
```

即使只有一条语句也必须有 `{}`。

原因包括：

```text
后续添加代码更安全
减少 dangling else 风险
避免 diff 中控制流误读
方便静态分析
降低维护时产生逻辑错误的概率
```

这不是 clang-format 能够保证的规则。

应由：

```text
clang-tidy:
readability-braces-around-statements
```

验证。

---

## 1.11 Brace 风格

采用：

```yaml
BreakBeforeBraces: Attach
```

例如：

```cpp
if (condition) {
    do_work();
}

class Worker {
public:
    void run();
};
```

不使用 Allman 风格：

```cpp
if (condition)
{
}
```

---

## 1.12 Pointer / Reference 风格

统一：

```cpp
Type* pointer;
const Type* pointer;

Type& reference;
const Type& reference;
```

不使用：

```cpp
Type *pointer;
Type &reference;
```

由：

```yaml
DerivePointerAlignment: false
PointerAlignment: Left
```

统一处理。

---

## 1.13 `nullptr`

空指针统一：

```cpp
nullptr
```

禁止：

```cpp
NULL
0
```

表示空指针。

例如：

```cpp
Widget* widget = nullptr;
```

---

## 1.14 类型转换

禁止 C 风格 cast：

```cpp
auto value = (TargetType)source;
```

应使用：

```cpp
static_cast<TargetType>(source);
const_cast<T*>(pointer);
reinterpret_cast<T*>(address);
dynamic_cast<T*>(object);
```

其中：

```text
static_cast
```

是正常显式转换的主要方式。

以下 cast 应相对少见：

```text
const_cast
reinterpret_cast
dynamic_cast
```

使用 `reinterpret_cast` 时必须能够解释：

```text
对象表示是否合法
alignment 是否满足
lifetime 是否成立
aliasing 是否满足
目标平台约束是什么
```

不要把：

```text
reinterpret_cast
```

当成绕过类型系统的快捷工具。

---

## 1.15 `auto`

当右侧已经清晰表达类型和语义时，推荐：

```cpp
auto result = create_connection(config);
auto iterator = entries.find(key);
auto backend = std::make_unique<Backend>();
```

以下场景倾向显式类型：

```text
整数宽度重要
signed/unsigned 重要
单位重要
时间类型重要
状态类型重要
隐式转换风险较高
API 语义仅靠类型才能理解
```

例如：

```cpp
std::uint32_t sequence = read_sequence();
Duration timeout = config.timeout();
ConnectionState state = connection.state();
```

不推荐：

```cpp
auto value = get_value();
auto result = query();
```

如果从局部上下文完全无法判断类型和语义。

---

## 1.16 `const`

合理使用：

```cpp
const auto size = entries.size();

void process(const Message& message);

std::string_view name() const noexcept;
```

应优先保护：

```text
borrowed input
只读对象
不会重新绑定的引用语义
const member function
```

但不要机械追求：

```text
const everywhere
```

例如：

```cpp
const auto value = make_large_object();
```

如果后续本应 move：

```cpp
consume(std::move(value));
```

则无意义 `const` 反而妨碍语义。

---

## 1.17 成员初始化

所有成员在对象进入可观察状态前必须具有确定值。

优先使用：

```text
in-class member initializer
```

表达字段自己的默认语义：

```cpp
class Connection {
private:
    ConnectionState state_ = ConnectionState::Created;
    std::uint64_t sequence_ = 0;
    bool enabled_ = true;
};
```

需要 constructor 参数时使用 initializer list：

```cpp
Connection::Connection(std::string name, Duration timeout)
    : name_(std::move(name)), timeout_(timeout) {}
```

避免：

```cpp
Connection::Connection(std::string name) {
    name_ = std::move(name);
}
```

如果字段本可以直接构造。

---

## 1.18 成员初始化顺序

真实初始化顺序由：

```text
成员声明顺序
```

决定，不由 initializer list 书写顺序决定。

例如：

```cpp
class Object {
private:
    int first_;
    int second_;
};
```

即使写：

```cpp
Object::Object() : second_(2), first_(1) {}
```

仍然先初始化 `first_`。

因此 initializer list 应与字段声明顺序保持一致。

避免产生：

```text
-Wreorder
```

以及错误的依赖假设。

---

## 1.19 一行一个逻辑语句

正确：

```cpp
initialize();
start();
wait();
```

错误：

```cpp
initialize(); start(); wait();
```

普通实现中不要使用逗号运算符模拟控制流。

---

## 1.20 Magic Number

具有稳定语义的固定值应命名：

```cpp
constexpr std::size_t kDefaultQueueCapacity = 8192;
constexpr std::size_t kMaxRetryCount = 3;
```

而不是散落：

```cpp
queue.reserve(8192);
```

适合命名的包括：

```text
默认容量
协议常量
retry limit
timeout default
buffer size
状态码固定值
```

不需要机械把所有测试数字都抽成常量。

---

## 1.21 Macro

Macro 应尽量少。

适合 macro 的场景：

```text
编译条件
visibility/export
source location capture
logging macro
平台 feature
```

普通常量优先：

```cpp
constexpr
```

普通函数优先：

```cpp
inline function
template
```

不要用 macro 代替正常语言机制。

---

# 2. 命名、文件与代码组织

## 2.1 命名规则总表

统一规则：

| 对象 | 风格 | 示例 |
|---|---|---|
| Namespace | `snake_case` | `motion_control` |
| Class | `PascalCase` | `LoggingContext` |
| Struct | `PascalCase` | `LoggingStats` |
| Enum class | `PascalCase` | `LogLevel` |
| Enumerator | `PascalCase` | `ResourceExhausted` |
| Function | `snake_case` | `create_backend()` |
| Member function | `snake_case` | `get_logger()` |
| Static member function | `snake_case` | `create()` |
| Local variable | `snake_case` | `backend_result` |
| Parameter | `snake_case` | `queue_capacity` |
| Public aggregate field | `snake_case` | `accepted` |
| Private/protected field | `snake_case_` | `state_` |
| Constant | `kPascalCase` | `kDefaultCapacity` |
| Macro | `UPPER_SNAKE_CASE` | `PROJECT_API` |
| File | `snake_case` | `logging_context.cpp` |
| Semantic type alias | `PascalCase` | `MessageBuffer` |
| STL-compatible nested alias | standard style | `value_type` |
| Type template parameter | `PascalCase` / `T` | `Value`, `T` |
| Non-type template parameter | `kPascalCase` | `kCapacity` |

Public 和 private function：

```text
全部使用 snake_case
```

不采用：

```text
Public PascalCase
Private snake_case
```

两套体系。

---

## 2.2 Namespace

Namespace 使用：

```text
snake_case
```

例如：

```cpp
namespace logging {
}

namespace motion_control {
}

namespace data_transport {
}
```

Namespace 应表达真实领域。

优先：

```text
serialization
execution
transport
logging
networking
memory
scheduler
protocol
```

不推荐没有真实领域意义的：

```text
utils
misc
common
helpers
stuff
```

---

## 2.3 Class / Struct

类型使用：

```text
PascalCase
```

例如：

```cpp
class Runtime;
class LoggingContext;
class Connection;

struct ConnectionOptions;
struct LoggingStats;
struct MessageInfo;
```

---

## 2.4 Enum

使用 scoped enum：

```cpp
enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};
```

优先：

```text
enum class
```

避免 unscoped enum 带来的：

```text
命名污染
隐式整数转换
作用域不明确
```

Enumerator 使用：

```text
PascalCase
```

例如：

```text
Created
Running
Stopping
Stopped
```

---

## 2.5 Function

项目自己拥有的函数统一：

```text
snake_case
```

包括：

```text
free function
member function
static member function
factory
private helper
```

例如：

```cpp
create();
initialize();
start();
stop();
shutdown();

get_logger();
set_level();
clear_category();

create_backend();
validate_config();
start_worker();
complete_request();
```

不要混用：

```text
Create()
create()
createBackend()
create_backend()
```

---

## 2.6 简单 Getter

纯属性访问通常不加 `get_`：

```cpp
std::string_view name() const noexcept;
ConnectionState state() const noexcept;
Duration timeout() const noexcept;
std::size_t size() const noexcept;
bool empty() const noexcept;
```

如果调用具有：

```text
查找
失败
registry access
资源获取
实际 query
```

则可以使用：

```cpp
Result<Logger> get_logger(std::string_view category);
Result<Value> get_value(Key key);
```

所以规则不是：

```text
禁止 get_
```

而是：

```text
simple property accessor
→ name()

real query / lookup / operation
→ get_xxx()
```

---

## 2.7 Setter

Setter 使用：

```text
set_xxx
```

例如：

```cpp
set_timeout();
set_enabled();
set_default_level();
```

如果 operation 并非简单状态设置，而是实际业务动作，则应使用更准确的动词，例如：

```text
register_handler()
install_backend()
attach_client()
replace_route()
```

---

## 2.8 Function 命名例外

本地命名规则不能破坏外部契约。

### 第三方接口 override

如果第三方 interface 要求：

```cpp
void OnMessage(...) override;
```

则 override 保留原名。

不要为了 snake_case 改成：

```cpp
void on_message(...);
```

导致不再 override。

---

### C++ 语言和标准约定

保留：

```text
operator==
operator()
operator[]
begin()
end()
swap()
size()
value_type
iterator
const_iterator
```

等标准生态名称。

---

### 生成代码

例如：

```text
Protobuf
gRPC
ROS IDL
DDS IDL
OpenAPI generator
```

生成接口保留 generator 命名。

不要人工修改生成文件。

---

### 外部 C ABI / Protocol ABI

如果 ABI 已冻结：

```text
按外部名称实现
```

不能为了内部风格破坏兼容。

---

## 2.9 Variable

局部变量：

```text
snake_case
```

例如：

```cpp
const auto backend_result = create_backend(config);
std::size_t queue_capacity = 8192;
bool should_retry = false;
```

避免：

```text
tmp
obj
data
thing
foo
bar
```

除非：

```text
循环索引
非常局部的数学表达
测试 fixture 中的临时变量
```

含义确实显而易见。

---

## 2.10 Parameter

参数使用：

```text
snake_case
```

例如：

```cpp
void set_timeout(Duration timeout);

Status write_message(
    std::string_view topic_name,
    const Message& message);
```

---

## 2.11 Public Aggregate Field

公开数据对象的字段：

```text
snake_case
```

不带 `_`。

例如：

```cpp
struct Statistics {
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
};

struct Config {
    bool enabled = true;
    std::size_t capacity = 0;
};
```

原因：

```text
public aggregate field
本身就是用户可见数据模型
```

不应伪装成 private implementation field。

---

## 2.12 Private / Protected Member

private / protected 数据成员：

```text
snake_case_
```

例如：

```cpp
class Queue {
private:
    std::size_t capacity_ = 0;
    std::vector<Item> entries_;
};
```

不使用：

```text
m_capacity
_capacity
capacity
```

表示 private field。

---

## 2.13 Boolean Naming

Boolean 名字应表达谓词。

好：

```cpp
bool enabled;
bool initialized;
bool is_running;
bool has_value;
bool should_retry;
bool can_write;
```

差：

```cpp
bool flag;
bool status;
bool state;
bool mode;
bool value;
```

---

## 2.14 Constant

命名常量：

```text
kPascalCase
```

例如：

```cpp
constexpr std::size_t kDefaultQueueCapacity = 8192;
constexpr int kMaxRetries = 3;
```

---

## 2.15 Macro Naming

Macro 使用：

```text
UPPER_SNAKE_CASE
```

项目 Macro 应有项目前缀：

```text
PROJECT_API
PROJECT_LOG_INFO
PROJECT_HAS_FEATURE
```

避免通用：

```text
DEBUG
ERROR
MAX_SIZE
CHECK
```

污染全局 macro namespace。

---

## 2.16 Type Alias

项目定义的领域 alias：

```text
PascalCase
```

例如：

```cpp
using MessageBuffer = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using Callback = std::function<void()>;
```

---

## 2.17 STL-compatible Alias

如果类型要与标准 library vocabulary 兼容，则采用：

```cpp
using value_type = T;
using size_type = std::size_t;
using iterator = ...;
using const_iterator = ...;
using reference = T&;
using const_reference = const T&;
```

不要改成：

```text
ValueType
SizeType
IteratorType
```

而破坏 C++ generic ecosystem。

---

## 2.18 Template 参数

Type template parameter：

```cpp
template <typename Value>
class Queue;
```

或者传统通用类型：

```cpp
template <typename T>
class Optional;
```

允许：

```text
T
U
E
```

用于高度通用且局部的模板。

具有领域意义时优先：

```text
Value
Allocator
Clock
Backend
Request
Response
```

---

## 2.19 Non-type Template 参数

使用：

```text
kPascalCase
```

例如：

```cpp
template <typename T, std::size_t kCapacity>
class FixedQueue;
```

---

## 2.20 File Naming

文件统一：

```text
snake_case
```

例如：

```text
logging_context.hpp
logging_context.cpp
async_queue.hpp
async_queue.cpp
connection_manager.hpp
connection_manager.cpp
```

---

## 2.21 Header / Source 后缀

统一：

```text
.hpp
.cpp
```

新项目不要无规则混用：

```text
.h
.hpp
.hh
.hxx
.cc
.cpp
```

如果已有历史项目已经稳定采用另一组后缀，则应由项目级 convention 明确。

---

## 2.22 Header 防重复包含

所有手写 Header 必须有重复包含保护。

推荐：

```cpp
#ifndef PROJECT_LOGGING_LOGGER_HPP_
#define PROJECT_LOGGING_LOGGER_HPP_

// ...

#endif  // PROJECT_LOGGING_LOGGER_HPP_
```

Include guard 名称应根据：

```text
项目
路径
文件名
```

生成。

例如：

```text
PROJECT_RUNTIME_LOGGING_CONTEXT_HPP_
```

如果某项目统一选择：

```cpp
#pragma once
```

也可以作为项目级统一约定。

但不要在同一个新代码库里随机混用：

```text
include guard
#pragma once
```

两种方案。

---

## 2.23 Header Self-contained

每个 public header 必须可以独立编译：

```cpp
#include <project/component.hpp>

int main() {
    return 0;
}
```

不能依赖：

```text
其他 header 恰好先 include
PCH
第三方 transitive include
某个特殊 include 顺序
```

---

## 2.24 Include What You Use

使用：

```cpp
std::string_view
```

应显式：

```cpp
#include <string_view>
```

使用：

```cpp
std::unique_ptr
```

应显式：

```cpp
#include <memory>
```

使用：

```cpp
std::uint64_t
```

应显式：

```cpp
#include <cstdint>
```

不要依赖 transitive include。

---

## 2.25 Forward Declaration

可以使用 forward declaration 降低 dependency，但不能为了减少 include 制造：

```text
复杂 incomplete-type lifetime
脆弱析构依赖
隐式 ABI 约束
```

例如：

```cpp
class Backend;
```

在仅保存：

```cpp
std::unique_ptr<Backend>
```

且析构在 `.cpp` 中定义时通常合理。

但如果：

```text
类型是 value member
inline function 需要完整定义
template 实例化需要完整定义
```

则必须 include。

---

## 2.26 Include 顺序

由于：

```yaml
IncludeBlocks: Preserve
SortIncludes: false
```

include 顺序由开发者维护。

`.cpp` 推荐：

```cpp
#include "own_header.hpp"

#include <standard_library>

#include <third_party/library.hpp>

#include <project/header.hpp>
```

第一条是最重要规则：

> **`.cpp` 对应自己的 header 必须第一个 include。**

这样可以尽早发现：

```text
own header 缺失依赖
隐式 transitive include
宏顺序依赖
```

---

## 2.27 Include Blocks

保留语义 block：

```cpp
#include "component.hpp"

#include <memory>
#include <string>
#include <vector>

#include <third_party/foo.hpp>

#include "runtime/state.hpp"
#include "transport/message.hpp"
```

由于：

```text
SortIncludes: false
```

不要求 block 内机械 alphabetic sort。

优先保持：

```text
依赖逻辑
阅读顺序
项目一致性
```

---

## 2.28 文件职责

文件围绕真实职责组织。

不要机械：

```text
一个 class 一个文件
```

例如：

```text
async_queue.hpp/.cpp
text_formatter.hpp/.cpp
connection_state.hpp
```

都合理。

如果几个 private 类型：

```text
只一起使用
只服务一个职责
很小
不能独立理解
```

可以放在同一文件。

---

## 2.29 避免超大文件

如果一个 `.cpp` 同时负责：

```text
config parsing
state machine
network I/O
thread scheduling
serialization
callback dispatch
error recovery
metrics
```

通常说明职责需要拆分。

但也不要为了满足“文件短”而拆成几十个碎片。

原则：

> **按独立职责拆分，不按行数拆分。**

---

## 2.30 避免万能文件

避免不断增长的：

```text
common.hpp
utils.hpp
helpers.hpp
misc.hpp
base.hpp
```

如果代码属于：

```text
time
filesystem
serialization
networking
logging
registry
scheduler
```

就放入对应领域。

---

## 2.31 `detail` / `internal` / `impl`

通用规范不绝对禁止。

例如 header-only template library：

```cpp
namespace library::detail {
}
```

隐藏模板实现是合理的。

但原则是：

> **不能把 `detail` / `internal` / `impl` 当成缺乏领域建模时的默认垃圾桶。**

如果一个 private 类型实际是：

```text
scheduler
registry
parser
serializer
queue
transport
```

应优先使用准确领域结构。

---

## 2.32 Generated Code

生成代码：

```text
不手工修改
不要求遵循全部本地命名规则
不要求通过全部手写代码 clang-tidy naming rule
```

应通过：

```text
独立目录
独立 target
HeaderFilter
clang-tidy exclusion
```

隔离。

手写 adapter 仍遵循本规范，第三方 override 名称例外。

---

## 2.33 规范覆盖旧的纯风格命名

当项目正式采用本规范后，如果旧设计文档使用：

```text
Create()
GetLogger()
SetCategoryLevel()
```

而新规范要求：

```text
create()
get_logger()
set_category_level()
```

则：

> **新规范覆盖旧的纯代码风格命名。**

这属于：

```text
风格迁移
```

不是语义重新设计。

项目应统一修改：

```text
设计文档
Public Header
测试
示例
README
```

避免长期并存两套 API 名称。

但以下仍保留原命名：

```text
第三方 override
标准接口
生成代码
外部 ABI
```

---

# 3. API、Ownership、错误处理与并发

## 3.1 Public API 保持克制

Public API 是长期维护成本最高的部分。

增加 Public API 前必须确认：

```text
调用方确实需要这个 primitive 吗？

现有 API 是否已经可以组合实现？

是不是只是为了 implementation 方便？

是否暴露了内部 state？

是否泄漏第三方类型？

未来是否难以撤回？

是否只是为未来假想场景预埋？
```

不要因为：

```text
以后可能有用
调试比较方便
接口更对称
```

就暴露：

```cpp
get_impl();
internal_state();
native_handle();
mutable_state();
```

---

## 3.2 Public API 与 Implementation 分离

Public API 应表达：

```text
稳定语义
Ownership
输入输出
错误模型
生命周期
线程安全
```

不应表达：

```text
当前 container
当前 mutex
当前第三方库
当前 thread model
当前缓存实现
```

如果实现以后可以从：

```text
std::map
```

切换到：

```text
flat structure
```

而 Public API 不需要变，这通常是良好边界。

---

## 3.3 单参数 Constructor

可能产生隐式转换的单参数 constructor 默认使用：

```cpp
explicit
```

例如：

```cpp
class Timeout {
public:
    explicit Timeout(std::chrono::milliseconds value);
};
```

只有在隐式转换满足：

```text
自然
安全
无损
符合类型语义
```

时才省略 `explicit`。

---

## 3.4 多参数 Constructor

多参数 constructor 通常不存在同样的隐式转换风险，但仍应检查：

```text
参数顺序是否清楚
是否存在多个 bool
是否应该使用 Options
是否可能失败
```

例如：

```cpp
Connection(bool a, bool b, bool c);
```

通常不如：

```cpp
ConnectionOptions options;
```

清晰。

---

## 3.5 Complex Resource 优先 Factory

简单 value object：

```cpp
ConnectionOptions options;
Identifier id(...);
```

直接构造合理。

复杂 runtime resource 如果初始化可能失败：

```text
打开文件
创建线程
连接网络
初始化 backend
注册系统资源
```

优先：

```cpp
static Result<T> create(...);
```

而不是：

```cpp
T object(...);
object.initialize();
```

避免：

```text
半初始化对象
忘记 initialize
错误状态隐藏在对象内部
```

---

## 3.6 Ownership 必须显式

优先通过类型表达：

```cpp
std::unique_ptr<T>
std::shared_ptr<T>
T&
const T&
T*
std::string_view
```

约定：

```text
unique_ptr
→ exclusive ownership

shared_ptr
→ shared ownership

T&
→ non-owning, non-null

T*
→ non-owning, nullable unless另有文档

string_view
→ borrowed character sequence
```

裸指针不用于隐式 ownership transfer。

---

## 3.7 `unique_ptr`

使用：

```cpp
std::unique_ptr<T>
```

表达：

```text
唯一 ownership
明确 transfer
自动销毁
```

创建：

```cpp
auto object = std::make_unique<Object>();
```

优先于：

```cpp
std::unique_ptr<Object> object(new Object);
```

---

## 3.8 `shared_ptr`

只在存在真实共享 lifetime 时使用。

不要因为：

```text
“不确定谁拥有”
```

就默认使用 `shared_ptr`。

使用 `shared_ptr` 应能够回答：

```text
为什么需要多个 owner？

谁可能延长 lifetime？

何时最终释放？

是否可能形成 cycle？
```

如果形成 cycle，必须明确：

```text
weak_ptr break point
```

---

## 3.9 Raw Pointer

裸指针：

```cpp
T*
```

默认表示：

```text
non-owning
nullable
```

如果 non-null 是硬约束，更适合：

```cpp
T&
```

如果 API 必须使用指针，应在文档明确：

```text
nullable?
borrowed lifetime?
caller/callee ownership?
```

---

## 3.10 Reference

`T&`：

```text
borrowed
non-null
```

`const T&`：

```text
borrowed
read-only through this reference
```

Reference 不表达 ownership。

---

## 3.11 `string_view`

`std::string_view` 只表达：

```text
borrowed character range
```

例如：

```cpp
void set_name(std::string_view name);
```

如果 API 返回后仍需要使用：

```text
name
```

必须：

```text
copy
intern
move to owned storage
```

不能把 temporary 的 view 保存进异步 state。

---

## 3.12 参数传递——小型值类型

以下通常按值传递：

```text
整数
浮点数
enum class
pointer
string_view
chrono duration
chrono time_point
small handle
轻量 ID
```

例如：

```cpp
void set_level(LogLevel level);
void set_timeout(Duration timeout);
void set_name(std::string_view name);
```

---

## 3.13 参数传递——大型只读对象

大型只读对象：

```cpp
const T&
```

例如：

```cpp
void process(const Message& message);
```

避免无意义 copy。

---

## 3.14 参数传递——取得 ownership

函数如果要取得一个 value 的 ownership，常见写法：

```cpp
void set_name(std::string name) {
    name_ = std::move(name);
}
```

这样 caller 可以：

```text
传 lvalue → copy
传 rvalue → move
```

对于真正独占资源：

```cpp
void set_backend(std::unique_ptr<Backend> backend);
```

---

## 3.15 `shared_ptr` 参数

按值：

```cpp
void attach(std::shared_ptr<State> state);
```

通常表示：

```text
callee 取得 shared ownership
```

如果函数只是观察：

```cpp
void inspect(const State& state);
```

不要因为 caller 手里恰好是 `shared_ptr<State>` 就把 `shared_ptr` 传播进所有接口。

---

## 3.16 Output Parameter

优先返回：

```text
value
Result<T>
struct
```

而不是大量 output parameter。

例如优先：

```cpp
Result<ConnectionInfo> query_connection();
```

而不是：

```cpp
Status query_connection(
    std::string* name,
    int* state,
    Duration* timeout);
```

确有需要时：

```text
T&
→ required output

T*
→ optional output
```

并明确语义。

---

## 3.17 RAII

所有资源优先 RAII：

```text
memory
mutex
thread
file
socket
native handle
registration
subscription
mapping
temporary state
```

不要依赖调用者手工正确配对：

```text
open / close
lock / unlock
register / unregister
acquire / release
```

---

## 3.18 Raw `new/delete`

普通 application/runtime/library 代码禁止 owning：

```cpp
new
delete
```

优先：

```cpp
std::make_unique
std::make_shared
container
RAII wrapper
```

以下低层实现例外：

```text
placement new
custom allocator
memory pool
arena
object storage
bounded ring storage
```

但必须：

```text
局部化
有测试
有清晰 lifetime
```

---

## 3.19 Rule of Zero 优先

如果类型完全由已经正确实现 RAII 的成员组成，并且默认：

```text
copy
move
destructor
```

行为正好符合类型语义：

> **不要手工声明 special member functions。**

例如：

```cpp
class Configuration {
public:
    explicit Configuration(std::string name) : name_(std::move(name)) {}

private:
    std::string name_;
    std::vector<int> values_;
};
```

不需要机械写：

```cpp
Configuration(const Configuration&) = default;
Configuration& operator=(const Configuration&) = default;
Configuration(Configuration&&) = default;
Configuration& operator=(Configuration&&) = default;
~Configuration() = default;
```

---

## 3.20 Rule of Five 的适用边界

如果类型因为：

```text
资源 ownership
特殊 moved-from semantics
polymorphism
copy prohibition
自定义 destructor
```

需要显式定义或删除某个 special member，则必须整体审查：

```text
destructor
copy constructor
copy assignment
move constructor
move assignment
```

不是机械要求五个都实现，而是：

> **一旦开始手工控制特殊成员，就必须理解整体语义。**

需要 compiler 默认实现：

```cpp
= default;
```

需要禁止：

```cpp
= delete;
```

---

## 3.21 Move-only 类型

资源 owner 通常适合：

```cpp
Connection(const Connection&) = delete;
Connection& operator=(const Connection&) = delete;

Connection(Connection&&) noexcept;
Connection& operator=(Connection&&) noexcept;
```

但只有在确有：

```text
资源转移
特殊 moved-from semantics
```

时才手写 move body。

如果成员天然支持正确 move，优先 default。

---

## 3.22 `noexcept` 是正式 Contract

`noexcept` 不是“通常不会抛”的标记。

例如：

```cpp
Status shutdown(Duration timeout) noexcept;
```

意味着：

```text
任何内部异常都不能逃出该边界
```

实现必须：

```text
避免异常
或
捕获并转换
```

不能让：

```text
std::bad_alloc
std::system_error
第三方 exception
```

穿过 `noexcept` 导致意外 terminate。

---

## 3.23 `noexcept` Move 的常见陷阱

以下断言：

```cpp
static_assert(std::is_nothrow_move_constructible_v<T>);
```

只能证明：

> **类型系统观察到的 `T` move construction 被声明为 non-throwing。**

它不能单独证明：

> **move body 中所有实际操作都不会抛。**

尤其：

```cpp
class T {
public:
    T(T&&) noexcept = default;

private:
    ThrowingMove member_;
};
```

如果 `ThrowingMove` 的 move 是：

```cpp
noexcept(false)
```

外层显式 `noexcept` 可能仍使：

```cpp
std::is_nothrow_move_constructible_v<T>
```

为 `true`。

如果成员实际抛出：

```text
std::terminate()
```

---

## 3.24 `noexcept` Move 的正确验证

优先：

```text
Rule of Zero
```

让 compiler 自然推导。

如果需要 defaulted move，但不需要强制契约：

```cpp
T(T&&) = default;
T& operator=(T&&) = default;
```

让 exception specification 根据成员推导。

如果 Public API 明确要求：

```text
nothrow move
```

则同时验证：

```text
外层 type trait
直接 base/member/storage trait
自定义 move body 调用的 operation
```

例如：

```cpp
static_assert(std::is_nothrow_move_constructible_v<Storage>);
static_assert(std::is_nothrow_move_assignable_v<Storage>);

static_assert(std::is_nothrow_move_constructible_v<T>);
static_assert(std::is_nothrow_move_assignable_v<T>);
```

对于自定义 move body：

```text
必须 code review
```

确认不存在潜在 throwing operation。

所以：

> **type trait 是验证手段，不是完整证明。**

---

## 3.25 多态接口 Destructor

如果类型通过 base pointer 删除：

```cpp
Interface* object = new Implementation();
delete object;
```

base destructor 必须：

```cpp
virtual ~Interface() = default;
```

例如：

```cpp
class Backend {
public:
    virtual ~Backend() = default;

    virtual Status initialize() noexcept = 0;
};
```

---

## 3.26 `override`

所有 override 函数必须明确：

```cpp
override
```

例如：

```cpp
class FileBackend final : public Backend {
public:
    Status initialize() noexcept override;
    void write(const Record& record) noexcept override;
};
```

有 `override` 后不必重复：

```cpp
virtual
```

---

## 3.27 `final`

只有确实需要禁止继续继承时使用：

```cpp
class FileBackend final : public Backend {
};
```

不要为了“优化虚函数”到处机械加 `final`。

---

## 3.28 继承与组合

优先 composition。

使用继承前应确认：

```text
是否是真正 is-a 关系？
是否需要 runtime polymorphism？
是否共享稳定 contract？
```

不要为了复用几个 helper function 建立复杂继承树。

---

## 3.29 `[[nodiscard]]`

如果忽略返回值通常意味着 bug，应使用：

```cpp
[[nodiscard]]
```

典型：

```text
Result<T>
重要 Status
资源创建结果
validation result
```

如果项目的：

```cpp
Result<T>
```

始终不可忽略，优先在类型层统一：

```cpp
class [[nodiscard]] Result {
};
```

而不是每个 function 重复标记。

---

## 3.30 `[[nodiscard]]` 不应滥用

不要机械给：

```text
所有 getter
所有 size()
所有简单 value-returning function
```

加 `[[nodiscard]]`。

目标是：

```text
发现真正可能被错误忽略的结果
```

不是制造 warning 噪声。

需要显式忽略时可使用：

```cpp
(void)operation();
```

让 code review 清楚看到：

```text
这是故意忽略
```

---

## 3.31 错误模型必须统一

项目应选择明确 recoverable error model，例如：

```text
Status / Result<T>
expected-like type
std::error_code
exception-based public API
```

同一 abstraction layer 不要无规则混用：

```text
bool
nullptr
-1
exception
Status
```

表达相似失败。

---

## 3.32 可恢复失败

典型：

```text
InvalidArgument
NotFound
Timeout
Cancelled
ResourceExhausted
Unavailable
```

属于可预期状态时，应通过项目统一错误模型返回。

不要把普通：

```text
用户输入错误
资源暂时不可用
超时
```

一律抛成 `std::runtime_error`。

---

## 3.33 Exception Boundary

即使项目 Public API 主要不用 exception，也必须承认 exception 可能来自：

```text
allocation
thread creation
filesystem
std::function
third-party library
user callback
```

必须明确：

```text
异常在哪里允许产生？

在哪里捕获？

映射成什么 Status/Error？

哪些异常表示程序 invariant 被破坏？
```

---

## 3.34 不要假装“项目不使用异常”

以下写法不够：

```text
项目不用异常
```

因为：

```cpp
std::string
std::vector
std::function
std::thread
```

都可能在某些 operation 中抛异常。

真正要求应该是：

> **Public error model 可以不用 exception，但 exception boundary 必须明确。**

---

## 3.35 Primary Failure 优先

例如：

```text
Initialize()
→ primary failure

Rollback()
→ secondary failure
```

最终应保留：

```text
primary failure
```

而不是让 cleanup error 覆盖根因。

Secondary failure 可通过：

```text
log
diagnostic
stats
```

报告。

---

## 3.36 Partial Initialization

可能部分成功的组件必须确保：

```text
destructor safe
shutdown safe
rollback safe
```

例如：

```text
file 已打开
worker 尚未启动
第二个资源创建失败
```

不能让对象进入：

```text
既不能用
也不能销毁
```

的状态。

---

## 3.37 状态机必须明确

复杂 lifecycle 使用明确 state：

```text
Created
Configured
Initialized
Running
Stopping
Stopped
```

避免：

```cpp
bool initialized_;
bool running_;
bool stopping_;
bool stopped_;
```

组合形成隐含状态。

必须定义：

```text
允许 transition
谁可以 transition
并发 transition
terminal state
重复调用语义
```

---

## 3.38 State Owner

每个状态机必须明确：

```text
谁拥有 state？
谁可以修改？
哪些线程可以读取？
哪些 transition 需要 lock？
哪些 transition 使用 atomic？
```

不要让多个模块随意改变同一 lifecycle。

---

## 3.39 First Terminal Wins

对于真正具有 terminal result 的异步 operation，如果：

```text
complete
cancel
deadline
shutdown
transport failure
```

竞争，推荐：

> **First terminal transition wins.**

第一次 terminal transition 确定最终结果。

之后的 terminal attempt：

```text
只能观察已有结果
不能覆盖
```

---

## 3.40 两种不同的 Timeout

必须区分：

```text
Operation Deadline

Caller Wait Timeout
```

它们语义不同。

---

## 3.41 Operation Deadline

如果一个 operation 本身规定：

```text
必须在 deadline 前完成
```

deadline 到达可以使 operation：

```text
进入 Timeout terminal state
```

例如：

```text
Request
→ Completed / Cancelled / Timeout / TransportError
```

这种 Timeout 参与：

```text
first-terminal-wins
```

---

## 3.42 Caller Wait Timeout

例如：

```cpp
shutdown(timeout);
flush(timeout);
wait(timeout);
```

timeout 可能只表示：

> **当前 caller 不再等待。**

后台工作仍然：

```text
继续执行
最终成功
最终失败
继续 cleanup
```

该 Timeout：

```text
不是后台 operation 的最终 terminal result
```

也不能事后覆盖后台完成状态。

---

## 3.43 Timeout API 必须说明语义

所有带 timeout 的 API 必须文档化：

```text
这是 operation deadline？

还是 caller wait deadline？
```

不能仅因为返回值有：

```text
Timeout
```

就让用户猜测。

---

## 3.44 Destructor

Destructor 应：

```text
noexcept
```

并避免：

```text
无界等待
可能永久阻塞的 I/O
未知用户 callback
```

需要完整同步关闭的对象应提供显式：

```text
shutdown(timeout)
stop(timeout)
close()
```

---

## 3.45 Destructor 与 Cleanup

Destructor 应保证：

```text
memory safety
resource ownership correctness
no exception escape
```

不一定保证：

```text
所有持久化数据已 flush
后台 I/O 已完成
所有 worker 都完成业务任务
```

这些需要显式 shutdown contract。

---

## 3.46 Timeout 后 Lifetime

如果 caller wait 超时：

```text
不能立即 free
仍被后台线程使用的 backing state
```

必须保持：

```text
self-owned state
shared lifetime
joinable owner
其他安全生命周期机制
```

直到真正 quiescent。

---

## 3.47 并发实现优先正确性

首版优先：

```text
mutex
condition_variable
atomic
bounded ring
simple state machine
```

而不是直接：

```text
lock-free queue
hazard pointer
RCU
复杂 generation protocol
custom memory order graph
```

只有 benchmark 显示真实瓶颈后再优化。

---

## 3.48 强一致并发 Operation

并非所有并发 API 都需要单一线性化点。

但如果 operation 承诺：

```text
原子接受
注册
取消
状态转换
ownership transfer
terminal completion
```

则应定义：

```text
linearization point
```

即：

> **在并发历史中，该 operation 何时被认为已经发生。**

---

## 3.49 典型需要 Linearization Point 的操作

例如：

```text
enqueue acceptance
registration
unregistration
cancellation
publish acceptance
Running → Stopping
first terminal completion
resource acquisition
```

如果无法指出发生时刻，则很难证明竞态语义。

---

## 3.50 弱一致查询

某些并发 API 可以明确承诺：

```text
eventually consistent
approximate
weak snapshot
```

例如：

```text
Statistics snapshot
metrics
queue high watermark
diagnostic counters
```

这类接口不需要强行定义一个：

```text
全局 snapshot linearization point
```

---

## 3.51 弱一致 Query 必须定义什么

至少定义：

```text
每个字段是否 race-free
每个字段可见性
字段是否来自同一时刻
是否 eventually converge
允许多旧
能否用于 correctness
能否用于 synchronization
```

例如：

```text
fields individually atomic/race-free

snapshot not point-in-time coherent

eventually converges

diagnostic only

must not be used as synchronization condition
```

这就是完整并发 contract。

---

## 3.52 Lock Scope

普通状态锁内避免：

```text
用户 callback
未知第三方 callback
blocking I/O
长期 sleep
可能永久等待的 operation
```

推荐：

```text
lock
→ read/update state
→ copy necessary data
→ unlock
→ call external code
```

---

## 3.53 Serialized Invocation Lock

如果某把锁的设计目的就是：

```text
确保第三方 Backend callback 不并发
```

那么在锁内调用 Backend 可以是正确设计。

关键是：

```text
必须明确这是 serialization lock
而不是普通 state lock
```

---

## 3.54 Lock Ordering

多个锁可能嵌套时必须建立固定 order。

例如：

```text
registry
→ operation
→ completion
```

不能另一路径：

```text
completion
→ registry
```

否则存在 ABBA deadlock 风险。

---

## 3.55 避免 Nested Lock

如果能够通过：

```text
copy state
reduce critical section
split operation
```

消除 nested locking，应优先这样做。

---

## 3.56 多 mutex 同时 Lock

C++17 下需要同时取得多个 mutex 时，优先：

```cpp
std::scoped_lock lock(mutex_a, mutex_b);
```

或者：

```cpp
std::lock(...)
```

避免手工：

```cpp
mutex_a.lock();
mutex_b.lock();
```

在异常和竞态下增加复杂性。

---

## 3.57 Atomic Memory Order

不要把：

```cpp
std::memory_order_relaxed
```

视为默认性能优化。

每个非默认 memory order 都必须能解释：

```text
哪个 write 发布什么 state？
哪个 read 需要看到？
happens-before 是什么？
为什么 relaxed 足够？
为什么 acquire/release 足够？
```

解释不清楚时优先：

```text
seq_cst
mutex
更简单状态模型
```

---

## 3.58 Atomic 不是 Lock-free 保证

使用：

```cpp
std::atomic<std::uint64_t>
```

不意味着所有目标平台都：

```text
lock-free
```

除非项目明确要求并验证：

```cpp
is_always_lock_free
```

否则：

```text
atomic correctness
```

与：

```text
atomic lock-free performance
```

应分别讨论。

---

## 3.59 `thread_local`

只用于真实 per-thread state，例如：

```text
recursion guard
thread cache
thread registration
per-thread scratch buffer
```

不要把 `thread_local` 当作：

```text
绕过共享状态设计
逃避 synchronization
```

的工具。

---

## 3.60 Callback Boundary

除非设计明确要求：

> **不要在持有普通内部状态锁时调用未知 callback。**

未知 callback 可能：

```text
递归进入当前组件
阻塞
等待另一把锁
调用外部模块
抛异常
执行很久
```

---

## 3.61 Callback Exception

用户 callback 是否允许抛异常必须明确。

常见策略：

```text
catch and convert

catch and terminate

allow propagation
```

必须由项目/模块 contract 指定。

不能默认假设：

```text
callback 永远不抛
```

---

# 4. 实现质量与可读性

## 4.1 注释解释 Why，而不是 What

差：

```cpp
// Increment counter.
++counter;
```

好：

```cpp
// Count only requests that reached the admission boundary
// and lost the shutdown race.
++rejected_count;
```

注释应重点解释：

```text
为什么这样实现
重要 invariant
ownership
lifetime
linearization point
failure guarantee
特殊 workaround
平台约束
```

不要逐行翻译代码。

---

## 4.2 Public API Documentation

非显而易见的 Public API 应使用 Doxygen-compatible comment。

重点描述：

```text
ownership
borrowed lifetime
thread safety
error semantics
timeout semantics
moved-from semantics
consistency model
```

例如：

```cpp
/// Returns a diagnostic snapshot.
///
/// Individual fields are race-free, but the snapshot is not guaranteed
/// to represent a single coherent point in time.
LoggingStats get_stats() const noexcept;
```

---

## 4.3 Borrowed Lifetime 必须写清楚

例如：

```cpp
/// `name` is borrowed only for the duration of this call.
Status register_name(std::string_view name);
```

或者：

```cpp
/// The returned view remains valid until the next mutation.
std::string_view name() const noexcept;
```

不要让调用者猜：

```text
string_view 能保存多久
pointer 谁拥有
reference 是否跨线程有效
```

---

## 4.4 Thread Safety Documentation

重要 public type 应说明：

```text
Thread-safe
Thread-compatible
Single-thread confined
Externally synchronized
```

至少对于并发 runtime 对象，应让调用者知道：

```text
哪些函数可并发调用
哪些不能
```

---

## 4.5 并发代码必须可解释

强一致 operation 必须能够解释：

```text
state owner
linearization point
lock / atomic
lifetime owner
shutdown race
first-terminal rule
```

弱一致 query 必须能够解释：

```text
visibility
consistency
convergence
diagnostic-only or correctness-safe
```

如果 code review 中只能回答：

```text
“应该没问题”
```

则实现未达到质量要求。

---

## 4.6 Helper 函数命名

好：

```cpp
validate_config();
create_backend();
start_worker();
rollback_initialization();
complete_request();
```

差：

```cpp
do_work();
handle_thing();
helper();
process_internal();
do_step();
```

Helper 应表达：

```text
真实动作
真实阶段
真实职责
```

---

## 4.7 Function 粒度

不设机械：

```text
20 行
50 行
100 行
```

限制。

判断标准：

> **这个函数是否仍然表达一个可以清晰命名和推理的 operation？**

如果一个 function 同时完成：

```text
validation
allocation
I/O
state transition
callback
rollback
logging
```

通常说明应拆分。

---

## 4.8 Function 拆分方式

按语义阶段：

```cpp
validate_config();
create_resources();
initialize_backend();
start_worker();
rollback_initialization();
```

不要拆成：

```cpp
step1();
step2();
do_part_a();
helper2();
```

仅为了降低行数。

---

## 4.9 Bool 参数

避免：

```cpp
open_file(true, false, true);
```

这种调用无法读懂。

优先：

```text
enum class
Options
Config
builder
```

例如：

```cpp
OpenOptions options;
options.truncate = true;
options.sync = false;
open_file(path, options);
```

简单 setter：

```cpp
set_enabled(bool enabled);
```

可以接受。

---

## 4.10 `optional<bool>` 等三态语义

如果存在：

```text
true
false
unspecified
```

不要用奇怪 sentinel。

可以：

```cpp
std::optional<bool>
```

但如果三态本身是领域概念，优先：

```cpp
enum class Policy {
    Default,
    Enabled,
    Disabled,
};
```

通常更清晰。

---

## 4.11 不为抽象而抽象

避免：

```text
Interface
→ AbstractBase
→ BaseImpl
→ Adapter
→ Manager
→ Factory
```

每一层必须能回答：

```text
它隔离了什么变化？

它拥有哪项职责？

哪个 contract 需要它？

删除它会破坏什么？
```

如果答不出来，就不应存在。

---

## 4.12 不为对称性建模

不要因为：

```text
Client 有 State
```

就默认：

```text
Server 也必须有相同 State 类型
```

不要因为：

```text
Request
```

就机械增加：

```text
RequestBase
ResponseBase
OperationBase
```

抽象应来自真实共享语义，而不是结构视觉对称。

---

## 4.13 避免万能 `Manager`

如果对象实际上只是：

```text
Registry
Scheduler
Factory
Router
Cache
Pool
```

就用准确名字。

`Manager` 只有在确实存在：

```text
多职责协调
生命周期管理
资源管理
```

且名字仍然最准确时才使用。

---

## 4.14 避免万能 `Context`

`Context` 应表示：

```text
某一明确生命周期或调用环境
```

例如：

```text
RequestContext
ExecutionContext
```

不要把任何参数集合都命名为：

```text
Context
```

---

## 4.15 避免万能 `Utils`

如果一个 helper 属于：

```text
string parsing
time
filesystem
serialization
```

就放到真实领域。

避免：

```text
Utils
CommonUtils
Helper
Misc
```

不断吸收无关逻辑。

---

## 4.16 Public API 不要泄漏第三方类型

除非模块的目标本身就是：

```text
对第三方 API 的直接 wrapper
```

否则 Public API 不应暴露：

```text
第三方 class
第三方 enum
第三方 smart pointer
第三方 callback type
第三方 config object
```

---

## 4.17 Dependency Direction

高层模块不应因为实现便利：

```text
反向依赖低层 implementation detail
```

例如：

```text
Core API
→ 不应 include Backend 第三方 header
```

依赖方向应清晰。

---

## 4.18 Interface 与 Implementation Header

Public interface：

```text
include/project/...
```

Private implementation：

```text
src/...
```

不要让 private header 成为用户事实上的 API。

---

## 4.19 Pimpl

Pimpl 不是默认选择。

只有在需要：

```text
ABI stability
隐藏大型 dependency
降低 rebuild
隐藏平台 implementation
```

时使用。

不要为了“看起来专业”给所有 class 加：

```cpp
struct Impl;
std::unique_ptr<Impl> impl_;
```

增加 heap 和 indirection。

---

## 4.20 `std::function`

`std::function` 适合需要：

```text
type-erased callback ownership
```

的场景。

但要意识到：

```text
可能 allocation
copy semantics
exception
type erasure overhead
```

Hot path 不应无评估大量使用。

如果 callback lifetime 明确、无需 ownership，也可以考虑：

```text
template callable
function pointer
reference wrapper
```

具体由 API 语义决定。

---

## 4.21 `std::shared_ptr` 不应成为默认状态 handle

即使 `shared_ptr` 很方便，也必须先确认：

```text
真的需要 shared ownership 吗？
```

如果只是：

```text
父对象拥有
子对象借用
```

则 unique ownership + borrowed reference 可能更清楚。

---

## 4.22 Container 选择

优先按语义选择：

```text
vector
array
deque
map
unordered_map
set
```

不要因为“大 O 看起来更好”就默认 `unordered_map`。

考虑：

```text
数据规模
迭代频率
cache locality
稳定引用需求
ordering
allocation
hash attack 是否相关
```

---

## 4.23 `vector` 优先原则

小到中等规模顺序数据通常优先：

```cpp
std::vector
```

其：

```text
cache locality
简单性
低常数
```

经常比链表结构更重要。

不要在没有稳定 iterator/node requirement 时默认 `std::list`。

---

## 4.24 String Ownership

长期存储文本：

```cpp
std::string
```

借用：

```cpp
std::string_view
```

固定字面量：

```text
const char*
string_view
```

视 contract 而定。

不要混淆：

```text
view
ownership
null-terminated C string
```

---

## 4.25 `std::move`

只在真正放弃原对象值时使用。

例如：

```cpp
name_ = std::move(name);
```

不要机械：

```cpp
return std::move(value);
```

阻碍 NRVO。

也不要：

```cpp
std::move(const_value)
```

误以为一定会 move。

---

## 4.26 Moved-from State

除标准保证之外，如果 Public type 需要更强 moved-from contract，应明确文档。

例如：

```text
valid but unspecified
```

或者：

```text
empty handle
shutdown() safe
其他 operation InvalidState
```

不能让用户依赖未定义的内部字段状态。

---

## 4.27 TODO

允许：

```cpp
// TODO: Consider a lock-free queue only if profiling shows
// producer contention is a meaningful bottleneck.
```

不允许：

```cpp
// TODO fix
// TODO optimize
// TODO later
```

TODO 应说明：

```text
什么条件触发后续工作
为什么当前不实现
```

当前版本必须完成的需求不能用 TODO 代替。

---

## 4.28 FIXME

`FIXME` 表示：

```text
当前存在已知缺陷
```

如果代码准备合并到主分支，一般不应保留未处理的严重 `FIXME`。

如果确实必须保留，应有：

```text
issue
原因
影响范围
```

---

## 4.29 Dead Code

禁止长期保留：

```text
注释掉的旧实现
#if 0 大块实现
未使用 function
未使用 field
废弃 compatibility branch
```

Git 已经保存历史。

---

## 4.30 Pre-release 开发

尚未正式发布、没有真实稳定兼容承诺时：

> **错误设计直接修正。**

不要因为旧草稿、旧测试、旧分支存在，就增加：

```text
deprecated wrapper
compatibility alias
legacy API
duplicate implementation
```

除非确实存在真实下游使用者。

---

## 4.31 优化纪律

优化顺序：

```text
正确
↓
清晰
↓
可测试
↓
建立 baseline
↓
测量
↓
优化
↓
重新验证
```

不要先写：

```text
lock-free
intrusive pointer
custom allocator
SBO
cache-line padding
manual prefetch
branch prediction hint
```

再寻找理由。

---

## 4.32 优化必须能解释收益

优化 PR 应尽量回答：

```text
瓶颈是什么？

如何测到？

baseline 是多少？

修改后是多少？

复杂度增加了多少？

是否影响正确性证明？
```

否则：

```text
简单实现
```

通常优先。

---

## 4.33 Logging 不替代错误处理

不要写：

```cpp
log_error("failed");
return;
```

然后丢失正式错误语义。

日志是：

```text
诊断
```

不是：

```text
API error channel
```

---

## 4.34 Assertion

Assertion 用于：

```text
程序 invariant
开发期 impossible state
```

而不是用户可触发的正常错误。

用户输入错误应：

```text
返回项目错误类型
```

而不是：

```cpp
assert(valid_input);
```

---

## 4.35 `assert` 与 Release

不能依赖：

```cpp
assert(...)
```

执行具有副作用的逻辑。

错误：

```cpp
assert(initialize());
```

因为 Release build 可能完全不执行。

---

# 5. 测试、工具链与交付质量

## 5.1 Definition of Done

功能不能因为：

```text
能编译
```

就视为完成。

通常至少要求：

```text
实现符合设计 contract

clang-format 通过

compiler warnings = 0

相关 unit tests 通过

相关 contract tests 通过
```

涉及复杂：

```text
ownership
lifetime
concurrency
```

还应按风险运行：

```text
ASAN
UBSAN
TSAN
```

---

## 5.2 规则到工具的映射

不同规则必须使用正确验证手段：

| 要求 | 主要验证方式 |
|---|---|
| 缩进、换行、brace placement、pointer alignment | clang-format 14.x |
| 函数/类型/变量命名 | clang-tidy `readability-identifier-naming` |
| 控制语句必须加 `{}` | clang-tidy `readability-braces-around-statements` |
| C-style cast | compiler / clang-tidy |
| override | compiler / clang-tidy |
| Header 自包含 | 独立编译测试 |
| Header guard | review / lint |
| C++17 compatibility | compiler |
| template contract | compile tests |
| invalid template usage | compile-fail tests |
| `noexcept` declaration | compiler trait |
| `noexcept` 实际安全性 | member trait + implementation review |
| Ownership / lifetime | API review + ASAN |
| data race | TSAN + review |
| 强一致并发语义 | linearization review + stress/contract tests |
| 弱一致 query | visibility/convergence tests |
| rollback | fault injection |
| Timeout | contract tests |
| 性能 | benchmark |

原则：

> **不要要求某个工具证明它本身无法证明的规则。**

---

## 5.3 Compiler Warning 基线

至少启用：

```text
-Wall
-Wextra
-Wpedantic
```

建议评估：

```text
-Wconversion
-Wsign-conversion
-Wshadow
-Wnon-virtual-dtor
-Wold-style-cast
-Woverloaded-virtual
-Wnull-dereference
-Wdouble-promotion
-Wformat=2
```

是否启用应以：

```text
真实信噪比
```

为标准。

不要为了：

```text
warning 数量更多
```

导致代码中到处 suppression。

---

## 5.4 Warnings as Errors

项目自身代码 CI：

```text
warning = error
```

推荐：

```text
-Werror
```

第三方 warning 不应阻塞项目构建。

通过：

```text
SYSTEM include
third-party target
external project
```

隔离。

不要为了第三方 warning：

```text
全局关闭自己的 warning
```

---

## 5.5 clang-format CI

CI 使用：

```text
clang-format 14.x
```

执行等价：

```text
clang-format --dry-run --Werror
```

的检查。

其职责只是：

```text
formatter 管理的格式
```

不能用于验证：

```text
控制语句 brace
命名
ownership
错误语义
线程安全
```

---

## 5.6 clang-tidy 基线

参考：

```text
clang-tidy 14.x
```

推荐从：

```text
bugprone-*
performance-*
modernize-*
readability-*
cppcoreguidelines-*
```

中选择高价值规则。

不是机械：

```text
全开
```

---

## 5.7 推荐 clang-tidy 重点规则

特别关注：

```text
readability-identifier-naming
readability-braces-around-statements

bugprone-use-after-move
bugprone-unchecked-optional-access（若版本支持）
bugprone-narrowing-conversions

performance-unnecessary-copy-initialization
performance-move-const-arg

modernize-use-override
modernize-use-nullptr

cppcoreguidelines-pro-type-cstyle-cast
```

具体可用规则以 clang-tidy 14 实际支持为准。

---

## 5.8 clang-tidy 不应机械全开

某些规则可能：

```text
不适合系统编程
不适合现有 API
噪声高
与项目设计冲突
```

应在：

```text
.clang-tidy
```

集中配置。

不要通过源码中大量：

```cpp
// NOLINT
```

维持“全绿”。

---

## 5.9 NOLINT

确实需要 suppression：

```text
范围尽量小
原因明确
不能掩盖真实 bug
```

例如：

```cpp
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
// Required by the platform ABI to map the opaque native handle.
```

比：

```cpp
// NOLINT
```

更好。

---

## 5.10 Naming Check 的例外

`readability-identifier-naming` 必须允许：

```text
第三方 override
标准 library-compatible alias
operator
generated code
外部 ABI
```

不要为了 clang-tidy 全绿：

```text
错误重命名第三方 override
修改生成代码
破坏标准 generic interface
```

---

## 5.11 Generated Code 排除

生成代码应从：

```text
clang-tidy
人工 naming check
部分 warning
```

中合理隔离。

通过：

```text
target separation
HeaderFilter
directory exclusion
```

处理，而不是每行加 `NOLINT`。

---

## 5.12 Unit Tests

复杂组件至少覆盖：

```text
happy path
invalid input
boundary values
failure path
state transition
```

测试重点是：

```text
contract
```

而不是单纯让每行代码执行一次。

---

## 5.13 Contract Tests

多个实现共享 interface 时，应尽量建立：

```text
contract test suite
```

例如：

```text
BackendContractTests
TransportContractTests
StorageContractTests
AllocatorContractTests
```

这样可以确保：

```text
Implementation A
Implementation B
```

遵守相同语义。

---

## 5.14 Public Header Compile Test

每个 Public Header 应独立编译：

```cpp
#include <project/component.hpp>

int main() {
    return 0;
}
```

用于发现：

```text
transitive include
第三方类型泄漏
错误 forward declaration
宏依赖
include 顺序依赖
```

---

## 5.15 C++17 Compile Test

CI 至少使用：

```text
-std=c++17
```

构建。

不能因为开发环境 compiler 默认支持 C++20，就让代码偷偷依赖高版本能力。

---

## 5.16 Compile-fail Tests

适合：

```text
非法 template argument
错误 callback return type
unsupported scope
invalid type trait
禁止的 API 组合
```

使用：

```text
try_compile
negative translation unit
```

验证。

不要只在文档声称：

```text
“这会编译失败”
```

---

## 5.17 Fault Injection

如果模块设计声称正确处理：

```text
allocation failure
thread creation failure
Backend failure
I/O failure
partial initialization
rollback failure
```

就应该能够测试这些路径。

否则：

```text
ResourceExhausted
rollback
secondary failure
```

只是纸面契约。

---

## 5.18 Allocation Failure

标准 C++ 中全面可靠模拟系统 OOM 并不简单。

可通过：

```text
测试 allocator
test hook
可注入 factory
受控 allocation wrapper
```

测试关键逻辑。

不要为了 fault injection 污染 production API。

---

## 5.19 Partial Initialization Test

必须覆盖：

```text
第一资源成功
第二资源失败
```

然后检查：

```text
第一资源已释放
对象可析构
无 thread leak
无 fd leak
原始 failure 保留
```

---

## 5.20 Rollback Secondary Failure

需要测试：

```text
primary operation fails
rollback also fails
```

确认：

```text
primary error 不被覆盖
```

secondary error 通过适当诊断暴露。

---

## 5.21 Concurrency Tests

并发模块不能只测：

```text
单线程 happy path
```

至少覆盖相关：

```text
multiple producers
multiple consumers
start/stop race
shutdown race
timeout race
cancel/completion race
register/unregister race
destroy/in-flight race
```

---

## 5.22 Stress Tests

Stress test 用于：

```text
扩大 race 暴露概率
```

应该：

```text
重复
并发
随机化部分调度
足够长但可控
```

不能把 stress test 当成形式证明。

---

## 5.23 Linearization Contract Test

对于强一致 operation，应设计测试验证：

```text
两种竞态顺序都合法

没有第三种非法状态
```

例如：

```text
operation wins
→ accepted

shutdown wins
→ rejected
```

不能出现：

```text
operation 看似成功
但 owner 完全不知道
```

---

## 5.24 Weak-consistency Test

对于 eventually-consistent query：

不要测试不存在的：

```text
全字段同一时刻 snapshot
```

而应该验证：

```text
没有 data race
值始终合法
每个字段满足自己的可见性保证
更新最终可见
最终收敛
不会被错误用于 synchronization
```

---

## 5.25 Timeout Tests

必须区分：

```text
operation deadline test
caller wait timeout test
```

Operation timeout：

```text
确认最终 operation result = Timeout
```

Wait timeout：

```text
确认 caller 返回 Timeout
后台 operation 可继续
backing lifetime 安全
以后可完成
```

---

## 5.26 Repeated Timeout / Wait

如果 API 支持重复：

```cpp
shutdown(timeout);
```

应测试：

```text
第一次 Timeout
后台继续

第二次继续等待
最终 Ok
```

如果设计如此规定。

---

## 5.27 Moved-from Tests

对 Public move-enabled type，如果定义 moved-from contract，应测试：

```text
哪些 operation 可调用
返回什么
是否 safe destruct
是否 safe move-assign
```

不能只测 move construction 本身。

---

## 5.28 Sanitizer

推荐：

```text
ASAN
UBSAN
TSAN
```

---

## 5.29 ASAN

重点发现：

```text
use-after-free
heap-buffer-overflow
stack-buffer-overflow
double free
```

特别适合：

```text
异步 lifetime
deferred cleanup
callback ownership
```

---

## 5.30 UBSAN

用于发现：

```text
undefined behavior
invalid shift
signed overflow 某些场景
invalid enum
alignment violation
```

---

## 5.31 TSAN

并发核心必须重点运行 TSAN。

适合：

```text
state machine
queue
shutdown
registration
statistics
category update
completion
```

TSAN clean 不是并发正确性的充分条件，但属于重要必要验证。

---

## 5.32 Sanitizer 与正常 build 分离

Sanitizer build：

```text
不等于 production performance build
```

应使用独立 build directory / CI job。

不要把 sanitizer flag 永久加入 production target。

---

## 5.33 Benchmark

Benchmark 的目的：

> **发现真实性能瓶颈和验证优化收益。**

不是：

```text
证明代码“很高性能”
```

---

## 5.34 Benchmark 前后对比

优化应尽量提供：

```text
before
after
```

例如：

```text
latency
throughput
allocation count
contention
CPU usage
memory footprint
```

---

## 5.35 Benchmark 驱动优化

只有数据说明问题后，再考虑：

```text
lock-free
intrusive refcount
SBO
custom allocator
generation cache
batching
cache-line padding
```

否则优先简单实现。

---

## 5.36 Benchmark 不能替代 Correctness

性能更好但：

```text
race
lifetime unclear
timeout broken
error path incomplete
```

的实现不可接受。

---

## 5.37 Coverage

不追求：

```text
100% line coverage
```

作为核心目标。

更重要的是：

```text
状态转换
错误路径
rollback
timeout
partial initialization
moved-from
race
weak consistency
```

得到覆盖。

---

## 5.38 Line Coverage 的局限

例如：

```text
95% line coverage
```

仍可能完全没测：

```text
两个线程同时 shutdown
allocation failure
rollback secondary failure
```

因此 coverage 只是辅助指标。

---

## 5.39 clang-format Check

每个 PR：

```text
format check
```

应通过。

不能要求 reviewer 手工指出：

```text
缩进
pointer spacing
brace formatting
```

这些机械问题。

---

## 5.40 Compiler Warning Check

项目自己的 source：

```text
0 warnings
```

尤其不要允许：

```text
新 warning
```

长期累积。

---

## 5.41 clang-tidy Check

clang-tidy 可以：

```text
PR 增量执行
定期完整执行
```

根据项目规模决定。

不要在开发阶段因为完整静态分析过重而阻塞每一个微小修改，但最终质量阶段应完整验证。

---

## 5.42 开发阶段与最终验证阶段

为了兼顾开发效率与最终质量，可区分：

### 开发阶段

每个阶段只执行：

```text
相关 target 编译
必要 unit test
必要 contract test
关键新增路径的针对性验证
```

不要求每次修改都运行：

```text
全量 test
全部 sanitizer
全部 stress
全部 clang-tidy
完整 benchmark
```

---

### 完成阶段

功能全部实现后，再统一执行：

```text
full build
full unit tests
contract tests
compile tests
fault injection
stress
ASAN
UBSAN
TSAN
clang-format
compiler warnings
clang-tidy
benchmark
```

然后集中修复。

---

## 5.43 Code Review：API

检查：

```text
[ ] API 是否真的需要 public？
[ ] 命名是否符合规范？
[ ] 是否存在不必要 convenience API？
[ ] 是否泄漏 implementation detail？
[ ] 是否泄漏第三方类型？
[ ] ownership 是否明确？
[ ] borrowed lifetime 是否明确？
[ ] 参数传递是否合理？
[ ] 单参数 constructor 是否需要 explicit？
[ ] [[nodiscard]] 是否应用在重要结果？
```

---

## 5.44 Code Review：Special Members

检查：

```text
[ ] 是否可以使用 Rule of Zero？
[ ] 是否无意义显式 default 所有 special members？
[ ] 若定义/删除一个 special member，是否整体审查？
[ ] move-only 是否确有必要？
[ ] noexcept move 是否真实？
[ ] 是否验证 direct member/storage 的 nothrow move？
[ ] custom move body 是否调用 throwing operation？
[ ] moved-from state 是否清晰？
```

---

## 5.45 Code Review：Implementation

检查：

```text
[ ] 是否存在过度抽象？
[ ] 是否存在万能 Manager/Utils/Helper？
[ ] 是否存在无意义 Base/Impl/Adapter 层？
[ ] 是否存在 raw owning new/delete？
[ ] 是否依赖 transitive include？
[ ] Header 是否 self-contained？
[ ] Header 是否有重复包含保护？
[ ] 成员是否全部确定初始化？
[ ] initializer order 是否正确？
[ ] 是否存在 dead code？
[ ] 是否存在无价值 compatibility code？
```

---

## 5.46 Code Review：Error Handling

检查：

```text
[ ] 失败是否使用统一 error model？
[ ] 是否无规则混用 nullptr/bool/-1/exception？
[ ] exception boundary 是否明确？
[ ] noexcept boundary 是否真的不会泄漏 exception？
[ ] allocation failure 是否有定义？
[ ] partial initialization 是否能安全清理？
[ ] rollback 是否完整？
[ ] secondary failure 是否覆盖 primary failure？
```

---

## 5.47 Code Review：Lifetime

检查：

```text
[ ] 谁拥有对象？
[ ] 谁只是借用？
[ ] string_view 是否可能 dangling？
[ ] callback 捕获对象 lifetime 是否安全？
[ ] async operation 是否延长必要 backing？
[ ] wrapper 析构后 in-flight 是否安全？
[ ] timeout 后 backing 是否安全？
[ ] moved-from object 是否符合 contract？
```

---

## 5.48 Code Review：Polymorphism

检查：

```text
[ ] base destructor 是否正确？
[ ] override 是否明确？
[ ] final 是否真的需要？
[ ] 是否存在 object slicing？
[ ] 是否能用 composition 代替复杂继承？
```

---

## 5.49 Code Review：Timeout

检查：

```text
[ ] Timeout 是 operation deadline 还是 caller wait timeout？
[ ] 两者是否混淆？
[ ] wait timeout 后后台 operation 是否安全继续？
[ ] Timeout 是否错误覆盖后台最终结果？
[ ] repeated wait 的语义是否定义？
```

---

## 5.50 Code Review：Concurrency

检查：

```text
[ ] state machine 是否明确？
[ ] state owner 是否明确？
[ ] 强一致 operation linearization point 是否明确？
[ ] 弱一致 query consistency contract 是否明确？
[ ] lock 保护什么？
[ ] lock order 是否明确？
[ ] 是否存在 ABBA 风险？
[ ] 是否在普通锁内调用未知 callback？
[ ] timeout 后 backing 是否仍安全？
[ ] first-terminal rule 是否正确？
[ ] atomic memory order 是否能解释？
```

---

## 5.51 Code Review：Testing

检查：

```text
[ ] Happy path
[ ] Invalid input
[ ] Boundary values
[ ] Failure path
[ ] Partial initialization
[ ] Rollback
[ ] Secondary failure
[ ] Moved-from
[ ] Timeout
[ ] Concurrent race
[ ] Weak consistency
[ ] Fault injection
[ ] Sanitizer
```

---

## 5.52 Code Review：Performance

检查：

```text
[ ] Hot path 是否新增 heap allocation？
[ ] Hot path 是否新增 blocking I/O？
[ ] Hot path 是否新增 contention？
[ ] 是否存在无数据支持的优化？
[ ] 优化是否显著增加 correctness complexity？
[ ] 是否有 benchmark before/after？
```

---

## 5.53 最终通用质量基线

所有采用本规范的 C++ 项目统一遵循：

```text
C++17 only.

GCC 11.x 作为主要参考编译环境。

clang-format 14.x 作为格式输出基线。

clang-tidy 14.x 作为静态检查参考基线。

格式由工具决定，不由人工建立第二套排版规范。

类型使用 PascalCase。

项目拥有的函数和成员函数使用 snake_case。

变量和参数使用 snake_case。

公开 aggregate 字段使用 snake_case。

private/protected member 使用 snake_case_。

常量使用 kPascalCase。

Macro 使用项目限定的 UPPER_SNAKE_CASE。

标准接口、第三方 override、生成代码、外部 ABI
保留外部规定名称。

Header 必须具有重复包含保护。

Header 必须 self-contained。

Include What You Use。

成员必须确定初始化。

initializer list 与成员声明顺序保持一致。

单参数 constructor 默认 explicit。

优先 Rule of Zero。

自定义 special member 时整体审查其语义。

显式 noexcept move 不能只依赖外层 type trait
证明实现安全。

多态 base destructor 必须正确。

override 必须显式写 override。

重要不可忽略结果使用 [[nodiscard]]。

参数传递必须表达数据大小和 ownership。

Ownership 优先通过类型表达。

Raw pointer 默认 non-owning。

string_view 默认 borrowed。

异步路径不得保存无 lifetime 保证的 borrowed data。

复杂 runtime resource 优先使用可表达失败的 factory。

复杂 lifecycle 使用显式状态机。

错误模型在同一抽象层保持统一。

异常必须具有明确 boundary。

Primary failure 不被 cleanup secondary failure 覆盖。

Partial initialization 必须可安全 rollback / destruct。

Operation deadline 与 caller wait timeout 必须明确区分。

真正具有 terminal state 的 operation 使用明确 terminal contract。

承诺原子状态变化的并发 operation 定义 linearization point。

弱一致 query 不强求单一线性化点，
但必须定义 visibility、consistency 和 convergence。

普通状态锁内避免执行未知 callback 和 blocking I/O。

存在多锁时定义 lock order。

非默认 atomic memory order 必须能够解释。

Destructor 不承担可能无界阻塞的等待。

Timeout 后不得释放仍被后台 operation 使用的 backing。

Public API 保持克制。

第三方 dependency 保持隔离。

不要为了对称性制造抽象。

不要把 Manager / Context / Utils / detail / internal / impl
当作缺乏职责建模时的默认容器。

优先简单、明确、可证明正确的实现。

先建立正确 baseline，再 benchmark，再优化。

性能优化必须由真实数据驱动。

测试验证 contract，而不仅是 line coverage。

并发代码同时依赖 review、contract tests、stress tests 和 TSAN。

开发阶段只做必要验证以保证开发效率。

功能完成后再集中执行系统级完整测试和质量验证。

尚未发布的项目优先直接修正错误设计，
不为不存在的兼容需求制造 legacy layer。
```

---

## 5.54 规范执行原则

最后需要强调：

本规范不是要求开发者：

```text
把所有规则机械编码成脚本
```

也不是要求：

```text
每次提交都跑所有工具
```

而是建立三个层级：

```text
机械规则
→ 自动化

局部代码质量
→ compiler / clang-tidy / unit test

架构、Ownership、生命周期、并发语义
→ design contract + review + contract test
```

工具适合处理：

```text
格式
命名
明显 bug pattern
compile contract
```

人和测试负责：

```text
API 是否合理
Ownership 是否正确
状态机是否完整
Timeout 是否定义清楚
并发语义是否可证明
错误路径是否闭合
```

最终目标不是：

```text
让工具全部变绿
```

而是：

> **构建一套长期可维护、行为明确、容易审查、容易测试、能够在 C++17 环境中稳定演进的代码基线。**
