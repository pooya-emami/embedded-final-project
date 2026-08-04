CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11 -I. -I../common
LDFLAGS = -lpthread -lrt

SRC = $(wildcard *.c) ../common/shared_frame.c
OBJ = $(SRC:.c=.o)
TARGET = mjpeg_relay

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
