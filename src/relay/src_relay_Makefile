CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11
LDFLAGS = -lpthread

SRC = $(wildcard *.c)
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
