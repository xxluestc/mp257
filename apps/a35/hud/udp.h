#ifndef UDP_H
#define UDP_H

int udp_init(int port);
int udp_init_local(int port);
int udp_receive(int sock, char *buffer, int buf_size, int timeout_ms);
void udp_close(int sock);

#endif
