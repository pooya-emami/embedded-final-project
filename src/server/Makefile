CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2 -std=gnu11 -I. -I.. -I../common -I../email -I../mqtt -I../sqlite
LDFLAGS = -lpthread -lrt -lssl -lcrypto -lcurl -lsqlite3 -lpaho-mqtt3c

# Source files
SRC = server.c \
      ../common/shared_frame.c \
      ../common/shared_frame_processed.c \
      ../email/email_sender.c \
      ../mqtt/mqtt_client.c \
      ../sqlite/sqlite_history.c

OBJ = server.o shared_frame.o email_sender.o mqtt_client.o sqlite_history.o
TARGET = server

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

server.o: server.c server.h
	$(CC) $(CFLAGS) -c $< -o $@

shared_frame.o: ../common/shared_frame.c ../common/shared_frame.h
	$(CC) $(CFLAGS) -c $< -o $@

email_sender.o: ../email/email_sender.c ../email/email_sender.h
	$(CC) $(CFLAGS) -c $< -o $@

mqtt_client.o: ../mqtt/mqtt_client.c ../mqtt/mqtt_client.h
	$(CC) $(CFLAGS) -c $< -o $@

sqlite_history.o: ../sqlite/sqlite_history.c ../sqlite/sqlite_history.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

run: $(TARGET)
	./$(TARGET)

debug: CFLAGS += -g -DDEBUG -O0
debug: clean $(TARGET)

.PHONY: all clean run debug