#define _POSIX_C_SOURCE 200809L

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
    // syslog(LOG_INFO, "enter signal_handler");
    if ((sig == SIGINT) || (sig == SIGTERM))
    {
        syslog(LOG_INFO, "Caught signal, exiting");
        signal_recv = 1;
        if (server_socket_fd != -1)
        {
            shutdown(server_socket_fd, SHUT_RDWR); // 尝试优雅关闭
            close(server_socket_fd);
            server_socket_fd = -1;
        }
        // 删除数据文件
        remove(DATA_FILE);
    }
}

void send_file_to_client(int client_fd, const char *file_path)
{
    FILE *file = fopen(file_path, "r");
    if (file == NULL)
    {
        syslog(LOG_ERR, "can not open file %s : %s", file_path, strerror(errno));
        return;
    }
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        ssize_t bytes_sent_total = 0;
        while (bytes_sent_total < bytes_read)
        {
            ssize_t bytes_sent = send(client_fd, buffer + bytes_sent_total, bytes_read - bytes_sent_total, 0);
            if (bytes_sent == -1)
            {
                if (errno == EINTR)
                    continue;
                syslog(LOG_ERR, "Error sending data to client: %s", strerror(errno));
                fclose(file);
                return;
            }
            syslog(LOG_INFO, "sent %zu bytes to client", bytes_sent);
            syslog(LOG_INFO, "buffer content %s", buffer);
            bytes_sent_total += bytes_sent;
        }
    }
    fclose(file);
}

void daemonize()
{
    pid_t pid;
    pid = fork();
    if (pid < 0)
    {
        syslog(LOG_ERR, "Fork failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (pid > 0)
    {
        // Parent process
        exit(EXIT_SUCCESS);
    }
}
int main(int argc, char *argv[])
{

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client_socket_fd = -1;
    char client_ip_str[INET_ADDRSTRLEN];
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask); // 在处理器执行期间不阻塞其他信号
    sa.sa_flags = 0;          // 不要设置 SA_RESTART，以便 accept 等系统调用被中断
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1)
    {
        syslog(LOG_ERR, "Error setting up signal handlers: %s", strerror(errno));
        closelog();
        return -1; // b. 如果套接字连接步骤失败，则失败并返回 -1
    }

    int daemonize_mode = 0;
    if (argc > 1)
    {
        if (strcmp(argv[1], "-d") == 0)
        {
            daemonize_mode = 1;
            syslog(LOG_INFO, "Daemon mode requested.");
        }
    }

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
    if (bind(server_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        syslog(LOG_ERR, "socket bind faild: %s", strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        closelog();
        return -1;
    }
    //  Listens for and accepts a connection
    syslog(LOG_INFO, "bind secccessfully : %d", PORT);
    if (listen(server_socket_fd, 10) == -1)
    {
        syslog(LOG_ERR, "socket listen failed : %s", strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        closelog();
        return -1;
    }
    syslog(LOG_INFO, "socket listen on port : %d", PORT);

    if (daemonize_mode)
    {
        daemonize();
    }
    // Listens for and accepts a connection
    while (!signal_recv)
    {
        syslog(LOG_INFO, "begin listen : %d", PORT);
        client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&client_addr, &client_addr_len);
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
        else
        {
            syslog(LOG_INFO, "open file : %s", DATA_FILE);
        }

        char *buffer_pointer = NULL;
        int packet_buffer_cap = 0;
        int packet_buffer_len = 0;
        char recv_temp_buf[BUFFER_SIZE];
        int bytes_received;

        int connection_active = 1;
        while (connection_active && !signal_recv)
        {
            syslog(LOG_INFO, "Begin receive from : %s", DATA_FILE);
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
            else
            {
                syslog(LOG_INFO, "Received %d bytes from %s", bytes_received, client_ip_str);
            }
            if (bytes_received == 0)
            {
                // 客户端关闭连接
                connection_active = 0;
                break;
            }

            // allocate buffer capacity
            if (packet_buffer_len + bytes_received + 1 > packet_buffer_cap)
            {
                int new_cap = (packet_buffer_cap == 0) ? (packet_buffer_cap = 1) : (2 * packet_buffer_cap);
                if (new_cap < packet_buffer_len + bytes_received + 1)
                {
                    new_cap = packet_buffer_len + bytes_received + 1;
                }
                char *new_buf = realloc(buffer_pointer, new_cap);
                if (new_buf == NULL)
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
                syslog(LOG_INFO, "Buffer allocation complete : %d bytes", packet_buffer_cap);
            }

            memcpy(buffer_pointer + packet_buffer_len, recv_temp_buf, bytes_received);
            packet_buffer_len += bytes_received;
            buffer_pointer[packet_buffer_len] = '\0';

            char *newline_char;
            char *current_search_start = buffer_pointer;
            while ((newline_char = strchr(current_search_start, '\n')) != NULL)
            {
                size_t packet_len_inc_newline = (newline_char - current_search_start) + 1;
                if (fwrite(current_search_start, 1, packet_len_inc_newline, file) != packet_len_inc_newline)
                {
                    syslog(LOG_ERR, "Error writing to %s: %s", DATA_FILE, strerror(errno));
                    connection_active = 0;
                    break;
                }
                else
                {
                    syslog(LOG_INFO, "write %zu bytes to %s", packet_len_inc_newline, DATA_FILE);
                }
                fflush(file);
                rewind(file);
                //
                send_file_to_client(client_socket_fd, DATA_FILE);
                // send_file_to_client(client_socket_fd, "/home/linux/embeded/assignments-3-and-later-Quaso2222/server/testcontent");
                syslog(LOG_INFO, "after send function");
                fseek(file, 0, SEEK_END);

                current_search_start = newline_char + 1;
            }
            if (!connection_active)
                break;
            syslog(LOG_INFO, "buffer pointer reset");
            if (current_search_start > buffer_pointer && current_search_start <= buffer_pointer + packet_buffer_len)
            {
                size_t remaining_len = packet_buffer_len - (current_search_start - buffer_pointer);
                memmove(buffer_pointer, current_search_start, remaining_len);
                packet_buffer_len = remaining_len;
                buffer_pointer[packet_buffer_len] = '\0';
            }
            else if (current_search_start >= buffer_pointer + packet_buffer_len)
            {

                packet_buffer_len = 0;
                if (buffer_pointer)
                    buffer_pointer[0] = '\0';
            }
        }
        free(buffer_pointer);
        buffer_pointer = NULL;
        fclose(file);

        syslog(LOG_INFO, "Closed connection from %s", client_ip_str);
        close(client_socket_fd);
        client_socket_fd = -1;

        if (signal_recv)
            break;
    }
    syslog(LOG_INFO, "Server shutting down.");

    if (client_socket_fd != -1)
    {
        close(client_socket_fd);
    }

    if (remove(DATA_FILE) != 0 && errno != ENOENT)
    {
        syslog(LOG_WARNING, "Could not remove %s on final shutdown: %s", DATA_FILE, strerror(errno));
    }

    closelog();
    return 0;
}