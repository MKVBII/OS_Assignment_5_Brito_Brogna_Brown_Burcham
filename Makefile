# Compiles the file manager and it's test program
#
# Usage:
# 	make -> build test_file
#       make run -> build and run the main application
#	make test -> build and run the tests
# 	make clean -> delete compiled files # 		      (keeps data/ folder)

CC = gcc # compiler

# show warnings, use c11 standard, enable threads, include debug info
CFLAGS = -Wall -Wextra -std=c11 -pthread -g

# link the math library
LDLIBS = -lm

# default target: just build the main and test program
all: main test_file

# Main project entry compilation
main: main.o threads_locks.o file_manager.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# test program is made from two compiled pieces
test_file: test_file.o file_manager.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Compiles object files
# Recompiles if a .c or .h file changes
main.o: main.c threads_locks.h file_manager.h
	$(CC) $(CFLAGS) -c main.c

threads_locks.o: threads_locks.c threads_locks.h file_manager.h
	$(CC) $(CFLAGS) -c threads_locks.c

file_manager.o: file_manager.c file_manager.h
	$(CC) $(CFLAGS) -c file_manager.c

test_file.o: test_file.c file_manager.h
	$(CC) $(CFLAGS) -c test_file.c

# Builds main if needed, then runs the main application
run: main
	./main

# build if needed, then run the tests
test: test_file
	./test_file

# delete build outputs (leaving the data/ folder alone)
clean: 
	rm -f *.o main test_file

# these names are commands, not real files
.PHONY: all run test clean
