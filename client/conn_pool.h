#ifndef EPC_CONN_POOL_H
#define EPC_CONN_POOL_H

#include <stdbool.h>
#include <pthread.h>
#include "rpc_async.h"

/**
 * @file conn_pool.h
 * @brief 连接池模块接口
 */

 
/**
 * @brief 连接池成员结构体
 */
typedef struct {
    int fd;
    bool in_use;
    bool registered; // 是否已添加到 epoll
} rpc_conn_t;

/**
 * @brief 初始化连接池
 * @param ip 服务端 IP
 * @param port 服务端端口
 * @param max_conn 最大连接数
 * @return 0 成功, -1 失败
 */
int rpc_pool_init(const char* ip, int port, int max_conn);

/**
 * @brief 从连接池获取一个可用连接
 * @param status_out 输出状态码
 * @return 成功返回 fd, 失败返回 -1
 */
int rpc_pool_get_conn(rpc_error_code* status_out);

/**
 * @brief 归还连接到连接池
 * @param fd 文件描述符
 * @param bad 是否是损坏的连接（如果是则会被关闭并移除）
 */
void rpc_pool_put_conn(int fd, bool bad);

/**
 * @brief 销毁连接池，释放所有资源
 */
void rpc_pool_destroy(void);

#endif // EPC_CONN_POOL_H
