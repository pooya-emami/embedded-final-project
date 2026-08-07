CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2 -std=gnu11 -I. -I../common -I../detection
CXXFLAGS = -Wall -Wextra -O2 -std=c++11 -I. -I../common -I../detection `pkg-config --cflags opencv4`
LDFLAGS = -lpthread -lrt -lssl -lcrypto `pkg-config --libs opencv4`

SRC = server.c ../common/shared_frame.c ../detection/human_detector.cpp
OBJ = server.o shared_frame.o human_detector.o
TARGET = server

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

server.o: server.c
	$(CC) $(CFLAGS) -c $< -o $@

shared_frame.o: ../common/shared_frame.c
	$(CC) $(CFLAGS) -c $< -o $@

human_detector.o: ../detection/human_detector.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

run: $(TARGET)
	./$(TARGET)

debug: CFLAGS += -g -DDEBUG -O0
debug: CXXFLAGS += -g -DDEBUG -O0
debug: clean $(TARGET)

.PHONY: all clean run debug
