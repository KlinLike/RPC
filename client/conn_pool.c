
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "conn_pool.h"
#include "epoll_api.h"

#include <sys/time.h>
#include "rpc.h"

#define COMPATIBLE  0   // 兼容性开关
#define HEARTBEAT_INTERVAL_MS 10000 // 10秒不活跃发送心跳

// ---------- 连接池全局变量 ----------
static rpc_conn_t* g_pool = NULL;   // 连接池
static int g_pool_size = 0;         // 连接池大小
static char g_ip[64] = {0};         // 服务 IP
static int g_port = 0;              // 服务端口
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;   // 连接池互斥锁

// ---------- 内部辅助函数 ----------

static long long get_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

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

    // 设置为非阻塞模式（必须！配合 epoll 使用）
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        close(fd);
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
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
        g_pool[i].last_active_ms = get_now_ms();
        
        // 核心改进：初始化时就加入 epoll 监听，以便处理心跳 PONG 和检测对端关闭
        epoll_add_for_read(fd);
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
            // 连接已经在初始化或归还时保持在 epoll 中，无需重复添加
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
            g_pool[i].last_active_ms = get_now_ms();
            epoll_add_for_read(fd);
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

void rpc_pool_update_active(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].fd == fd) {
            g_pool[i].last_active_ms = get_now_ms();
            pthread_mutex_unlock(&g_pool_mu);
            return;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
}

// 归还 fd；若 bad=true 则关闭并回收槽位
void rpc_pool_put_conn(int fd, bool bad) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].fd == fd) {
            if (bad) {
                // 只有在连接确认失效时才从 epoll 移除并关闭
                epoll_del(g_pool[i].fd);
                close(g_pool[i].fd); 
                g_pool[i].fd = -1;
            } else {
                // 正常归还：保持 epoll 监听，仅更新活跃时间
                g_pool[i].last_active_ms = get_now_ms();
            }
            g_pool[i].in_use = false;
            pthread_mutex_unlock(&g_pool_mu);
            return;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    // 未找到：说明是外部创建或已清理的 fd，直接关闭避免泄漏
    if (fd >= 0) close(fd);
}

// 进来前需要保证没有线程在使用连接池
void rpc_pool_destroy(void) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_pool) {
        for (int i = 0; i < g_pool_size; ++i) {
            if (g_pool[i].fd >= 0) {
                epoll_del(g_pool[i].fd);
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

void rpc_pool_heartbeat(void) {
    if (!g_pool) return;

    long long now = get_now_ms();
    
    // 构造 PING 包缓冲区 (在循环外构造一次，重复使用)
    uint8_t ping_buf[RPC_HEADER_LEN];
    uint16_t v = htons(1);
    uint16_t t = htons(RPC_TYPE_PING);
    uint32_t bl = 0;
    uint32_t c = 0;
    memcpy(ping_buf, &v, 2);
    memcpy(ping_buf + 2, &t, 2);
    memcpy(ping_buf + 4, &bl, 4);
    memcpy(ping_buf + 8, &c, 4);

    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_size; i++) {
        if (g_pool[i].fd < 0) continue;

        // 1. 检查是否长期没有响应 (如 2 倍心跳间隔)
        if (now - g_pool[i].last_active_ms > HEARTBEAT_INTERVAL_MS * 2) {
            printf("[Heartbeat] Connection on fd %d timeout (no PONG for %lld ms), closing\n", 
                   g_pool[i].fd, now - g_pool[i].last_active_ms);
            epoll_del(g_pool[i].fd);
            close(g_pool[i].fd);
            g_pool[i].fd = -1;
            g_pool[i].in_use = false;
            continue;
        }

        // 2. 仅对空闲且活跃时间超过间隔的连接发送心跳
        if (!g_pool[i].in_use && (now - g_pool[i].last_active_ms > HEARTBEAT_INTERVAL_MS)) {
            
            printf("[Heartbeat] Sending PING to fd %d\n", g_pool[i].fd);
            ssize_t n = send(g_pool[i].fd, ping_buf, RPC_HEADER_LEN, 0);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 信号中断或缓冲区满，本次跳过，不关闭连接
                    // 出错跳过，很快这个函数又会再次执行，不用担心链接失效很久都没发现
                    continue;
                }
                printf("[Heartbeat] Send PING failed on fd %d, error: %s, closing\n", 
                       g_pool[i].fd, strerror(errno));
                close(g_pool[i].fd);
                g_pool[i].fd = -1;
            } else {
                // 更新时间，防止在等待 PONG 期间重复发送
                g_pool[i].last_active_ms = now;
            }
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
}