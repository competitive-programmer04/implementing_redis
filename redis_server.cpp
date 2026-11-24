#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netdb.h>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<errno.h>
#include<fcntl.h>
#include<signal.h>
#include<sys/epoll.h>

#define MAX_MSG_LEN 4096
#define HEADER_LEN 4
#define BACKLOG 10
#define MAX_EVENTS 1024
const char *PORT = "3000";

typedef struct Connection{
    char *read_buf;
    char *write_buf;
    size_t read_len;
    size_t write_len;
    ssize_t read_offset;
    ssize_t write_offset;
    int fd;
}Connection;
struct epoll_event events[MAX_EVENTS];
int epfd = 0;

volatile sig_atomic_t stop_flag = 0;

void handle_signal(int sig){
    if(sig == SIGINT || sig == SIGTERM){
        stop_flag = 1;
    }
    return;
}

void set_non_block(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1){
        perror("fcntl:F_GETFL");
        exit(1);
    }
    flags = flags | O_NONBLOCK;
    if(fcntl(fd, F_SETFL, flags) == -1){
        perror("fcntl:F_SETFL");
        exit(1);
    }
    return;
}

void create_new_conn(int fd){
    Connection *conn = (Connection*)malloc(sizeof(Connection));
    conn->fd = fd;
    conn->read_buf = NULL;
    conn->write_buf = NULL;
    conn->read_len = 0;
    conn->write_len = 0;
    conn->read_offset = 0;
    conn->write_offset = 0;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, conn->fd, &ev) == -1){
        perror("epoll_ctl");
        close(conn->fd);
        free(conn);
    }
    return;
}

int parse_request(Connection *conn){
    ssize_t start = 0;
    ssize_t end = conn->read_offset;
    while(1){
        if(end - start >= HEADER_LEN && conn->read_len == 0){
            int len = 0;
            memcpy(&len, conn->read_buf, HEADER_LEN);
            if(len > MAX_MSG_LEN){
                printf("Number of bytes received from client: %d. Number of bytes allowed: %d. Client send too many bytes\n", len, MAX_MSG_LEN);
                close(conn->fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
                free(conn->read_buf);
                free(conn->write_buf);
                free(conn);
                return -1;
            }
            conn->read_len = len;
        }
        if(conn->read_len > 0 && end - start >= conn->read_len + HEADER_LEN){
            size_t total_len = HEADER_LEN + conn->read_len;
            if(conn->write_buf == NULL){
                conn->write_buf = (char*)malloc(total_len);
                memcpy(conn->write_buf, conn->read_buf + start, HEADER_LEN);
                memcpy(conn->write_buf + HEADER_LEN, conn->read_buf + start + HEADER_LEN, conn->read_len);
            }
            else{
                conn->write_buf = (char*)realloc(conn->write_buf, conn->write_len + total_len);
                memcpy(conn->write_buf + conn->write_len, conn->read_buf + start, HEADER_LEN);
                memcpy(conn->write_buf + conn->write_len + HEADER_LEN, conn->read_buf + start + HEADER_LEN, conn->read_len);
            }
            conn->write_len = conn->write_len + total_len;
            start = start + conn->read_len + HEADER_LEN;
            conn->read_len = 0;
        }
        else{
            break;
        }
    }
    if(conn->write_len > 0){
        if(end - start == 0){
            free(conn->read_buf);
            conn->read_buf = NULL;
            conn->read_len = 0;
            conn->read_offset = 0;
        }
        else{
            int extra_bytes = end - start;
            char *extra_buf = (char*)malloc(extra_bytes);
            memcpy(extra_buf, conn->read_buf + start, extra_bytes);
            printf("freeing old data \n");
            free(conn->read_buf);
            conn->read_buf = extra_buf;
            conn->read_offset = extra_bytes;
        }
    }
    return 0;
}

void handle_read(Connection *conn){
    char my_buffer[4096];
    while(1){
        ssize_t bytes_read = read(conn->fd, my_buffer, sizeof(my_buffer));
        if(bytes_read <= 0 && errno != EINTR){
            if(bytes_read == 0){
                printf("connection closed by the client\n");
            }
            else{
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    printf("recive buffer is empty. We read all the client's request\n");
                    return; // receive buffer is empty. We read all the requests data
                }
                perror("read");
            }
            close(conn->fd);
            epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
            free(conn);
            return;
        }
        if(conn->read_buf == NULL){
            conn->read_buf = (char*)malloc(bytes_read);
            memcpy(conn->read_buf, my_buffer, bytes_read);
            conn->read_offset = bytes_read;
        }
        else{
            conn->read_buf = (char*)realloc(conn->read_buf, conn->read_offset + bytes_read);
            memcpy(conn->read_buf + conn->read_offset, my_buffer, bytes_read);
            conn->read_offset = conn->read_offset + bytes_read;
        }
        int status_code = parse_request(conn);
        if(status_code == -1) return;
        if(conn->write_len > 0){
            ssize_t bytes_write = write(conn->fd, conn->write_buf + conn->write_offset, conn->write_len);
            if(bytes_write == conn->write_len){
                free(conn->write_buf);
                conn->write_buf = 0;
                conn->write_len = 0;
                conn->write_offset = 0;
            }
            else{
                if(bytes_write < 0){
                    if(errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK){
                        perror("write");
                        close(conn->fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
                        free(conn->read_buf);
                        free(conn->write_buf);
                        return;
                    }
                }
                else if(bytes_write > 0){
                    conn->write_offset = conn->write_offset + bytes_write;
                }
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLOUT;
                ev.data.ptr = conn;
                epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev);
            }
        }
    }
    return;
}

