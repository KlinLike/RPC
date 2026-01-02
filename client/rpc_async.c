#include <time.h>
#include "rpc_async.h"

#include <arpa/inet.h>
#include <errno.h>

#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "rpc.h"
#include "crc.h"
#include "third_party/uthash.h"


// ------------------------ 异步实现 Ver 2 ------------------------

// 生成下一个唯一的异步请求 ID（线程安全）
uint32_t rpc_async_next_id(void) {
    return atomic_fetch_add(&g_req_id, 1);
}

// 同步（阻塞）调用封装：内部仍走异步管线，但在本线程阻塞等待结果。
// json: 请求 JSON（需包含与 id 对应的 "id" 字段）
// id: 调用方指定的请求 ID（必须非 0）
// body_out/body_len_out: 若非 NULL，将拷贝响应 body 返回，调用方负责 free(*body_out)
// status_out: 若非 NULL，返回最终状态
// 返回 0 表示已收齐；<0 表示未发送或等待失败
int rpc_async_call_blocking(const char* json, uint32_t id, char** body_out, size_t* body_len_out, rpc_async_status_t* status_out){
    // 1. 初始化 future
    rpc_future_t future = {0};
    pthread_mutex_init(&future.mutex, NULL);
    pthread_cond_init(&future.cond, NULL);

}

// ------------------------ 异步实现 Ver 1 ------------------------

// 前向声明（避免顺序导致的隐式声明）
static int rpc_epoll_add(int fd);
static void rpc_epoll_del(int fd);
static int rpc_recv_all(int fd, void* buf, size_t len);
static int rpc_start_timeout_thread(void);
static void rpc_stop_timeout_thread(void);
static void* rpc_timeout_thread_fn(void* arg);
static void* rpc_recv_thread_fn(void* arg);
static void rpc_blocking_cb(uint32_t id,
                            rpc_async_status_t status,
                            const char* body,
                            size_t body_len,
                            void* user_data);
static void rpc_blocking_ctx_init(rpc_blocking_ctx_t* ctx);
static void rpc_blocking_ctx_destroy(rpc_blocking_ctx_t* ctx);

// --------------- 连接池（简单版，占位实现） ---------------

typedef struct {
    int fd;
    bool in_use;
    bool registered; // 是否已添加到epoll
} rpc_conn_t;

static rpc_conn_t* g_pool = NULL;   // 连接池
static int g_pool_size = 0;         // 连接池大小
static char g_ip[64] = {0};         // 服务 IP
static int g_port = 0;              // 服务端口
static int g_epoll_fd = -1;         // epoll fd

static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;

// 简单连接函数（阻塞）
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
        // IP地址无效
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

// 获取连接（简单策略：先找空闲可用，若没有且有空槽则新建，否则返回忙）
// 从连接池获取可用 fd；无可用且有空槽则新建，否则返回 -1/EBUSY
static int rpc_pool_get_conn(void) {
    int fd = -1;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].in_use)
            continue;
        if (g_pool[i].fd >= 0) {
            g_pool[i].in_use = true;
            // 确保已在 epoll
            if (!g_pool[i].registered && g_epoll_fd >= 0) {
                if (rpc_epoll_add(g_pool[i].fd) == 0) {
                    g_pool[i].registered = true;
                }
            }
            fd = g_pool[i].fd;
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
                pthread_mutex_unlock(&g_pool_mu);
                return -1;
            }
            g_pool[i].fd = fd;
            g_pool[i].in_use = true;
            if (g_epoll_fd >= 0 && rpc_epoll_add(fd) == 0) {
                g_pool[i].registered = true;
            } else {
                g_pool[i].registered = false;
            }
            pthread_mutex_unlock(&g_pool_mu);
            return fd;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    errno = EBUSY;
    return -1;
}

