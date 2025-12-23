#pragma once

// RPC客户端初始化函数
// 参数: ip - 服务器IP地址, port - 服务器端口号
// 返回值: 0表示成功，非0表示失败
int rpc_client_init(const char* ip, int port);
// RPC客户端调用函数
// 参数: json - JSON格式的RPC请求
// 返回值: 返回JSON格式的RPC响应，调用者需要释放返回的内存
char* rpc_client_call(const char* json);
