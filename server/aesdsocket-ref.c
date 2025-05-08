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

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024 // 用于读取文件和接收数据的初始/临时缓冲区大小

// 全局变量以便信号处理器可以访问
volatile sig_atomic_t signal_received = 0;
int server_socket_fd = -1;

// 信号处理函数
void handle_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
    {
        syslog(LOG_INFO, "Caught signal, exiting");
        signal_received = 1;

        // 关闭服务器套接字以中断 accept() 调用
        if (server_socket_fd != -1)
        {
            shutdown(server_socket_fd, SHUT_RDWR); // 尝试优雅关闭
            close(server_socket_fd);
            server_socket_fd = -1;
        }
        // 删除数据文件
        remove(DATA_FILE);
        // 注意：不在此处调用 exit()，让主循环干净地退出
    }
}

// 发送文件全部内容给客户端
void send_file_to_client(int client_fd, const char *file_path)
{
    FILE *file = fopen(file_path, "r");
    if (file == NULL)
    {
        syslog(LOG_ERR, "Could not open %s for sending: %s", file_path, strerror(errno));
        return;
    }

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        ssize_t bytes_sent_total = 0;
        while (bytes_sent_total < bytes_read)
        {
            ssize_t bytes_sent = send(client_fd, buffer + bytes_sent_total, bytes_read - bytes_sent_total, 0);
            if (bytes_sent == -1)
            {
                if (errno == EINTR)
                    continue; // 被信号中断，重试
                syslog(LOG_ERR, "Error sending data to client: %s", strerror(errno));
                fclose(file);
                return;
            }
            bytes_sent_total += bytes_sent;
        }
    }
    if (ferror(file))
    {
        syslog(LOG_ERR, "Error reading from %s during send: %s", file_path, strerror(errno));
    }
    fclose(file);
}