// 归还 fd；若 bad=true 则关闭并回收槽位
static void rpc_pool_put_conn(int fd, bool bad) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].fd == fd) {
            if (bad) {
                if (g_pool[i].registered) {
                    rpc_epoll_del(g_pool[i].fd);
                    g_pool[i].registered = false;
                }
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
static void rpc_pool_destroy(void) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_pool) {
        for (int i = 0; i < g_pool_size; ++i) {
            if (g_pool[i].fd >= 0) {
                if (g_pool[i].registered) {
                    rpc_epoll_del(g_pool[i].fd);
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
}

// 通过 fd 找到池槽位索引，找不到返回 -1
static int rpc_pool_index_by_fd(int fd) {
    for (int i = 0; i < g_pool_size; ++i) {
        if (g_pool[i].fd == fd) return i;
    }
    return -1;
}

// --------------- pending 表（基础操作） ---------------

typedef struct rpc_pending_s {
    uint32_t id;
    int fd;
    rpc_async_cb cb;
    void* user_data;
    long long expire_ms; // 绝对超时，后续使用
    UT_hash_handle hh; // uthash的hash表
} rpc_pending_t;

static rpc_pending_t* g_pending = NULL;
static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER; // uthash不线程安全
static uint32_t g_next_id = 1;

// TODO 删除这个函数
static uint32_t rpc_next_id(void) {
    // 简单递增，不做溢出处理（实践中可跳过0）
    return __sync_fetch_and_add(&g_next_id, 1);
}

// 注册一个挂起请求 添加 pending，成功返回指针，失败返回 NULL
static rpc_pending_t* rpc_pending_add(uint32_t id, int fd, rpc_async_cb cb, void* user_data, long long expire_ms) {
    // 并发量不大，忽略ID冲突
    rpc_pending_t* p = calloc(1, sizeof(rpc_pending_t));
    if (!p) return NULL;
    p->id = id;
    p->fd = fd;
    p->cb = cb;
    p->user_data = user_data;
    p->expire_ms = expire_ms;
    pthread_mutex_lock(&g_pending_mu);
    HASH_ADD_INT(g_pending, id, p);
    pthread_mutex_unlock(&g_pending_mu);
    return p;
}

// 按 id 取出并移除挂起请求
static rpc_pending_t* rpc_pending_take(uint32_t id) {
    rpc_pending_t* p = NULL;
    pthread_mutex_lock(&g_pending_mu);
    HASH_FIND_INT(g_pending, &id, p);
    if (p) {
        HASH_DEL(g_pending, p);
    }
    pthread_mutex_unlock(&g_pending_mu);
    return p;
}

// 更新挂起请求的 fd（不分配新节点），成功返回 0
// 使用的时候，是先注册pending再获取连接的，所以需要有通过id更新fd的功能
static int rpc_pending_set_fd(uint32_t id, int fd) {
    int ret = -1;
    pthread_mutex_lock(&g_pending_mu);
    rpc_pending_t* p = NULL;
    HASH_FIND_INT(g_pending, &id, p);
    if (p) {
        p->fd = fd;
        ret = 0;
    }
    pthread_mutex_unlock(&g_pending_mu);
    return ret;
}

// 按 fd 取出一个挂起请求（假设单连接单并发）
static rpc_pending_t* rpc_pending_take_by_fd(int fd) {
    rpc_pending_t *p, *tmp, *found = NULL;
    pthread_mutex_lock(&g_pending_mu);
    HASH_ITER(hh, g_pending, p, tmp) {
        if (p->fd == fd) {
            HASH_DEL(g_pending, p);
            found = p;
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_mu);
    return found;
}

// 移除指定 fd 的所有 pending（例如连接坏掉）
// 移除某 fd 上的所有挂起请求并触发错误回调
static void rpc_pending_remove_by_fd(int fd, rpc_async_status_t status) {
    rpc_pending_t *p, *tmp;
    pthread_mutex_lock(&g_pending_mu);
    HASH_ITER(hh, g_pending, p, tmp) {
        if (p->fd == fd) {
            HASH_DEL(g_pending, p);
            // 回调放到此处后续可改为投递队列；先直接调用占位
            if (p->cb) {
                p->cb(p->id, status, NULL, 0, p->user_data);
            }
            free(p);
        }
    }
    pthread_mutex_unlock(&g_pending_mu);
}

// --------------- 回调队列与消费线程骨架 ---------------

typedef struct {
    uint32_t id;
    rpc_async_status_t status;
    char* body;     // body由生产者malloc/copy，保存RPC Request Body，消费者调用毁掉后用rpc_resp_event_cleanup函数释放
    size_t body_len;
    rpc_async_cb cb;
    void* user_data;
} rpc_resp_event_t;

// 阻塞等待上下文
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool done;
    rpc_async_status_t status;
    char* body;
    size_t body_len;
} rpc_blocking_ctx_t;

typedef struct {
    rpc_resp_event_t* buf;  // 事件数组
    int capacity;           // 容量
    int head;               // 头指针下标
    int tail;               // 尾指针下标
    int size;               // 当前队中元素个数
    bool stop;              // 停止标志，通知线程退出
    pthread_mutex_t mu;
    pthread_cond_t not_empty;   // 消费者用，等待队列非空
    pthread_cond_t not_full;    // 生产者用，等待队列不满
} rpc_resp_queue_t;

static rpc_resp_queue_t g_resp_q;   // 回调队列
static pthread_t g_cb_thread;       // 回调线程
static int g_default_queue_cap = 16;
static int g_timeout_ms = 3000;     // 默认超时时间
static pthread_t g_timeout_thread;  // 超时扫描线程
static bool g_timeout_stop = false; // 超时线程停止标志
static pthread_t g_recv_thread;     // 接收线程
static bool g_recv_stop = false;

// epoll 辅助
static int rpc_epoll_add(int fd) {
    if (g_epoll_fd < 0) return -1;
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        return -1;
    }
    return 0;
}

static void rpc_epoll_del(int fd) {
    if (g_epoll_fd < 0) return;
    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

// --------------- 时间工具 ---------------

// 当前时间戳（毫秒）
static long long rpc_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

// --------------- 超时扫描线程 ---------------

// 扫描pending超时，推送超时事件
static void rpc_pending_scan_timeout(void) {
    long long now = rpc_now_ms();
    rpc_pending_t *p, *tmp;
    rpc_pending_t** expired = NULL;
    size_t exp_count = 0, cap = 0;

    pthread_mutex_lock(&g_pending_mu);
    HASH_ITER(hh, g_pending, p, tmp) {
        if (p->expire_ms > 0 && now >= p->expire_ms) {
            HASH_DEL(g_pending, p);
            if (exp_count == cap) {
                cap = cap ? cap * 2 : 8;
                rpc_pending_t** newbuf = realloc(expired, cap * sizeof(rpc_pending_t*));
                if (!newbuf) {
                    free(p);
                    continue;
                }
                expired = newbuf;
            }
            expired[exp_count++] = p;
        }
    }
    pthread_mutex_unlock(&g_pending_mu);

    for (size_t i = 0; i < exp_count; ++i) {
        rpc_pending_t* e = expired[i];
        rpc_emit_event(e->id, RPC_ASYNC_TIMEOUT, NULL, 0, e->cb, e->user_data);
        free(e);
    }
    free(expired);
}

static void* rpc_timeout_thread_fn(void* arg) {
    (void)arg;
    while (!g_timeout_stop) {
        rpc_pending_scan_timeout();
        usleep(100 * 1000); // 100ms 周期
    }
    return NULL;
}

static int rpc_start_timeout_thread(void) {
    g_timeout_stop = false;
    if (pthread_create(&g_timeout_thread, NULL, rpc_timeout_thread_fn, NULL) != 0) {
        return -1;
    }
    return 0;
}

static void rpc_stop_timeout_thread(void) {
    g_timeout_stop = true;
    pthread_join(g_timeout_thread, NULL);
}

// 释放事件里可能有的 body 缓冲
static void rpc_resp_event_cleanup(rpc_resp_event_t* ev) {
    if (ev->body) {
        free(ev->body);
        ev->body = NULL;
    }
}

static int rpc_resp_queue_init(int capacity) {
    if (capacity <= 0) return -1;
    g_resp_q.buf = calloc((size_t)capacity, sizeof(rpc_resp_event_t));
    if (!g_resp_q.buf) return -1;
    g_resp_q.capacity = capacity;
    g_resp_q.head = g_resp_q.tail = g_resp_q.size = 0;
    g_resp_q.stop = false;
    pthread_mutex_init(&g_resp_q.mu, NULL);
    pthread_cond_init(&g_resp_q.not_empty, NULL);
    pthread_cond_init(&g_resp_q.not_full, NULL);
    return 0;
}

static void rpc_resp_queue_destroy(void) {
    pthread_mutex_lock(&g_resp_q.mu);
    for (int i = 0; i < g_resp_q.size; ++i) {
        int idx = (g_resp_q.head + i) % g_resp_q.capacity;
        rpc_resp_event_cleanup(&g_resp_q.buf[idx]);
    }
    free(g_resp_q.buf);
    g_resp_q.buf = NULL;
    g_resp_q.size = g_resp_q.capacity = g_resp_q.head = g_resp_q.tail = 0;
    pthread_mutex_unlock(&g_resp_q.mu);
    pthread_mutex_destroy(&g_resp_q.mu);
    pthread_cond_destroy(&g_resp_q.not_empty);
    pthread_cond_destroy(&g_resp_q.not_full);
}

// 队列满会阻塞等待
// 推送事件到回调队列（满则等待，stop 返回 -1）
static int rpc_resp_queue_push(const rpc_resp_event_t* ev) {
    pthread_mutex_lock(&g_resp_q.mu);
    if (g_resp_q.stop) {
        pthread_mutex_unlock(&g_resp_q.mu);
        return -1;
    }
    while (g_resp_q.size == g_resp_q.capacity) {
        // NOTE: 不满足条件的时候，用条件变量等待，条件变量会自动解锁+上锁，这就是为什么wait函数也需要传入互斥锁
        pthread_cond_wait(&g_resp_q.not_full, &g_resp_q.mu);
    }

    g_resp_q.buf[g_resp_q.tail] = *ev;
    g_resp_q.tail = (g_resp_q.tail + 1) % g_resp_q.capacity;
    g_resp_q.size++;
    pthread_cond_signal(&g_resp_q.not_empty);
    pthread_mutex_unlock(&g_resp_q.mu);
    return 0;
}

// 队列空会阻塞等待
// 从回调队列取事件（空则等待，stop+空返回 -1）
static int rpc_resp_queue_pop(rpc_resp_event_t* ev) {
    pthread_mutex_lock(&g_resp_q.mu);
    while (!g_resp_q.stop && g_resp_q.size == 0) {
        // stop = false且队列为空，继续等待
        pthread_cond_wait(&g_resp_q.not_empty, &g_resp_q.mu);
    }
    if (g_resp_q.stop && g_resp_q.size == 0) {
        // stop = true且队列为空，退出
        // stop = true且队列不为空，正常取元素
        pthread_mutex_unlock(&g_resp_q.mu);
        return -1;
    }
    // 这里 size>0，可正常取元素

    *ev = g_resp_q.buf[g_resp_q.head];
    g_resp_q.head = (g_resp_q.head + 1) % g_resp_q.capacity;
    g_resp_q.size--;
    pthread_cond_signal(&g_resp_q.not_full);
    pthread_mutex_unlock(&g_resp_q.mu);
    return 0;
}

static void* rpc_cb_thread_fn(void* arg) {
    (void)arg;
    rpc_resp_event_t ev;
    // pop为空会等待，这里不用做特别处理了
    while (rpc_resp_queue_pop(&ev) == 0) {
        if (ev.cb) {
            ev.cb(ev.id, ev.status, ev.body, ev.body_len, ev.user_data);
        }
        rpc_resp_event_cleanup(&ev);
    }
    return NULL;
}

// 启动回调线程
static int rpc_start_cb_thread(int queue_capacity) {
    if (rpc_resp_queue_init(queue_capacity) != 0) {
        return -1;
    }
    if (pthread_create(&g_cb_thread, NULL, rpc_cb_thread_fn, NULL) != 0) {
        rpc_resp_queue_destroy();
        return -1;
    }
    return 0;
}

// 停止回调线程
static void rpc_stop_cb_thread(void) {
    pthread_mutex_lock(&g_resp_q.mu);
    g_resp_q.stop = true;
    // NOTE: 唤醒后的线程会阻塞在加锁
    pthread_cond_broadcast(&g_resp_q.not_empty);
    pthread_cond_broadcast(&g_resp_q.not_full);
    pthread_mutex_unlock(&g_resp_q.mu);
    pthread_join(g_cb_thread, NULL);
    rpc_resp_queue_destroy();
}

// 带body拷贝的事件推送，body_in可为NULL；若len>0则拷贝
// 生产者侧封装事件并推送回调队列（body 会被拷贝并在消费后释放）
static void rpc_emit_event(uint32_t id,
                           rpc_async_status_t status,
                           const char* body_in,
                           size_t body_len,
                           rpc_async_cb cb,
                           void* user_data) {
    rpc_resp_event_t ev;
    ev.id = id;
    ev.status = status;
    ev.cb = cb;
    ev.user_data = user_data;
    ev.body = NULL;
    ev.body_len = 0;
    if (body_in && body_len > 0) {
        ev.body = malloc(body_len + 1);
        if (ev.body) {
            memcpy(ev.body, body_in, body_len);
            ev.body[body_len] = '\0';
            ev.body_len = body_len;
        }
    }
    if (rpc_resp_queue_push(&ev) != 0) {
        // 队列停或push失败，释放拷贝
        rpc_resp_event_cleanup(&ev);
    }
}

// --------------- 接收线程 ---------------

static void rpc_handle_recv(int fd) {
    rpc_header_t hdr_net;
    if (rpc_recv_all(fd, &hdr_net, RPC_HEADER_LEN) != 0) {
        rpc_pending_remove_by_fd(fd, RPC_ASYNC_RECV_ERR);
        rpc_pool_put_conn(fd, true);
        return;
    }
    rpc_header_t hdr;
    hdr.version = ntohl(hdr_net.version);
    hdr.body_len = ntohl(hdr_net.body_len);
    hdr.crc32 = ntohl(hdr_net.crc32);
    if (hdr.body_len > MAX_BODY_LEN) {
        rpc_pending_remove_by_fd(fd, RPC_ASYNC_RECV_ERR);
        rpc_pool_put_conn(fd, true);
        return;
    }
    char* body = malloc(hdr.body_len + 1);
    if (!body) {
        rpc_pending_remove_by_fd(fd, RPC_ASYNC_RECV_ERR);
        rpc_pool_put_conn(fd, true);
        return;
    }
    if (rpc_recv_all(fd, body, hdr.body_len) != 0) {
        free(body);
        rpc_pending_remove_by_fd(fd, RPC_ASYNC_RECV_ERR);
        rpc_pool_put_conn(fd, true);
        return;
    }
    body[hdr.body_len] = '\0';
    rpc_async_status_t status = RPC_ASYNC_OK;
    if (!rpc_crc32_verify(body, hdr.body_len, hdr.crc32)) {
        status = RPC_ASYNC_CRC_ERR;
    }
    rpc_pending_t* p = rpc_pending_take_by_fd(fd);
    if (p) {
        rpc_emit_event(p->id, status, body, hdr.body_len, p->cb, p->user_data);
        free(p);
    }
    free(body);
    rpc_pool_put_conn(fd, false);
}

static void* rpc_recv_thread_fn(void* arg) {
    (void)arg;
    const int MAX_EVENTS = 32;
    struct epoll_event events[MAX_EVENTS];
    while (!g_recv_stop) {
        int n = epoll_wait(g_epoll_fd, events, MAX_EVENTS, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            if (ev & (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
                // 连接异常，移除所有相关pending并关闭连接
                rpc_pending_remove_by_fd(fd, RPC_ASYNC_RECV_ERR);
                rpc_pool_put_conn(fd, true);
                continue;
            }
            if (ev & EPOLLIN) {
                rpc_handle_recv(fd);
            }
        }
    }
    return NULL;
}

// --------------- 异步接口占位 ---------------

int rpc_async_init(const char* ip, int port, int max_conn, int queue_size, int timeout_ms)
{
    (void)queue_size;
    if (!ip || ip[0] == '\0' || port <= 0 || max_conn <= 0) {
        return -1;
    }
    if ((size_t)snprintf(g_ip, sizeof(g_ip), "%s", ip) >= sizeof(g_ip)) {
        return -1;
    }
    g_port = port;
    g_pool_size = max_conn;
    g_pool = calloc((size_t)g_pool_size, sizeof(rpc_conn_t));
    if (!g_pool) {
        return -1;
    }
    for (int i = 0; i < g_pool_size; ++i) {
        g_pool[i].fd = -1;
        g_pool[i].in_use = false;
    }
    if (timeout_ms > 0) {
        g_timeout_ms = timeout_ms;
    }
    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) {
        rpc_pool_destroy();
        return -1;
    }
    if (rpc_start_cb_thread(queue_size > 0 ? queue_size : g_default_queue_cap) != 0) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
        rpc_pool_destroy();
        return -1;
    }
    if (rpc_start_timeout_thread() != 0) {
        rpc_stop_cb_thread();
        close(g_epoll_fd);
        g_epoll_fd = -1;
        rpc_pool_destroy();
        return -1;
    }
    g_recv_stop = false;
    if (pthread_create(&g_recv_thread, NULL, rpc_recv_thread_fn, NULL) != 0) {
        rpc_stop_timeout_thread();
        rpc_stop_cb_thread();
        close(g_epoll_fd);
        g_epoll_fd = -1;
        rpc_pool_destroy();
        return -1;
    }
    return 0;
}

void rpc_async_shutdown(void)
{
    g_recv_stop = true;
    if (g_epoll_fd >= 0) {
        close(g_epoll_fd); // 唤醒 epoll_wait
        g_epoll_fd = -1;
    }
    pthread_join(g_recv_thread, NULL);
    rpc_stop_timeout_thread();
    rpc_stop_cb_thread();
    rpc_pool_destroy();
}

// 发送固定长度数据（阻塞重试 EINTR），失败返回 -1
static int rpc_send_all(int fd, const void* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char*)buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

// 接收固定长度数据（阻塞重试 EINTR），失败返回 -1
static int rpc_recv_all(int fd, void* buf, size_t len) {
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(fd, (char*)buf + recvd, len - recvd, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; // 对端关闭
        recvd += (size_t)n;
    }
    return 0;
}

// 异步请求: 注册 pending，发送RPC，记录 fd，返回 0/-1
int rpc_async_call(const char* json, rpc_async_cb cb, void* user_data, uint32_t id)
{
    if (!json || !cb) {
        return -1;
    }
    size_t body_len = strlen(json);
    if (body_len > MAX_BODY_LEN) {
        return -1;
    }

    // 使用调用方提供的请求ID并注册pending
    if (id == 0) {
        return -1;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long now_ms = (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    long long expire_ms = now_ms + g_timeout_ms;

    if (!rpc_pending_add(id, -1, cb, user_data, expire_ms)) {
        return -1;
    }

    int fd = rpc_pool_get_conn();
    if (fd < 0) {
        rpc_pending_t* p = rpc_pending_take(id);
        if (p) free(p);
        return -1;
    }

    // 组装并发送请求（同步发送，占位实现）
    rpc_header_t header;
    header.version = htonl(1);
    header.body_len = htonl((uint32_t)body_len);
    header.crc32 = htonl(rpc_crc32(json, body_len));

    if (rpc_send_all(fd, &header, RPC_HEADER_LEN) != 0) {
        rpc_pool_put_conn(fd, true);
        rpc_pending_t* p = rpc_pending_take(id);
        if (p) {
            rpc_emit_event(id, RPC_ASYNC_SEND_ERR, NULL, 0, p->cb, p->user_data);
            free(p);
        }
        return -1;
    }

    if (rpc_send_all(fd, json, body_len) != 0) {
        rpc_pool_put_conn(fd, true);
        rpc_pending_t* p = rpc_pending_take(id);
        if (p) {
            rpc_emit_event(id, RPC_ASYNC_SEND_ERR, NULL, 0, p->cb, p->user_data);
            free(p);
        }
        return -1;
    }

    // 暂存fd到pending
    if (rpc_pending_set_fd(id, fd) != 0) {
        rpc_pool_put_conn(fd, true);
        rpc_pending_t* p = rpc_pending_take(id);
        if (p) {
            rpc_emit_event(id, RPC_ASYNC_SEND_ERR, NULL, 0, p->cb, p->user_data);
            free(p);
        }
        return -1;
    }

    return 0;
}

