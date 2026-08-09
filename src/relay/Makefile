CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11 -I. -I.. -I../common
LDFLAGS = -lpthread -lrt

SRC = mjpeg_relay.c ../common/shared_frame.c
OBJ = mjpeg_relay.o shared_frame.o
TARGET = mjpeg_relay

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

mjpeg_relay.o: mjpeg_relay.c mjpeg_relay.h ../common/shared_frame.h
	$(CC) $(CFLAGS) -c $< -o $@

shared_frame.o: ../common/shared_frame.c ../common/shared_frame.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

run: $(TARGET)
	./$(TARGET)

debug: CFLAGS += -g -DDEBUG -O0
debug: clean $(TARGET)

.PHONY: all clean run debug