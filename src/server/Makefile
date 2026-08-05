CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11 -I. -I../common
LDFLAGS = -lpthread -lrt -lssl -lcrypto

SRC = server.c ../common/shared_frame.c
OBJ = $(SRC:.c=.o)
TARGET = server

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJ) $(TARGET)
