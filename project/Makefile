CC=gcc
CFLAGS=-O2 -Wall -std=c11 -pthread
LIBS=-lmysqlclient
OBJS=server.o queries.o

all: server

server: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o server

