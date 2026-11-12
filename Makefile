CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c99 -D_DEFAULT_SOURCE -D_C_POSIX_SOURCE=200112

TARGET_FILE1 = redis_server
TARGET_FILE2 = redis_client
SRCS_FILE1 = redis_server.c
SRCS_FILE2 = redis_client.c

all: $(TARGET_FILE1) $(TARGET_FILE2)

$(TARGET_FILE1): $(SRCS_FILE1)
	$(CC) $(CFLAGS) $(SRCS_FILE1) -o $(TARGET_FILE1)

$(TARGET_FILE2): $(SRCS_FILE2)
	$(CC) $(CFLAGS) $(SRCS_FILE2) -o $(TARGET_FILE2)

clean:
	rm -r $(TARGET_FILE1) $(TARGET_FILE2)

.PHONY: all clean
