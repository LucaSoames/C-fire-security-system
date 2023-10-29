CC=gcc
CFLAGS=-pthread

all: project

project: simulator.o overseer.o cardreader.o door.o
	$(CC) $(CFLAGS) -o project simulator.o overseer.o cardreader.o door.o

simulator.o: simulator.c
	$(CC) $(CFLAGS) -c simulator.c

overseer.o: overseer.c overseer.h
	$(CC) $(CFLAGS) -c overseer.c

cardreader.o: cardreader.c
	$(CC) $(CFLAGS) -c cardreader.c

door.o: door.c
	$(CC) $(CFLAGS) -c door.c


clean:
	rm -f *.o project
