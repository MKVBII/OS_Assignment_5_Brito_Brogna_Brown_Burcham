# Build file for the recovery and logging part (Member 4)
#
# Uses the group's Makefile as is (same compiler and flags) and only adds
# targets for the new files, so the group's Makefile is never changed.
#
# Usage:
# 	make -f recovery.mk -> build recovery_demo and test_recovery
# 	make -f recovery.mk demo -> run Mode C, T2 recovers by timeout
# 	make -f recovery.mk demo_abort -> run Mode C, T2 is asked to abort
# 	make -f recovery.mk test_recovery_run -> run the recovery and logging tests
# 	make -f recovery.mk clean_recovery -> delete what this file builds

include Makefile

.DEFAULT_GOAL := recovery_all

recovery_all: recovery_demo test_recovery

# Mode C demo, links the group's threads_locks.o and file_manager.o unchanged
recovery_demo: recovery_demo.o recovery.o logger.o threads_locks.o file_manager.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# recovery and logging tests
test_recovery: test_recovery.o recovery.o logger.o file_manager.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

logger.o: logger.c logger.h
	$(CC) $(CFLAGS) -c logger.c

recovery.o: recovery.c recovery.h logger.h threads_locks.h file_manager.h
	$(CC) $(CFLAGS) -c recovery.c

recovery_demo.o: recovery_demo.c recovery.h logger.h threads_locks.h file_manager.h
	$(CC) $(CFLAGS) -c recovery_demo.c

test_recovery.o: test_recovery.c recovery.h logger.h threads_locks.h file_manager.h
	$(CC) $(CFLAGS) -c test_recovery.c

demo: recovery_demo
	./recovery_demo

demo_abort: recovery_demo
	./recovery_demo abort

test_recovery_run: test_recovery
	./test_recovery

clean_recovery:
	rm -f logger.o recovery.o recovery_demo.o test_recovery.o recovery_demo test_recovery

.PHONY: recovery_all demo demo_abort test_recovery_run clean_recovery