void handle_write(Connection *conn){
    while(conn->write_len > conn->write_offset){
        ssize_t bytes_write = write(conn->fd, conn->write_buf + conn->write_offset, conn->write_len - conn->write_offset);
        if(bytes_write < 0){
            if(errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK){
                perror("write");
                close(conn->fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
                free(conn->read_buf);
                free(conn->write_buf);
                free(conn);
            }
            return;
        }
        conn->write_offset = conn->write_offset + bytes_write;
    }
    printf("data written successfully\n");
    free(conn->write_buf);
    conn->write_buf = NULL;
    conn->write_offset = 0;
    conn->write_len = 0;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev);
    return;
}

int main(void){
    struct sigaction sa;
    int status = 0;
    struct addrinfo a_info;
    struct addrinfo *head;
    memset(&a_info, 0, sizeof(a_info));
    a_info.ai_family = AF_INET; // ipv4 address family
    a_info.ai_socktype = SOCK_STREAM; // tcp socket
    a_info.ai_flags = AI_PASSIVE; // for localhost

    if((status = getaddrinfo(NULL, PORT, &a_info, &head)) != 0){
        fprintf(stderr, "Some error occurred: %s\n", gai_strerror(status));
        return EXIT_FAILURE;
    }

    int lis_fd = socket(head->ai_family, head->ai_socktype, head->ai_protocol);
    if(lis_fd == -1){
        perror("listener socket");
        return EXIT_FAILURE;
    }
    int yes = 1;
    if(setsockopt(lis_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1){
        perror("setsockopt");
        close(lis_fd);
        return EXIT_FAILURE;
    }
    if(bind(lis_fd, (struct sockaddr*)head->ai_addr, head->ai_addrlen) == -1){
        perror("bind");
        close(lis_fd);
        return EXIT_FAILURE;
    }
    if(listen(lis_fd, BACKLOG) == -1){
        perror("listen");
        close(lis_fd);
        return EXIT_FAILURE;
    }
    fprintf(stdout, "server started at port %s\n", PORT);
    set_non_block(lis_fd);
    /* Message Structure
     first 4 bytes will indicate the payload length
     then the next bytes will contain the payload
     */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    // Register handle for both Ctrl-C and termination request
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // creating epoll instance
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if(epfd == -1){
        perror("epoll");
        return EXIT_FAILURE;
    }
    // adding the fd to the red-black tree of epoll instance
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = lis_fd;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, lis_fd, &ev) == -1){
        perror("epoll_ctl");
        close(epfd);
        return EXIT_FAILURE;
    }
    while(!stop_flag){
        int num_ready_fd = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if(num_ready_fd == -1){
            if(errno != EINTR){
                perror("epoll_wait");
                close(lis_fd);
                close(epfd);
                return EXIT_FAILURE;
            }
        }
        else if(num_ready_fd > 0){
            for(int i=0; i<num_ready_fd; ++i){
                if(events[i].data.fd == lis_fd){
                    struct sockaddr_storage ss;
                    socklen_t size = sizeof(ss);
                    int client_fd = accept(lis_fd, (struct sockaddr*)&ss, &size);
                    if(client_fd == -1){
                        if(errno != EINTR){
                            perror("accept");
                        }
                        continue;
                    }
                    char my_buf[256];
                    if(ss.ss_family == AF_INET){
                        struct sockaddr_in *my_addr = (struct sockaddr_in*)&ss;
                        inet_ntop(my_addr->sin_family, &my_addr->sin_addr.s_addr, my_buf, sizeof(my_buf));
                        printf("The ip address of the client is %s and the port number is %d\n", my_buf, my_addr->sin_port);
                    }
                    else if(ss.ss_family == AF_INET6){
                        struct sockaddr_in6 *my6_addr = (struct sockaddr_in6*)&ss;
                        inet_ntop(my6_addr->sin6_family, &my6_addr->sin6_addr.s6_addr, my_buf, sizeof(my_buf));
                        printf("The ip address of the client is %s and the port number is %d\n", my_buf, my6_addr->sin6_port);
                    }
                    set_non_block(client_fd);
                    printf("making %d non-blocking\n", client_fd);
                    create_new_conn(client_fd);
                }
                else{
                    Connection *conn = (Connection *)events[i].data.ptr;
                    if(events[i].events & (EPOLLERR|EPOLLHUP)){
                        close(conn->fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
                        free(conn);
                    }
                    else if(events[i].events & EPOLLIN){
                        printf("calling handle_read\n");
                        handle_read(conn);
                    }
                    else if(events[i].events & EPOLLOUT){
                        printf("calling handle_write\n");
                        handle_write(conn);           
                    }
                }
            }
        }
    }
    printf("User press Ctrl-C to end the server\n");
    close(epfd);
    close(lis_fd);
    return EXIT_SUCCESS;
}
