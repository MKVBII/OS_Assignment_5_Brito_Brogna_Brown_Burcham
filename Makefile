# Compiles the file manager and it's test program
#
# Usage:
# 	make -> build test_file
#	make test -> build and run the tests
# 	make clean -> delete compiled files # 		      (keeps data/ folder)

CC = gcc # compiler

# show warnings, use c11 standard, enable threads, include debug info
CFLAGS = -Wall -Wextra -std=c11 -pthread -g

# link the math library
LDLIBS = -lm

# default target: just build the test program
all: test_file

# test program is made from two compiled pieces
test_file: test_file.o file_manager.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# recompile the file manager if its .c or .h changes
file_manager.o: file_manager.c file_manager.h
	$(CC) $(CFLAGS) -c file_manager.c

# recompile the tests if the test file or the header changes
test_file.o: test_file.c file_manager.h
	$(CC) $(CFLAGS) -c test_file.c

# build if needed, then run the tests
test: test_file
	./test_file

# delete build output (leaving the data/ folder alone)
clean: 
	rm -f *.o test_file

# these names are commands, not real files
.PHONY: all test clean
