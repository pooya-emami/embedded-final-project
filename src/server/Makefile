CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11 -I.
LDFLAGS = -lmicrohttpd -lssl -lcrypto -lsqlite3 -lmosquitto -lpthread

SRC = $(wildcard *.c)
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
