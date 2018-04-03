a:threadpool.o threadpool.test.o
	gcc -pthread  threadpool.test.o threadpool.o
threadpool_test.o:threadpool.test.c
	gcc -c threadpool.test.c
threadpool.o:threadpool.c threadpool.h
	gcc -c threadpool.c
clean:
	rm threadpool.o threadpool.test.o