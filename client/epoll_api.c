#include <sys/epoll.h>
#include <unistd.h>
#include <stdio.h>
#include "epoll_api.h"

// 全局 epoll 文件描述符，初始化为 -1 表示未创建
static int g_epoll_fd = -1;

/**
 * 初始化 epoll 实例
 */
int epoll_init(void) {
    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }
    return 0;
}

/**
 * 销毁 epoll 实例，释放相关资源
 */
int epoll_destroy(void) {
    if (g_epoll_fd >= 0) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
    }
    return 0;
}

/**
 * 向 epoll 实例中添加监控的文件描述符
 * 默认监控: 可读(EPOLLIN), 可写(EPOLLOUT), 错误(EPOLLERR), 挂断(EPOLLHUP)
 */
int epoll_add_for_read(int fd) {
    if (g_epoll_fd < 0) {
        return -1;
    }
    struct epoll_event ev;
    // EPOLLIN: 表示对应的文件描述符可以读
    // EPOLLERR: 表示对应的文件描述符发生错误
    // EPOLLHUP: 表示对应的文件描述符被挂断
    // EPOLLRDHUP: 表示对端关闭了写端（TCP半关闭）
    // 注意：不监听 EPOLLOUT（可写），避免不必要的事件触发
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    ev.data.fd = fd;

    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl add");
        return -1;
    }
    return 0;
}

/**
 * 从 epoll 实例中移除指定的文件描述符
 */
int epoll_del(int fd) {
    if (g_epoll_fd < 0) {
        return -1;
    }
    // 在删除时，ev 参数在某些内核版本下可以为 NULL，为了兼容性通常传递有效指针
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        perror("epoll_ctl del");
        return -1;
    }
    return 0;
}

/**
 * 获取当前的 epoll 文件描述符
 */
int get_epoll_fd(void) {
    return g_epoll_fd;
}
