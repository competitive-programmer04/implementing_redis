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

#define HEADER_LEN 4
#define MAX_MSG_LEN 4096


int read_message(int fd, char *buf, size_t size){
    ssize_t num_read = 0;
    errno = 0;
    while(size > 0){
        num_read = read(fd, buf, size);
        if(num_read < 0 && errno != EINTR){
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
        if(num_write < 0 && errno != EINTR){
            return -1;
        }
        size = size - num_write;
        buf = buf + num_write;
    }
    return 0;
}

int main(void){
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = ntohs(3000);
    server_addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);

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
    /* Message Structure
     first 4 bytes will indicate the payload length
     then the next bytes will contain the payload
     */
    char read_buf[HEADER_LEN + MAX_MSG_LEN];
    char write_buf[HEADER_LEN + MAX_MSG_LEN];
    memset(read_buf, 0, sizeof(read_buf));
    memset(write_buf, 0, sizeof(write_buf));
    const char *msg = "Hello World from client";
    int msg_len = strlen(msg);
    memcpy(write_buf, &msg_len, sizeof(int));
    memcpy(write_buf + HEADER_LEN, msg, msg_len);
    int err_code = write_message(client_fd, write_buf, sizeof(write_buf));
    if(err_code == -1){
        perror("write");
        close(client_fd);
    }
    err_code = 0;
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
    msg_len = 0;
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
    close(client_fd);
    return EXIT_SUCCESS;
}
