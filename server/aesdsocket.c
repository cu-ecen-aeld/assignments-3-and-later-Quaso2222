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
#include <pthread.h>
#include <time.h>

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

struct message_file
{
    FILE *file;
    pthread_mutex_t mutex;
};

int init_message_file(struct message_file *msg_file, char *file_name)
{
    msg_file->file = fopen(file_name, "a+");
    if (msg_file->file == NULL)
    {
        syslog(LOG_ERR, "Failed to open file %s: %s", DATA_FILE, strerror(errno));
        return -1;
    }
    if (pthread_mutex_init(&msg_file->mutex, NULL) != 0)
    {
        syslog(LOG_ERR, "Failed to initialize mutex: %s", strerror(errno));
        fclose(msg_file->file);
        return -1;
    }
    return 0;
}

int cleanup_message_file(struct message_file *msg_file)
{
    if (msg_file->file != NULL)
    {
        fclose(msg_file->file);
        msg_file->file = NULL;
    }
    pthread_mutex_destroy(&msg_file->mutex);
    return 0;
}

void send_file_to_client(int client_fd, FILE *file)
{

    if (file == NULL)
    {
        syslog(LOG_ERR, "can not open file : %s", strerror(errno));
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
                // fclose(file);
                return;
            }
            syslog(LOG_INFO, "sent %zu bytes to client", bytes_sent);
            syslog(LOG_INFO, "buffer content %s", buffer);
            bytes_sent_total += bytes_sent;
        }
    }
    // fclose(file);
}

struct ThreadArgs
{
    int sid;
    struct sockaddr_in *client_addr;
    struct message_file *msg_file;
};

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

void *client_connection(void *args)
{
    struct ThreadArgs *thread_args = (struct ThreadArgs *)args;
    int sid = thread_args->sid;
    struct sockaddr_in *client_addr = thread_args->client_addr;
    struct message_file *msg_file = thread_args->msg_file;
    int client_socket_fd = sid;
    char client_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip_str, INET_ADDRSTRLEN);
    syslog(LOG_INFO, "Accepted connection from %s", client_ip_str);

    // 处理客户端连接

    // handle client connection begin

    // Receives data over the connection and appends to file /var/tmp/aesdsocketdata, creating this file if it doesn’t exist.

    // FILE *file = fopen("/var/tmp/aesdsocketdata", "a+");
    // if (file == NULL)
    // {
    //     syslog(LOG_ERR, "fail to open file : %s", strerror(errno));
    //     close(client_socket_fd);
    //     syslog(LOG_INFO, "close connection from : %s", client_ip_str);
    //     continue;
    // }
    // else
    // {
    //     syslog(LOG_INFO, "open file : %s", DATA_FILE);
    // }

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
        syslog(LOG_INFO, "test here1");
        char *newline_char;
        char *current_search_start = buffer_pointer;
        while ((newline_char = strchr(current_search_start, '\n')) != NULL)
        {
            syslog(LOG_INFO, "test here2");
            size_t packet_len_inc_newline = (newline_char - current_search_start) + 1;
            syslog(LOG_INFO, "before _mutex_lock");
            pthread_mutex_lock(&msg_file->mutex);
            syslog(LOG_INFO, "CURRENT FILE POINTER : %p", msg_file->file);
            syslog(LOG_INFO, "after _mutex_lock");

            if (fwrite(current_search_start, 1, packet_len_inc_newline, msg_file->file) != packet_len_inc_newline)
            {
                syslog(LOG_ERR, "Error writing to %s: %s", DATA_FILE, strerror(errno));
                connection_active = 0;
                pthread_mutex_unlock(&msg_file->mutex);
                break;
            }
            else
            {
                syslog(LOG_INFO, "write %zu bytes to %s", packet_len_inc_newline, DATA_FILE);
            }

            fflush(msg_file->file);
            rewind(msg_file->file);
            //
            send_file_to_client(client_socket_fd, msg_file->file);
            // send_file_to_client(client_socket_fd, "/home/linux/embeded/assignments-3-and-later-Quaso2222/server/testcontent");
            syslog(LOG_INFO, "after send function");
            fseek(msg_file->file, 0, SEEK_END);
            pthread_mutex_unlock(&msg_file->mutex);
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

    syslog(LOG_INFO, "Closed connection from %s", client_ip_str);
    close(client_socket_fd);
    free(client_addr);
    syslog(LOG_INFO, "msg_file check L279 : %p", msg_file);
    syslog(LOG_INFO, "msg_file->file_check L280 : %p", msg_file->file);
    client_addr = NULL;
    client_socket_fd = -1;
    return 0;
    //  handle client connection end
};

