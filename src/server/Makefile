CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11 -I. -I../common
LDFLAGS = -lmicrohttpd -lrt -lpthread

SRC = $(wildcard *.c) ../common/shared_frame.c
OBJ = $(SRC:.c=.o)
TARGET = server

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
