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

#define HEADER_LEN (sizeof(int))
#define MAX_MSG_LEN (1<<12)

void set_non_block(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1){
        perror("F_GETFL");
        exit(1);
    }
    flags = flags | O_NONBLOCK;
    if(fcntl(fd, F_SETFL, flags) == -1){
        perror("F_SETFL");
        exit(1);
    }
    printf("We make fd %d non-blocking\n", fd);
    return;
}

void handle_signal(int sig){
    (void)sig;
    printf("handling sigpipe: Connection closed by the server\n");
    return;
}

int read_message(int fd, char *buf, size_t size){
    ssize_t num_read = 0;
    errno = 0;
    while(size > 0){
        num_read = read(fd, buf, size);
        if(num_read <= 0){
            if(num_read == 0){
                printf("server sends incomplete data\n");
                return -1;
            }
            if(num_read < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)){
                continue;
            }
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
        if(num_write < 0){
            if(errno == ECONNRESET || errno == EPIPE){
                close(fd);
                exit(1);
            }
            if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK){
                continue;
            }
            return -1;
        }
        size = size - num_write;
        buf = buf + num_write;
    }
    return 0;
}

int main(void){
    struct sigaction sa;
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(3000);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(client_fd == -1){
        perror("client fd");
        return EXIT_FAILURE;
    }
    if(connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1){
        perror("connect");
        close(client_fd);
        return EXIT_FAILURE;
    }
    set_non_block(client_fd);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    // Registering handle for sigpipe
    sigaction(SIGPIPE, &sa, NULL);
    char read_buf[HEADER_LEN + MAX_MSG_LEN];
    char write_buf[HEADER_LEN + MAX_MSG_LEN];
    memset(read_buf, 0, sizeof(read_buf));
    memset(write_buf, 0, sizeof(write_buf));

    // Testing request-response pipelining
    const char *msg[]  = {"Hello World one from client", "Hello World two from client", "Hello World three from client","Hello World five from client"};
    printf("sending request to the server\n");
    int num_elements = sizeof(msg)/sizeof(char*);
    for(int i=0; i<num_elements; ++i){
        int msg_len = strlen(msg[i]);
        memcpy(write_buf, &msg_len, HEADER_LEN);
        memcpy(write_buf + HEADER_LEN, msg[i], msg_len);
        int err_code = write_message(client_fd, write_buf, HEADER_LEN + msg_len);
        if(err_code == -1){
            perror("write");
            close(client_fd);
        }
    }
    int err_code = 0;
    printf("receiving response from the server\n");
    for(int i=0; i<num_elements; ++i){
        err_code = read_message(client_fd, read_buf, HEADER_LEN);
        if(err_code == -1){
            if(errno == 0){
                fprintf(stdout, "End of Connection\n");
            }
            else{
                perror("read");
            }
            close(client_fd);
            return EXIT_FAILURE;
        }
        int msg_len = 0;
        memcpy(&msg_len, read_buf, HEADER_LEN);
        if(msg_len > MAX_MSG_LEN){
            fprintf(stdout, "Message received from the server is too long\n");
            close(client_fd);
            return EXIT_FAILURE;
        }
        err_code = 0;
        err_code = read_message(client_fd, read_buf + HEADER_LEN, msg_len);
        if(err_code == -1){
            perror("read");
            close(client_fd);
            return EXIT_FAILURE;
        }
        fprintf(stdout, "data send by the server: %.*s\n", msg_len, read_buf + HEADER_LEN);
    }
    close(client_fd);
    return EXIT_SUCCESS;
}
