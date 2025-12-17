# 工作流程
1. 编写rpc_idl.json。这是按照 JSON-RPC 2.0 的方式来定义的
2. 执行gen_rpc_func_h.py，生成rpc_api.h
3. 执行gen_rpc_client_stub.py，生成rpc_client_stub.c
4. 在客户端代码中包含rpc_api.h并调用生成的函数(rpc_api.h中定义的函数，会在rpc_client_stub.c中实现)