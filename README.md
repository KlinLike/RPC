
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
3. 壳函数解析后，会调用实际的业务处理函数，disptach函数获取到返回值后，将返回值封装成JSON响应
4. 服务器端程序将JSON响应和RPC Header一起返回给客户端



# 未来的改进计划
- 支持并行调用
- stub函数打印错误信息
- 增加超时机制
