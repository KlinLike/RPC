
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "conn_pool.h"
#include "epoll_api.h"

#define COMPATIBLE  0   // 兼容性开关

// ---------- 连接池全局变量 ----------
static rpc_conn_t* g_pool = NULL;   // 连接池
static int g_pool_size = 0;         // 连接池大小
static char g_ip[64] = {0};         // 服务 IP
static int g_port = 0;              // 服务端口
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;   // 连接池互斥锁

// ---------- 内部辅助函数 ----------

/**
 * 建立一个连接并返回 fd
 */
static int rpc_connect_once(const char* ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

// ---------- 连接池外部接口 ----------

int rpc_pool_init(const char* ip, int port, int max_conn) {
    if (g_pool) {
        return -1; // 连接池已初始化
    }
    
    #if COMPATIBLE
    // 重新初始化互斥锁，以防 destroy 后再次 init
    pthread_mutex_init(&g_pool_mu, NULL);
    #endif

    if (!ip || ip[0] == '\0' || port <= 0 || max_conn <= 0) {
        return -1;
    }
    if ((size_t)snprintf(g_ip, sizeof(g_ip), "%s", ip) >= sizeof(g_ip)) {
        return -1; // IP字符串过长
    }
    g_port = port;
    g_pool_size = max_conn;

    g_pool = calloc((size_t)g_pool_size, sizeof(rpc_conn_t));
    if (!g_pool) {
        return -1;
    }

    // 初始化 fd 为 -1，避免 calloc 默认的 0 (stdin) 被误关闭
    for (int i = 0; i < g_pool_size; ++i) {
        g_pool[i].fd = -1;
    }

    // 连接池初始化：创建并连接 max_conn 个连接
    // NOTE:如果一开始就都连上，调用的速度会快。但如果到用的时候再连，可以节省资源（包括服务器资源）
    for (int i = 0; i < g_pool_size; ++i) {
        int fd = rpc_connect_once(ip, port);
        if (fd < 0) {
            rpc_pool_destroy();
            return -1;
        }
        g_pool[i].fd = fd;
        g_pool[i].in_use = false;
        g_pool[i].registered = false;
    }

    // 注意：epoll 应该在 rpc_async_init 中统一初始化，这里不再调用 epoll_init()
    // 连接池依赖 epoll，但初始化顺序由上层控制
    // 调用方必须确保在调用 rpc_pool_init 之前已经调用了 epoll_init()

    return 0;
}

// 获取连接（简单策略：先找空闲可用，若没有且有空槽则新建，否则返回忙）
// 从连接池获取可用 fd；无可用且有空槽则新建，否则返回 -1/EBUSY
int rpc_pool_get_conn(rpc_error_code* status_out) {
    if (get_epoll_fd() < 0) {
        if (status_out) *status_out = RPC_OTHER_ERR;
        return -1;
    }

    int fd = -1;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].in_use)
            continue;
        if (g_pool[i].fd >= 0) {
            g_pool[i].in_use = true;
            // 确保已在 epoll中监听
            if (!g_pool[i].registered) {
                if (epoll_add_for_read(g_pool[i].fd) == 0) {
                    g_pool[i].registered = true;
                }
            }
            fd = g_pool[i].fd;
            if (status_out) *status_out = RPC_OK;
            pthread_mutex_unlock(&g_pool_mu);
            return fd;
        }
    }
    // 尝试新建
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].fd < 0 && !g_pool[i].in_use) {
            g_pool[i].in_use = true; // 先占位
            pthread_mutex_unlock(&g_pool_mu);
            fd = rpc_connect_once(g_ip, g_port);
            pthread_mutex_lock(&g_pool_mu);
            if (fd < 0) {
                g_pool[i].in_use = false;
                if (status_out) *status_out = RPC_CONN_ERR;
                pthread_mutex_unlock(&g_pool_mu);
                return -1;
            }
            g_pool[i].fd = fd;
            g_pool[i].in_use = true;
            if (epoll_add_for_read(fd) == 0) {
                g_pool[i].registered = true;
            } else {
                g_pool[i].registered = false;
            }
            if (status_out) *status_out = RPC_OK;
            pthread_mutex_unlock(&g_pool_mu);
            return fd;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    if (status_out) *status_out = RPC_OTHER_ERR; // 连接池忙
    errno = EBUSY;
    return -1;
}

// 归还 fd；若 bad=true 则关闭并回收槽位
void rpc_pool_put_conn(int fd, bool bad) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].fd == fd) {
            if (g_pool[i].registered) {
                epoll_del(g_pool[i].fd);
                g_pool[i].registered = false;
            }
            if (bad) {
                close(g_pool[i].fd); 
                g_pool[i].fd = -1;
            }
            g_pool[i].in_use = false;
            pthread_mutex_unlock(&g_pool_mu);
            return;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    // 未找到：关闭避免泄漏
    if (fd >= 0) close(fd);
}

// 进来前需要保证没有线程在使用连接池
void rpc_pool_destroy(void) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_pool) {
        for (int i = 0; i < g_pool_size; ++i) {
            if (g_pool[i].fd >= 0) {
                if (g_pool[i].registered) {
                    epoll_del(g_pool[i].fd);
                }
                close(g_pool[i].fd);
                g_pool[i].fd = -1;
            }
        }
        free(g_pool);
        g_pool = NULL;
    }
    g_pool_size = 0;
    pthread_mutex_unlock(&g_pool_mu);

    #if COMPATIBLE
    // 销毁互斥锁（注意：静态初始化的锁在某些平台上也建议销毁）
    pthread_mutex_destroy(&g_pool_mu);
    #endif

    epoll_destroy();
}