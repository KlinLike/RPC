#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>

#include "rpc.h"

// 初始化服务端socket
int init_server(int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        perror("bind");
        return -1;
    }

    if (listen(sockfd, 5) < 0)
    {
        perror("listen");
        return -1;
    }

    printf("server listen on port %d\n", port);
    return sockfd;
}

int main(int argc, char *argv[])
{
    // 初始化epoll
    int epollfd = epoll_create(1);
    if (epollfd < 0)
    {
        perror("epoll_create");
        return -1;
    }

    // 初始化服务端socket
    int port = 8888;
    int sockfd = init_server(port);
    if (sockfd < 0)
    {
        perror("init_server");
        return -1;
    }

    // 注册服务端socket到epoll
    struct epoll_event ev;
    ev.data.fd = sockfd;
    ev.events = EPOLLIN;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev) < 0)
    {
        perror("epoll_ctl");
        return -1;
    }

    // 主循环
    struct epoll_event events[10];
    while (1)
    {
        int nfds = epoll_wait(epollfd, events, 10, -1);
        if (nfds < 0)
        {
            perror("epoll_wait");
            return -1;
        }

        for (int i = 0; i < nfds; i++)
        {
            if (events[i].data.fd == sockfd)
            {
                // 处理新连接
                int connfd = accept(sockfd, NULL, NULL);
                if (connfd < 0)
                {
                    perror("accept");
                    return -1;
                }

                // 注册新连接到epoll
                ev.data.fd = connfd;
                ev.events = EPOLLIN;
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &ev) < 0)
                {
                    perror("epoll_ctl");
                    return -1;
                }
            }
            else
            {
                // 处理已连接socket
                int connfd = events[i].data.fd;
                if (connfd < 0)
                {
                    perror("connfd");
                    return -1;
                }

                bool is_error = false;

                // 接收RPC Header
                char rpc_header[RPC_HEADER_LEN] = {0};
                ssize_t recv_bytes = 0;
                while (recv_bytes < RPC_HEADER_LEN)
                {
                    recv_bytes = recv(connfd, rpc_header + recv_bytes, RPC_HEADER_LEN - recv_bytes, 0); 
                    if (recv_bytes == -1 && errno == EINTR){
                        continue;
                    }
                    if (recv_bytes == 0){
                        // 对端关闭连接
                        perror("recv header: connection closed");
                        is_error = true;
                        break;
                    }
                    if (recv_bytes < 0){
                        // 异常
                        perror("recv header");
                        is_error = true;
                        break;
                    }
                }
                if (is_error){
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    continue;
                }

                // 解析RPC Header
                rpc_header_t *header = (rpc_header_t *)rpc_header;
                uint32_t version = ntohl(header->version);
                uint32_t body_len = ntohl(header->body_len);
                uint32_t crc32 = ntohl(header->crc32);

                // 接收RPC Body
                char* body = malloc(body_len + 1);
                recv_bytes = 0;
                while (recv_bytes < body_len){
                    recv_bytes = recv(connfd, body + recv_bytes, body_len - recv_bytes, 0); 
                    if (recv_bytes == -1 && errno == EINTR){
                        continue;
                    }
                    if (recv_bytes == 0){
                        // 对端关闭连接
                        printf("recv body: connection closed");
                        is_error = true;
                        break;
                    }
                    if (recv_bytes < 0){
                        perror("recv body");
                        is_error = true;
                        break;
                    }
                }
                if (is_error){
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    free(body);
                    continue;
                }
                body[body_len] = '\0';
                printf("recv %d bytes: %s\n", body_len, body);

                // TODO 解析RPC Body
                // 先原样返回
                ssize_t send_bytes = 0;
                while (send_bytes < RPC_HEADER_LEN){
                    ssize_t n = send(connfd, rpc_header + send_bytes, RPC_HEADER_LEN - send_bytes, 0);
                    if (n == -1 && errno == EINTR) {
                        continue; // EINTR等临时错误，继续重试
                    }
                    if (n < 0) {
                        perror("send rpc body");
                        is_error = true;
                        break;
                    }
                    if (n == 0) {
                        printf("send rpc body: connection closed\n");
                        is_error = true;
                        break;
                    }
                    send_bytes += n;
                }
                if (is_error) {
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    free(body);
                    continue;
                }


                send_bytes = 0;
                while (send_bytes < body_len){
                    ssize_t n = send(connfd, body + send_bytes, body_len - send_bytes, 0);
                    if (n == -1 && errno == EINTR) {
                        continue; // EINTR等临时错误，继续重试
                    }
                    if (n < 0) {
                        perror("send rpc body");
                        is_error = true;
                        break;
                    }
                    if (n == 0) {
                        printf("send rpc body: connection closed\n");
                        is_error = true;
                        break;
                    }
                    send_bytes += n;
                }
                if (is_error){
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    free(body);
                    continue;
                }

                free(body);
            }
        }
    }

    close(sockfd);
    close(epollfd);
    return 0;
}
