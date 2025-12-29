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

# 构建与运行
```bash
mkdir -p build && cd build
cmake ..
make
# 启动示例 server/client 可执行文件
```

# 如何使用自动生成脚本
首先，要编写rpc_idl.json文件定义RPC接口，文件符合 JSON-RPC 2.0 定义，后续所有代码的生成，都是基于这个文件自动完成的。
-> 生成工具都在tools目录下
## 客户端
执行 gen_rpc_client_api.py 生成客户端用的头文件 rpc_client_api.h 以及实现文件 rpc_client_api.c
  - rpc_client_api.h中定义了客户端需要调用的函数
  - rpc_client_api.c中实现了这些函数，函数内容是组织RPC-JSON请求并发送，接收响应并解析，最后返回

## 服务端
执行 gen_rpc_server_skeleton.py 生成服务端用的头文件 rpc_server_skeleton.h \ rpc_server_impl.h 以及实现文件 rpc_server_skeleton.c。
  - rpc_server_skeleton.h/.c中定义了dispatch函数，用于解析请求JSON，调用对应 *_impl，并返回响应JSON字符串
  - rpc_server_impl.h中定义了需要手动实现的业务函数的声明

# JSON-RPC 客户端请求的组织

RPC的每次请求，都是这种格式：
{
  "jsonrpc": "2.0",
  "method": "<method-from-idl>",
  "params": { ... },
  "id": <auto-increment>
}

这种格式非常便于解析和处理，所以我们可以轻松实现RPC客户端函数的自动化生成。
生成脚本 gen_rpc_client_api.py 会从 idl 中读取函数名和参数信息，生成 `rpc_client_api.h/.c`。stub 不用直接处理网络传输，只负责将 RPC 调用封装成标准格式并用固定方法发送，接收到响应后解析并返回结果。

stub函数中一些需要注意的细节：
- string参数是const char*类型，所有权归调用方
- string返回值是cahr*类型，调用者负责free
- 调用失败会直接返回默认值，比如NULL或0
- 因为JSON的number没区分类型，可以是int32、int64、float、double等，为了解决int64的承载问题，当前会将int64转换为字符串进行传输

# JSON-RPC 服务端请求的解析和响应组织
工作流程：
1. 服务端接收到请求JSON后，调用dispatch函数解析method和params
2. 根据method找到对应的处理函数，这个函数是程序自动生成的壳函数
3. 壳函数解析后，会调用实际的业务处理函数，dispatch函数获取到返回值后，将返回值封装成JSON响应
4. 服务器端程序将JSON响应和RPC Header一起返回给客户端

# 目录提示
- `client/rpc_async.c/.h`：异步客户端核心（连接池、epoll 接收、pending、回调队列、超时线程）。
- `crc.c/.h`：CRC32 封装。
- `client/third_party/uthash.h`：hash 表实现。
- `third_party/cJSON.*`：JSON 解析。
