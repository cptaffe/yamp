
CFLAGS = -Weverything --std=c11 -D_POSIX_SOURCE -D_GNU_SOURCE -g

main: main.o
	$(CC) $(LDFLAGS) $^ -o $@

main.o: main.c
	$(CC) $(CFLAGS) -c $^ -o $@

clean:
	$(RM) main.o main
