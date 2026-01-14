# RPC 框架使用指南

基于 JSON-RPC 2.0 的轻量级 RPC 实现。采用 **“连接池 + 全时 epoll 监听 + 协议级心跳”** 模型，支持高并发异步调用、CRC32 校验、请求超时处理及自动代码生成。

---

## 1. 核心流程：从 IDL 到调用

项目的核心开发流程是：**定义 IDL -> 运行脚本生成代码 -> 实现服务端业务 -> 客户端调用**。

### 第一步：定义接口 (rpc_idl.json)
所有的 RPC 接口都定义在 `rpc_idl.json` 中。
- **method**: 方法名。
- **params**: 参数列表（支持 `i32`, `i64`, `double`, `bool`, `string`）。
- **result**: 返回值类型。

```json
{
  "functions": [
    {
      "method": "add_i32",
      "params": {
        "properties": {
          "a": { "type": "i32" },
          "b": { "type": "i32" }
        }
      },
      "result": { "type": "i32" }
    }
  ]
}
```

### 第二步：生成代码 (Tools)
使用 `tools/` 目录下的 Python 脚本根据 IDL 自动生成前后端胶水代码。

```bash
# 生成客户端代码 (client/gen/rpc_client_gen.*)
python3 tools/gen_rpc_client.py

# 生成服务端代码 (server/gen/rpc_server_skeleton.* 和 server/rpc_server_impl.h)
python3 tools/gen_rpc_server.py
```

### 第三步：实现服务端业务 (rpc_server_impl.c)
脚本会生成接口声明，你只需要在 [rpc_server_impl.c](file:///root/github/RPC/server/rpc_server_impl.c) 中编写具体的业务逻辑。

```c
// 示例：实现 add_i32 接口
int32_t add_i32_impl(int32_t a, int32_t b) {
    return a + b;
}

// 示例：实现 ping 接口（注意：返回字符串需通过 malloc 分配，框架会自动 free）
char* ping_impl(void) {
    return strdup("pong");
}
```

### 第四步：客户端发起调用
客户端包含自动生成的函数桩（Stub），可以像调用本地函数一样发起 RPC 调用。

```c
#include "rpc_async.h"
#include "rpc_client_gen.h"

int main() {
    // 1. 初始化异步系统（IP, 端口, 连接池大小, 超时ms）
    rpc_async_init("127.0.0.1", 8888, 10, 5000);

    // 2. 直接调用自动生成的函数
    int32_t sum = add_i32(10, 20);
    printf("Result: %d\n", sum);

    char* s = ping();
    printf("Server says: %s\n", s);
    free(s); // 记得释放返回的字符串

    // 3. 关闭系统
    rpc_async_shutdown();
    return 0;
}
```

---

## 2. 关键特性说明

### 协议级心跳 (Ping/Pong)
- **保活机制**：连接池会对空闲连接自动发送协议级的 `PING` 包（非业务逻辑）。
- **失效清理**：如果服务器在 20 秒内没有响应 `PONG`，客户端会自动断开并清理该失效连接。
- **全时监听**：所有连接在建立后立即加入 epoll 监听，能够实时感知对端关闭或心跳响应。

### 连接池管理
- **自动扩容**：初始分配连接，若连接不足且未达上限，会自动创建新连接。
- **自动重连**：当连接损坏被回收后，下次调用会自动触发重连。

### 数据安全与完整性
- **CRC32**：每个 Body 都会计算 CRC32 校验和，并在 Header 中传输。
- **Packed Header**：二进制头部采用 `__attribute__((packed))`，消除字节对齐差异，确保跨语言/跨平台兼容。

---

## 3. 编译与运行

```bash
mkdir build && cd build
cmake ..
make

# 启动服务端
./server

# 启动客户端测试
./client
```

---

## 4. 目录结构

- `tools/`: 核心生成脚本。
- `client/`: 异步客户端实现、连接池、epoll 核心。
- `server/`: 服务端主循环、请求分发逻辑。
- `rpc.h`: 协议头部定义。
- `rpc_idl.json`: 接口定义文件。
