# RPC 框架概览

基于 JSON-RPC 2.0 的轻量 RPC 实现，采用“少量长连接 + 多请求”模型，支持并发调用、CRC 校验、超时与回调分发。

# 这个项目是什么
这是一个基于RPC-JSON 2.0 的RPC框架实现，采用“单链接多请求”的方式，支持并发调用。

# 项目架构
## Client
  - 连接池：负责连接的创建、维护、复用、回收，每一次发送一个RPC请求，都从连接池中获取一个连接，省去了反复创建和销毁的开销
  - RPC调用封装：在连接池的基础上工作，封装了RPC请求的组织、发送的过程
  - 接收线程：基于EPOLL实现，持续监听socket可读/异常事件并进行处理
  - 响应队列+回调线程：接收线程将响应放入队列，回调线程从队列中取出响应并调用对应的回调函数，这样设计是为了避免长时间的回调影响接收线程的性能。其中队列是线程安全的环形队列
  - Pending表：存储未完成的RPC请求信息，用于超时检测和结果回调。以 RPC Request ID为Key存储信息，在响应到达时根据ID找到对应的请求并处理
  - 超时检测：定期检查Pending表中是否有请求超时，超时则触发回调并清理
  - CRC：请求/响应 body 计算 CRC32，header 携带网络序 crc32 字段

## Server
相比之下，Server的实现非常简单，只需要单线程处理请求即可，不需要处理并发问题。
简单单线程示例：接收请求、回显 ID，组织 JSON-RPC 响应并附 header（含 CRC）。

# 协议格式说明

## 传输协议结构

每个 RPC 请求/响应都由两部分组成：**Header（二进制头部，12字节）** + **Body（JSON字符串）**

### 1. RPC Header（12字节，网络字节序）

```c
typedef struct rpc_header_t {
    uint32_t version;   // 协议版本号，当前为 1
    uint32_t body_len;  // JSON Body 的字节长度
    uint32_t crc32;     // JSON Body 的 CRC32 校验码
} rpc_header_t;
```

**字段说明：**
- `version` (4字节): 协议版本号，当前固定为 `1`
- `body_len` (4字节): JSON Body 的长度（字节数），最大 1024 字节
- `crc32` (4字节): 对 JSON Body 计算的 CRC32 校验和
- **字节序**: 所有字段使用网络字节序（big-endian），发送时通过 `htonl()` 转换，接收时通过 `ntohl()` 转换

### 2. 请求格式（JSON-RPC 2.0）

**JSON Body 结构：**
```json
{
  "jsonrpc": "2.0",
  "method": "<method-from-idl>",
  "params": { ... },
  "id": <auto-increment>
}
```

**字段说明：**
- `jsonrpc`: 必须为 `"2.0"`，表示使用 JSON-RPC 2.0 协议
- `method`: 调用的方法名（在 IDL 中定义）
- `params`: 参数对象，包含方法所需的所有参数
- `id`: 请求 ID，用于匹配请求和响应，由客户端自动递增生成

**示例（调用 add_i32 方法）：**
```json
{"jsonrpc":"2.0","method":"add_i32","params":{"a":10,"b":20},"id":1}
```

### 3. 响应格式（JSON-RPC 2.0）

**正常响应：**
```json
{
  "jsonrpc": "2.0",
  "result": <返回结果>,
  "id": <与请求一致的 id>
}
```

**错误响应：**
```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": <错误码>,
    "message": "<错误描述>",
    "data": <可选，补充说明>
  },
  "id": <与请求一致的 id>
}
```

**字段说明：**
- `jsonrpc`: 必须为 `"2.0"`
- `id`: 必须与请求的 id 一致，用于客户端匹配响应
- `result`: 正常应答时返回的结果（任意 JSON 类型）
- `error`: 错误应答时的错误信息对象
  - `code`: 错误码（如 -32601 表示方法未找到）
  - `message`: 人类可读的错误描述
  - `data`: 可选的额外错误信息

**示例（正常返回）：**
```json
{"jsonrpc":"2.0","result":30,"id":1}
```

**示例（错误返回）：**
```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32601,
    "message": "Method not found"
  },
  "id": 1
}
```

### 4. 完整传输流程

**发送请求：**
1. 构造 JSON-RPC 2.0 格式的请求 Body
2. 计算 Body 长度和 CRC32
3. 构造 Header（version=1, body_len, crc32）
4. 将 Header 转换为网络字节序
5. 先发送 12 字节 Header
6. 再发送 JSON Body

**接收响应：**
1. 先接收 12 字节 Header
2. 将 Header 转换为主机字节序
3. 根据 body_len 接收 JSON Body
4. 用 Header 中的 crc32 校验 Body 完整性
5. 解析 JSON Body，根据 id 匹配请求
6. 处理 result 或 error

### 5. 协议特性

- **CRC 校验**：所有请求和响应的 Body 都进行 CRC32 校验，确保数据完整性
- **字节序处理**：Header 使用网络字节序，保证跨平台兼容性
- **长连接复用**：支持在同一连接上发送多个请求，避免频繁建立连接的开销
- **异步 ID 匹配**：通过请求 ID 实现异步调用，客户端可以并发发送多个请求

# 构建与运行
```bash
mkdir -p build && cd build
cmake ..
make
# 启动示例 server/client 可执行文件
```

# 如何使用自动生成脚本

首先，要编写 `rpc_idl.json` 文件定义 RPC 接口，文件符合 JSON-RPC 2.0 定义，后续所有代码的生成，都是基于这个文件自动完成的。

生成工具都在 `tools/` 目录下。

## 客户端代码生成

执行 `gen_rpc_client_api.py` 生成客户端代码：
- **rpc_client_api.h**：客户端调用函数的声明
- **rpc_client_api.c**：函数实现，负责将调用封装成 JSON-RPC 请求，发送并接收响应，解析后返回结果

**stub 函数的注意事项：**
- `string` 参数是 `const char*` 类型，所有权归调用方
- `string` 返回值是 `char*` 类型，调用者负责 `free`
- 调用失败会直接返回默认值（如 `NULL` 或 `0`）
- 因为 JSON 的 `number` 不区分类型，为了解决 `int64` 的承载问题，当前会将 `int64` 转换为字符串进行传输

## 服务端代码生成

执行 `gen_rpc_server_skeleton.py` 生成服务端代码：
- **rpc_server_skeleton.h/.c**：定义 `dispatch` 函数，用于解析请求 JSON，路由到对应的 `*_impl` 函数，并封装响应
- **rpc_server_impl.h**：声明需要手动实现的业务函数

**工作流程：**
1. 服务端接收到请求后，调用 `dispatch` 函数解析 `method` 和 `params`
2. 根据 `method` 路由到自动生成的壳函数
3. 壳函数解析参数后调用实际的业务实现函数（`*_impl`）
4. 将返回值封装成 JSON-RPC 响应，加上 RPC Header 返回给客户端


# 目录提示
- `client/rpc_async.c/.h`：异步客户端核心（连接池、epoll 接收、pending、回调队列、超时线程）。
- `crc.c/.h`：CRC32 封装。
- `client/third_party/uthash.h`：hash 表实现。
- `third_party/cJSON.*`：JSON 解析。