int main(void)
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char client_ip_str[INET_ADDRSTRLEN];
    int client_socket_fd = -1;

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    // 设置信号处理
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask); // 在处理器执行期间不阻塞其他信号
    sa.sa_flags = 0;          // 不要设置 SA_RESTART，以便 accept 等系统调用被中断

    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1)
    {
        syslog(LOG_ERR, "Error setting up signal handlers: %s", strerror(errno));
        closelog();
        return -1; // b. 如果套接字连接步骤失败，则失败并返回 -1
    }

    // b. 打开流套接字
    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd == -1)
    {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    // 允许地址重用，便于测试
    int opt = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        syslog(LOG_WARNING, "setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
        // 非致命错误，但记录下来
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // 监听所有可用接口
    server_addr.sin_port = htons(PORT);

    // 绑定到端口 9000
    if (bind(server_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        syslog(LOG_ERR, "Socket bind failed: %s", strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        closelog();
        return -1;
    }

    // c. 监听连接
    if (listen(server_socket_fd, 10) == -1)
    { // 允许最多10个挂起连接
        syslog(LOG_ERR, "Socket listen failed: %s", strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        closelog();
        return -1;
    }
    syslog(LOG_INFO, "Server listening on port %d", PORT);

    // h. 循环接受新客户端的连接，直到收到 SIGINT 或 SIGTERM
    while (!signal_received)
    {
        client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket_fd == -1)
        {
            if (errno == EINTR && signal_received)
            {
                // 被我们的信号中断，正常退出循环
                break;
            }
            if (errno == EINTR)
            { // 其他信号中断，重试
                continue;
            }
            syslog(LOG_WARNING, "Socket accept failed: %s", strerror(errno)); // 非致命，尝试下一个
            continue;
        }

        // d. 记录消息到 syslog
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_str, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip_str);

        // e. 接收数据并追加到文件 /var/tmp/aesdsocketdata
        FILE *data_file = fopen(DATA_FILE, "a+"); // 追加读写模式，不存在则创建
        if (data_file == NULL)
        {
            syslog(LOG_ERR, "Failed to open/create %s: %s", DATA_FILE, strerror(errno));
            close(client_socket_fd);
            syslog(LOG_INFO, "Closed connection from %s due to file error", client_ip_str);
            continue; // 继续接受下一个客户端
        }

        char *packet_buffer = NULL;      // 动态分配的缓冲区，用于存储一个完整的数据包
        size_t packet_buffer_cap = 0;    // packet_buffer 的容量
        size_t packet_buffer_len = 0;    // packet_buffer 中当前数据长度
        char recv_temp_buf[BUFFER_SIZE]; // 临时接收区
        ssize_t bytes_received;

        int connection_active = 1;
        while (connection_active && !signal_received)
        {
            bytes_received = recv(client_socket_fd, recv_temp_buf, sizeof(recv_temp_buf), 0);

            if (bytes_received < 0)
            {
                if (errno == EINTR && signal_received)
                    break; // 被退出信号中断
                if (errno == EINTR)
                    continue; // 其他信号，重试
                syslog(LOG_ERR, "Error receiving from %s: %s", client_ip_str, strerror(errno));
                connection_active = 0;
                break;
            }
            if (bytes_received == 0)
            {
                // 客户端关闭连接
                connection_active = 0;
                break;
            }

            // 扩展 packet_buffer 以容纳新数据
            if (packet_buffer_len + bytes_received + 1 > packet_buffer_cap)
            {
                size_t new_cap = (packet_buffer_cap == 0) ? (bytes_received + 1) : (packet_buffer_cap * 2);
                if (new_cap < packet_buffer_len + bytes_received + 1)
                {
                    new_cap = packet_buffer_len + bytes_received + 1;
                }
                char *new_buf = realloc(packet_buffer, new_cap);
                if (new_buf == NULL)
                {
                    syslog(LOG_ERR, "Failed to realloc packet_buffer for %s. Discarding data.", client_ip_str);
                    free(packet_buffer);
                    packet_buffer = NULL;
                    packet_buffer_len = 0;
                    packet_buffer_cap = 0;
                    connection_active = 0; // 内存分配失败，终止此客户端连接
                    break;
                }
                packet_buffer = new_buf;
                packet_buffer_cap = new_cap;
            }
            memcpy(packet_buffer + packet_buffer_len, recv_temp_buf, bytes_received);
            packet_buffer_len += bytes_received;
            packet_buffer[packet_buffer_len] = '\0'; // 确保以 null 结尾，便于 strchr 操作

            // 查找并处理完整的数据包 (以 '\n' 分隔)
            char *newline_char;
            char *current_search_start = packet_buffer;
            while ((newline_char = strchr(current_search_start, '\n')) != NULL)
            {
                size_t packet_len_inc_newline = (newline_char - current_search_start) + 1;

                // 将数据包写入文件
                if (fwrite(current_search_start, 1, packet_len_inc_newline, data_file) != packet_len_inc_newline)
                {
                    syslog(LOG_ERR, "Error writing to %s: %s", DATA_FILE, strerror(errno));
                    connection_active = 0; // 写入失败，终止此客户端
                    break;
                }
                fflush(data_file); // 确保写入磁盘

                // f. 收到完整数据包后，将文件的全部内容返回给客户端
                // 注意：此时 data_file 以追加模式打开，需要先将其定位到文件头进行读取
                rewind(data_file); // 等效于 fseek(data_file, 0, SEEK_SET);
                send_file_to_client(client_socket_fd, DATA_FILE);
                // 读取完毕后，将文件指针重新定位到末尾，以便后续追加
                fseek(data_file, 0, SEEK_END);

                // 移动 packet_buffer 中剩余的数据到开头
                current_search_start = newline_char + 1;
            }
            if (!connection_active)
                break; // 如果写入失败，则跳出

            // 处理 packet_buffer 中的数据移动
            if (current_search_start > packet_buffer && current_search_start <= packet_buffer + packet_buffer_len)
            {
                size_t remaining_len = packet_buffer_len - (current_search_start - packet_buffer);
                memmove(packet_buffer, current_search_start, remaining_len);
                packet_buffer_len = remaining_len;
                packet_buffer[packet_buffer_len] = '\0'; // 重新 null 结尾
            }
            else if (current_search_start >= packet_buffer + packet_buffer_len)
            {
                // 所有数据都已处理 (即 current_search_start 指向或超过了 buffer 末尾)
                packet_buffer_len = 0;
                if (packet_buffer)
                    packet_buffer[0] = '\0'; // 清空缓冲区
            }
        } // 内部接收循环结束

        free(packet_buffer);
        packet_buffer = NULL;
        fclose(data_file);

        // g. 记录消息到 syslog
        syslog(LOG_INFO, "Closed connection from %s", client_ip_str);
        close(client_socket_fd);
        client_socket_fd = -1;

        if (signal_received)
            break; // 在处理完一个客户端后检查信号
    }

    // i. 优雅退出
    syslog(LOG_INFO, "Server shutting down.");
    if (server_socket_fd != -1)
    { // 如果信号处理器没有关闭它
        close(server_socket_fd);
    }
    if (client_socket_fd != -1)
    {                            // 如果在收到信号时仍有活动客户端连接
        close(client_socket_fd); // 关闭它
    }
    // 文件删除主要由信号处理器负责，但这里可以再次尝试以防万一
    if (remove(DATA_FILE) != 0 && errno != ENOENT)
    {
        syslog(LOG_WARNING, "Could not remove %s on final shutdown: %s", DATA_FILE, strerror(errno));
    }

    closelog();
    return 0; // 正常退出
}