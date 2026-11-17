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
char my_buf[MAX_MSG_LEN];

typedef struct Connection{
    int fd;
    char *read_buf;
    ssize_t read_len;
    char *write_buf;
    ssize_t write_len;
}Connection;

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
        perror("fcntl");
        exit(1);
    }
    flags = flags | O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
    return;
}

/*
int read_message(int fd, char *buf, size_t size){
    ssize_t num_read = 0;
    errno = 0;
    while(size > 0){
        num_read = read(fd, buf, size);
        if(num_read < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK){
            return -1;
        }
        size = size - num_read;
        buf = buf + num_read;
    }
    return 0;
}

int write_message(int fd, const char *buf, size_t size){
    ssize_t num_write = 0;
while(size > 0){
        num_write = write(fd, buf, size);
        if(num_write < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK){
            return -1;
        }
        size = size - num_write;
        buf = buf + num_write;
    }
    return 0;
}
*/

int main(void){
    memset(my_buf, 0, MAX_MSG_LEN);
    my_buf[MAX_MSG_LEN-1] = '\0';
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
    int epfd = epoll_create1(EPOLL_CLOEXEC);
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
        return EXIT_FAILURE;
    }
    struct epoll_event events[MAX_EVENTS];
    while(!stop_flag){
        int num_ready_fd = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if(num_ready_fd == -1){
            if(errno != EINTR){
                perror("epoll_wait");
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
                    if(ss.ss_family == AF_INET){
                        struct sockaddr_in *my_addr = (struct sockaddr_in*)&ss;
                        inet_ntop(my_addr->sin_family, &my_addr->sin_addr.s_addr, my_buf, sizeof(my_buf)-1);
                        printf("The ip address of the client is %s and the port number is %d\n", my_buf, my_addr->sin_port);
                    }
                    else if(ss.ss_family == AF_INET6){
                        struct sockaddr_in6 *my6_addr = (struct sockaddr_in6*)&ss;
                        inet_ntop(my6_addr->sin6_family, &my6_addr->sin6_addr.s6_addr, my_buf, sizeof(my_buf)-1);
                        printf("The ip address of the client is %s and the port number is %d\n", my_buf, my6_addr->sin6_port);
                    }
                    set_non_block(client_fd);
                    Connection *conn = (Connection*)malloc(sizeof(Connection));
                    conn->fd = client_fd;
                    conn->read_buf = NULL;
                    conn->write_buf = NULL;
                    conn->read_len = 0;
                    conn->write_len = 0;
                    struct epoll_event ev;
                    ev.events = EPOLLIN;
                    ev.data.ptr = conn;
                    if(epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1){
                        perror("epoll_ctl");
                        close(client_fd);
                    }
                }
                else{
                    Connection *conn = (Connection *)events[i].data.ptr;
                    if(events[i].events & (EPOLLERR|EPOLLHUP)){
                        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
                    }
                    else if(events[i].events & EPOLLIN){
                        ssize_t bytes_read = 0;
                        int fd = conn->fd;
                        if(conn->read_buf == NULL){
                            bytes_read = read(fd, my_buf, HEADER_LEN);
                            int len = 0;
                            memcpy(&len, my_buf, HEADER_LEN);
                            if(len <= MAX_MSG_LEN){
                                bytes_read = read(fd, my_buf, MAX_MSG_LEN);
                                if(bytes_read != MAX_MSG_LEN){
                                    conn->read_buf = malloc(len);
                                    memcpy(conn->read_buf, my_buf, bytes_read);
                                    conn->read_len = bytes_read;
                                }
                                else{
                                    events[i].events = EPOLLIN | EPOLLOUT;
                                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &events[i]);
                                }
                            }
                        }
                        else{
                            bytes_read = read(fd, conn->read_buf + conn->read_len, sizeof(conn->read_buf)-conn->read_len);
                            conn->read_len = conn->read_len + bytes_read;
                            if(conn->read_len == sizeof(conn->read_buf)){
                               events[i].events = EPOLLIN | EPOLLOUT;
                               epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &events[i]);
                               free(conn->read_buf);
                               conn->read_buf = NULL;
                               conn->read_len = 0;
                            }
                        }
                    }
                    else if(events[i].events & EPOLLOUT){
                        ssize_t bytes_write = 0;
                        int fd = conn->fd;
                        if(conn->write_buf == NULL){
                            const char *msg = "Hello World from server";
                            int len = strlen(msg);
                            memcpy(my_buf, &len, sizeof(int));
                            memcpy(my_buf + HEADER_LEN, msg, len);
                            bytes_write = write(fd, my_buf, HEADER_LEN + len);
                            if(bytes_write != HEADER_LEN + len){
                                conn->write_buf = malloc(HEADER_LEN + len);
                                memcpy(conn->write_buf, my_buf, HEADER_LEN + len);
                                conn->write_len = bytes_write;
                            }
                            else{
                                events[i].events = EPOLLIN;
                                epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &events[i]);
                            }
                        }
                        else{
                            bytes_write = write(fd, conn->write_buf + conn->write_len, sizeof(conn->write_buf)-conn->write_len);
                            conn->write_len = conn->write_len + bytes_write;
                            if(conn->write_len == sizeof(conn->write_buf)){
                                events[i].events = EPOLLIN;
                                epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &events[i]);
                                free(conn->write_buf);
                                conn->write_buf = NULL;
                                conn->write_len = 0;
                            }
                        }
                    }
                }
            }
        }
    }
    close(epfd);
    close(lis_fd);
    return EXIT_SUCCESS;
}
    /*
    while(!stop_flag){
        struct sockaddr_storage ss;
        socklen_t size = sizeof(ss);
        int accept_fd = accept(lis_fd, (struct sockaddr*)&ss, &size);
        if(accept_fd == -1){
            if(errno != EINTR){
                perror("accept");
            }
            continue;
        }
        char buf[256];
        memset(buf, 0, sizeof(buf));
        if(ss.ss_family == AF_INET){
            struct sockaddr_in *my_addr = (struct sockaddr_in*)&ss;
            inet_ntop(my_addr->sin_family, &my_addr->sin_addr.s_addr, buf, sizeof(buf)-1);
            buf[255] = '\0';
            fprintf(stdout, "The ip address of clinet is %s and port is %d\n", buf, my_addr->sin_port);
        }
        else if(ss.ss_family == AF_INET6){
            struct sockaddr_in6 *my_addr6 = (struct sockaddr_in6*)&ss;
            inet_ntop(my_addr6->sin6_family, &my_addr6->sin6_addr.s6_addr, buf, sizeof(buf)-1);
            buf[255] = '\0';
            fprintf(stdout, "The ip address of client is %s and port number is %d\n", buf, my_addr6->sin6_port);
        }

        char read_buf[HEADER_LEN + MAX_MSG_LEN];
        char write_buf[HEADER_LEN + MAX_MSG_LEN];
        memset(read_buf, 0, sizeof(read_buf));
        memset(write_buf, 0, sizeof(write_buf));
        int err_code = read_message(accept_fd, read_buf, HEADER_LEN);
        if(err_code == -1){
            if(errno == 0){
                fprintf(stdout, "End of Connection\n");
            }
            else{
                perror("read");
            }
            close(accept_fd);
            return EXIT_FAILURE;
        }
        int msg_len = 0;
        memcpy(&msg_len, read_buf, HEADER_LEN);
        if(msg_len > MAX_MSG_LEN){
            fprintf(stderr, "Message rceived from the client is too long\n");
            close(accept_fd);
        }
        else{
            err_code = 0;
            err_code = read_message(accept_fd, read_buf + HEADER_LEN, msg_len);
            if(err_code == -1){
                perror("read");
                close(accept_fd);
            }
            else{
                fprintf(stdout, "data send by the client: %.*s\n", msg_len, read_buf + HEADER_LEN);
            }
        }
        const char *msg = "Hello World from server";
        msg_len = strlen(msg);
        memcpy(write_buf, &msg_len, sizeof(int));
        memcpy(write_buf + HEADER_LEN, msg, msg_len);
        err_code = 0;
        err_code = write_message(accept_fd, write_buf, sizeof(write_buf));
        if(err_code == -1){
            perror("write");
            close(accept_fd);
        }
        //graceful shutdown
        memset(read_buf, 0, sizeof(read_buf));
        shutdown(accept_fd, SHUT_WR);
        while(read(accept_fd, read_buf, sizeof(read_buf)-1) != 0);
        close(accept_fd);
    }
    printf("user pressed Ctrl-C\n");
    close(lis_fd);
    return EXIT_SUCCESS;
}
*/
