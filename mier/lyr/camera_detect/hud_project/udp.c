#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include "udp.h"

int udp_init(int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    return sock;
}

int udp_receive(int sock, char *buffer, int buf_size, int timeout_ms) {
    struct pollfd fds = {.fd = sock, .events = POLLIN};
    int ret = poll(&fds, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) {
            return -2; // 信号中断，视为超时
        }
        perror("poll");
        return -1;
    }
    if (ret == 0) {
        return -2; // 超时
    }
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    int n = recvfrom(sock, buffer, buf_size - 1, 0, (struct sockaddr *)&src_addr, &addr_len);
    if (n < 0) {
        if (errno == EINTR) {
            return -2;
        }
        perror("recvfrom");
        return -1;
    }
    buffer[n] = '\0';
    return n;
}

void udp_close(int sock) {
    if (sock >= 0)
        close(sock);
}
