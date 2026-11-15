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
/*#include<fcntl.h>*/
#include<signal.h>

#define MAX_MSG_LEN 4096
#define HEADER_LEN 4
#define BACKLOG 10
const char *PORT = "3000";

volatile sig_atomic_t stop_flag = 0;

void handle_signal(int sig){
    if(sig == SIGINT || sig == SIGTERM){
        stop_flag = 1;
    }
    return;
}

int read_message(int fd, char *buf, size_t size){
    ssize_t num_read = 0;
    errno = 0;
    while(size > 0){
        num_read = read(fd, buf, size);
        if(num_read < 0 && errno != EINTR /* && errno != EAGAIN && errno != EWOULDBLOCK*/){
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
        if(num_write < 0 && errno != EINTR /*&& errno != EAGAIN && errno != EWOULDBLOCK*/){
            return -1;
        }
        size = size - num_write;
        buf = buf + num_write;
    }
    return 0;
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
        /*
        int flags = fcntl(accept_fd, F_GETFL);
        flags = flags | O_NONBLOCK;
        if(fcntl(accept_fd, F_SETFL, flags) == -1){
            perror("fcntl");
            close(accept_fd);
            continue;
        }
        */
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
