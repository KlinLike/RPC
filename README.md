# 工作流程
1. 编写rpc_idl.json。这是按照 JSON-RPC 2.0 的方式来定义的
2. 执行gen_rpc_func_h.py，生成rpc_api.h
3. 执行gen_rpc_client_stub.py，生成rpc_client_stub.c
4. 在客户端代码中包含rpc_api.h并调用生成的函数(rpc_api.h中定义的函数，会在rpc_client_stub.c中实现)

# JSON-RPC 客户端请求的组织

RPC的每次请求，都是这种格式：
{
  "jsonrpc": "2.0",
  "method": "<method-from-idl>",
  "params": { ... },
  "id": <auto-increment>
}

这种格式非常便于解析和处理，所以我们可以轻松实现RPC客户端函数的自动化生成。
生成脚本gen_rpc_client_stub.py不用关心具体的什么函数，只需要负责从idl中读取 函数名 和 参数信息，并生成相应的RPC调用代码。
并且项目抽象了公共层，stub不用直接处理网络传输，只需要负责将RPC调用封装成标准格式并用固定方法发送，接收到响应后解析并返回结果。这也是一个非常固定的模式。

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
