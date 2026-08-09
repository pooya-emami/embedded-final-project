CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2 -std=gnu11 -I. -I.. -I../common
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -I. -I.. -I../common -I/usr/include/opencv4
LDFLAGS = -lpthread -lrt -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lopencv_dnn \
          -lonnxruntime

# Source files
SRC = detection_server.c \
      ../common/shared_frame.c \
      ../common/shared_frame_processed.c \
      human_detector.cpp

OBJ = detection_server.o shared_frame.o shared_frame_processed.o human_detector.o
TARGET = detection_server

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

detection_server.o: detection_server.c
	$(CC) $(CFLAGS) -c $< -o $@

shared_frame.o: ../common/shared_frame.c ../common/shared_frame.h
	$(CC) $(CFLAGS) -c $< -o $@

shared_frame_processed.o: ../common/shared_frame_processed.c ../common/shared_frame_processed.h
	$(CC) $(CFLAGS) -c $< -o $@

human_detector.o: human_detector.cpp human_detector.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

run: $(TARGET)
	./$(TARGET)

debug: CFLAGS += -g -DDEBUG -O0
debug: CXXFLAGS += -g -DDEBUG -O0
debug: clean $(TARGET)

.PHONY: all clean run debug