void get_time(char *return_val, char *format)
{
    time_t t;
    struct tm *tmp;

    t = time(NULL);
    tmp = localtime(&t);
    if (tmp == NULL)
    {
        // perror("localtime");
        syslog(LOG_ERR, "get local time fail: %s", strerror(errno));
    }

    if (strftime(return_val, 200, format, tmp) == 0)
    {
        // fprintf(stderr, "strftime returned 0");
        syslog(LOG_ERR, "strftime time fail: %s", strerror(errno));
    }
    // printf("Result string is \"%s\"\n", outstr);
}

void *timer_thread(void *arg)
{
    struct timespec req, rem;
    struct message_file *file = (struct message_file *)arg;

    while (!signal_recv)
    {
        char time_str[200];

        memset(time_str, 0, sizeof(time_str));
        get_time(time_str, "%Y-%m-%d %H:%M:%S");

        syslog(LOG_INFO, "get time : %s", time_str);
        char *time_start = "timestamp:";
        char combined_timestamp[300];
        snprintf(combined_timestamp, sizeof(combined_timestamp), "%s%s\n", time_start, time_str);
        pthread_mutex_lock(&file->mutex);
        fputs(combined_timestamp, file->file);
        fflush(file->file);
        syslog(LOG_INFO, "write timestamp to file : %s", combined_timestamp);
        pthread_mutex_unlock(&file->mutex);
        req.tv_sec = 10; // 请求睡眠 10 秒
        req.tv_nsec = 0; // 0 纳秒
        nanosleep(&req, &rem);
    }
    return NULL;
}

int main(int argc, char *argv[])
{

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
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

    struct message_file *msg_file = malloc(sizeof(struct message_file));
    if (init_message_file(msg_file, DATA_FILE) != 0)
    {
        syslog(LOG_ERR, "Failed to initialize message file");
        // close(client_socket_fd);
        // syslog(LOG_INFO, "close connection from : %s", client_ip_str);
        return -1;
    }
    else
    {
        syslog(LOG_INFO, "open file : %s", DATA_FILE);
    }
    pthread_t timer_thread_id;
    pthread_create(&timer_thread_id, NULL, timer_thread, (void *)msg_file);
    while (!signal_recv)
    {
        int client_socket_fd = -1;
        client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&client_addr, &client_addr_len);

        if (client_socket_fd != -1)
        {
            inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip_str, INET_ADDRSTRLEN);
            syslog(LOG_INFO, "Accepted connection from %s", client_ip_str);
        }
        else
            continue;

        syslog(LOG_INFO, "msg_file check L381 : %p", msg_file);
        syslog(LOG_INFO, "msg_file->file_check L382 : %p", msg_file->file);
        pthread_t client_thread_id;
        struct sockaddr_in *new_client_addr = malloc(sizeof(struct sockaddr_in));
        *new_client_addr = client_addr;

        struct ThreadArgs *args_for_thread = malloc(sizeof(struct ThreadArgs));
        args_for_thread->sid = client_socket_fd;
        args_for_thread->client_addr = new_client_addr;
        args_for_thread->msg_file = msg_file;
        syslog(LOG_INFO, "thread_args_POINTER_CHECK : %p", args_for_thread->msg_file->file);

        int ret = pthread_create(&client_thread_id, NULL, client_connection, (void *)args_for_thread);
        if (ret != 0)
        {
            syslog(LOG_ERR, "Failed to create thread: %s", strerror(errno));
            close(client_socket_fd);
            free(new_client_addr);
            continue;
        }
        // init message file

        if (signal_recv)
            break;
    }
    syslog(LOG_INFO, "Server shutting down.");
    cleanup_message_file(msg_file);
    // if (client_socket_fd != -1)
    // {
    //     close(client_socket_fd);
    // }

    if (remove(DATA_FILE) != 0 && errno != ENOENT)
    {
        syslog(LOG_WARNING, "Could not remove %s on final shutdown: %s", DATA_FILE, strerror(errno));
    }

    closelog();
    return 0;
}