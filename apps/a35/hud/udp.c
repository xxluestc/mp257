#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include "udp.h"

static int udp_init_address(int port, uint32_t address) {
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(address);
    addr.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    return sock;
}

int udp_init(int port) {
    return udp_init_address(port, INADDR_ANY);
}

int udp_init_local(int port) {
    // IMU 转发是本机进程接口，只绑定 loopback，不接受外部主机直接投递事件。
    return udp_init_address(port, INADDR_LOOPBACK);
}

int udp_receive(int sock, char *buffer, int buf_size, int timeout_ms) {
    if (!buffer || buf_size < 2 || timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }
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
    if (fds.revents & (POLLERR | POLLHUP | POLLNVAL))
        return -1;
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    int n =
        recvfrom(sock, buffer, buf_size - 1, MSG_TRUNC, (struct sockaddr *)&src_addr, &addr_len);
    if (n < 0) {
        if (errno == EINTR) {
            return -2;
        }
        perror("recvfrom");
        return -1;
    }
    if (n >= buf_size)
        return -2; // MSG_TRUNC 返回原始报文长度，超长报文整体丢弃，避免解析半份 JSON。
    buffer[n] = '\0';
    return n;
}

void udp_close(int sock) {
    if (sock >= 0)
        close(sock);
}
