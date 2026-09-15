CC=g++
CFLAGS= -g -Wall 

all: proxy

proxy: Main.c config.c
	$(CC) $(CFLAGS) -o proxy_parse.o -c proxy_parse.c -lpthread
	$(CC) $(CFLAGS) -o config.o -c config.c -lpthread
	$(CC) $(CFLAGS) -o proxy.o -c Main.c -lpthread
	$(CC) $(CFLAGS) -o proxy proxy_parse.o config.o proxy.o -lpthread

clean:
	rm -f proxy *.o

tar:
	tar -cvzf ass1.tgz Main.c README Makefile proxy_parse.c proxy_parse.h