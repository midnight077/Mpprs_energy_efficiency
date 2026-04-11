CXX      = g++
CC       = gcc
CXXFLAGS = -O3 -std=c++11 -pthread
CFLAGS   = -O3

all: streams

profiler.o: profiler.c
	$(CC) $(CFLAGS) -c profiler.c -o profiler.o -lpfm

quill-runtime.o: quill-runtime.cpp quill-runtime.h quill.h
	$(CXX) $(CXXFLAGS) -c quill-runtime.cpp -o quill-runtime.o

streams: streams.cpp quill-runtime.o profiler.o
	$(CXX) $(CXXFLAGS) streams.cpp quill-runtime.o profiler.o -o streams -lpfm

clean:
	rm -f *.o streams