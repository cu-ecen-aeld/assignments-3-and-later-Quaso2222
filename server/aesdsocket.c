#define _POSIX_C_SOURCE 200809L // 为了 sigaction

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024 // 用于读取文件和接收数据的初始/临时缓冲区大小

int server_socket_fd = -1;
bool signal_recv = 0;

void signal_handler(int sig)
{
    if ((sig == SIGINT) || (sig == SIGTERM))
    {
        syslog(LOG_INFO, "Caught signal, exiting");
        signal_recv = 1;
    }
}

int main(void)
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client_socket_fd = -1;
    char client_ip_str[INET_ADDRSTRLEN];
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;

    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd == -1)
    {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    // Opens a stream socket bound to port 9000, failing and returning -1 if any of the socket connection steps fail.
    if (bind(server_socket_fd, &server_addr, sizeof(server_addr)) == -1)
    {
        syslog(LOG_ERR, "socket bind faild: %s", strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        closelog();
        return -1;
    }
    //  Listens for and accepts a connection

    if (listen(server_socket_fd, 10) == -1)
    {
        syslog(LOG_ERR, "socket listen failed : %s", strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        closelog();
        return -1;
    }
    syslog(LOG_INFO, "socket listen on port : %d", PORT);
    // Listens for and accepts a connection
    while (!signal_recv)
    {
        client_socket_fd = accept(client_socket_fd, &client_addr, sizeof(client_addr));
        if (client_socket_fd != -1)
        {
            inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip_str, INET_ADDRSTRLEN);
            syslog(LOG_INFO, "Accepted connection from %s", client_ip_str);
        }
        else
            continue;

        // Receives data over the connection and appends to file /var/tmp/aesdsocketdata, creating this file if it doesn’t exist.

        FILE *file = fopen("/var/tmp/aesdsocketdata", "a+");
        if (file == NULL)
        {
            syslog(LOG_ERR, "fail to open file : %s", strerror(errno));
            close(client_socket_fd);
            syslog(LOG_INFO, "close connection from : %s", client_ip_str);
            continue;
        }

        char *buffer_pointer = NULL;
        int packet_buffer_cap = 0;
        int packet_buffer_len = 0;
        char recv_temp_buf[BUFFER_SIZE];
        int bytes_received;

        int connection_active = 1;
        while (connection_active && !signal_recv)
        {
            bytes_received = recv(client_socket_fd, recv_temp_buf, sizeof(recv_temp_buf), 0);
            if (bytes_received < 0)
            {
                if (errno == EINTR && signal_recv)
                    break;
                if (errno == EINTR)
                    continue;
                syslog(LOG_ERR, "error recving from %s : %s", client_ip_str, strerror(errno));
                connection_active = 0;
                break;
            }
            if (bytes_received == 0)
            {
                // 客户端关闭连接
                connection_active = 0;
                break;
            }

            // allocate buffer capacity
            if (packet_buffer_len + bytes_received + 1 > packet_buffer_cap + 1 > packet_buffer_cap)
            {
                int new_cap = (packet_buffer_cap == 0) ? (packet_buffer_cap = 1) : (2 * packet_buffer_cap);
                if (new_cap < packet_buffer_len + bytes_received + 1)
                {
                    new_cap = packet_buffer_len + bytes_received + 1;
                }
                char *new_buf = realloc(buffer_pointer, new_cap);
                if (new_buf = NULL)
                {
                    syslog(LOG_ERR, "fail to allocate buffer for %s packet", client_ip_str);
                    buffer_pointer = NULL;
                    packet_buffer_cap = 0;
                    packet_buffer_len = 0;
                    connection_active = 0;
                    break;
                }
                buffer_pointer = new_buf;
                packet_buffer_cap = new_cap;

                memcpy(buffer_pointer + packet_buffer_len, recv_temp_buf, bytes_received);
                packet_buffer_len += bytes_received;
                buffer_pointer[packet_buffer_len] = '\0';

                if (fwrite(buffer_pointer, 1, packet_buffer_len, file) != packet_buffer_len)
                {
                    syslog(LOG_ERR, "Error writing  %s", strerror(errno));
                    connection_active = 0;
                    break;
                }

                fflush(file);
                rewind(file);
                        }
        }
    }
}