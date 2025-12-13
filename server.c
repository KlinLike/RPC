#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>


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
                char buf[1024];
                int n = recv(connfd, buf, sizeof(buf), 0);
                if (n < 0)
                {
                    perror("recv");
                    return -1;
                }
                if (n == 0)
                {
                    // 对端关闭连接
                    close(connfd);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL);
                    continue;
                }
                // 处理收到的数据
                printf("recv %d bytes: %s\n", n, buf);
                // echo
                n = send(connfd, buf, n, 0);
                if (n < 0)
                {
                    perror("send");
                    return -1;
                }
            }
        }
    }

    
}
