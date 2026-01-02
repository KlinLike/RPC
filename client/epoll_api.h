#ifndef EPC_EPOLL_API_H
#define EPC_EPOLL_API_H

/**
 * @file epoll_api.h
 * @brief 封装 epoll 基础操作的模块化接口
 */

/**
 * @brief 初始化全局 epoll 实例
 * @return 0 成功, -1 失败
 */
int epoll_init(void);

/**
 * @brief 销毁全局 epoll 实例并关闭文件描述符
 * @return 0 成功, -1 失败
 */
int epoll_destroy(void);

/**
 * @brief 将指定文件描述符添加到 epoll 监控列表
 * @param fd 要监控的文件描述符
 * @return 0 成功, -1 失败
 */
int epoll_add_for_read(int fd);

/**
 * @brief 从 epoll 监控列表中移除指定文件描述符
 * @param fd 要移除的文件描述符
 * @return 0 成功, -1 失败
 */
int epoll_del(int fd);

/**
 * @brief 获取全局 epoll 文件描述符
 * @return epoll fd
 */
int get_epoll_fd(void);

#endif // EPC_EPOLL_API_H
