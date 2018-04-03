a:threadpool.o threadpool.test.o
	gcc -pthread  threadpool.test.o threadpool.o
threadpool.test.o:threadpool.test.c
	gcc -c threadpool.test.c
threadpool.o:threadpool.c threadpool.h
	gcc -c threadpool.c
clean:
	rm ./*.o