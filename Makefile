ERROR_OPTS= -Wall -Wfatal-errors
DEBUG= -ggdb

all: libmalloc.so test1 test2 test3 test4 test5
.PHONY: all

test1: test1.c 
	gcc -o test1 ${DEBUG} ${ERROR_OPTS} test1.c 

test2: test2.c 
	gcc -o test2 ${DEBUG} ${ERROR_OPTS} test2.c 

test3: test3.c 
	gcc -o test3 ${DEBUG} ${ERROR_OPTS} test3.c 

test4: test4.c 
	gcc -o test4 ${DEBUG} ${ERROR_OPTS} test4.c

test5: test5.c 
	gcc -o test5 ${DEBUG} ${ERROR_OPTS} test5.c

libmalloc.so: malloc.c malloc.h memreq.c memreq.h
	gcc ${DEBUG} ${ERROR_OPTS} -fPIC -c -Wall memreq.c
	gcc ${DEBUG} ${ERROR_OPTS} -fPIC -c -Wall malloc.c
	gcc ${DEBUG} ${ERROR_OPTS} -shared -Wl,-soname,libmalloc.so -o libmalloc.so memreq.o malloc.o

clean:
	-@rm -f *.o test1 test2 test3 test4 test5 libmalloc.so
.PHONY: clean
