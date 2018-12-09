all: lru-sequential lru-mutex lru-fine

CFLAGS = -g -Wall -Werror -pthread

%.o: %.c *.h
	gcc $(CFLAGS) -c -o $@ $<

lru-sequential: main.c sequential-lru.o
	gcc $(CFLAGS) -o lru-sequential sequential-lru.o main.c

lru-mutex: main.c mutex-lru.o
	gcc $(CFLAGS) -o lru-mutex mutex-lru.o main.c

lru-fine: main.c fine-lru.o
	gcc $(CFLAGS) -o lru-fine fine-lru.o main.c

handin: clean
	@if [ `git status --porcelain| wc -l` != 0 ] ; then echo "\n\n\n\n\t\tWARNING: YOU HAVE UNCOMMITTED CHANGES\n\n    Consider committing any pending changes and rerunning make handin.\n\n\n\n"; fi
	git tag -f -a handin -m "Lab 3 Handin"
	git push -f origin --tags handin

update:
	git checkout master
	git pull https://github.com/comp530-f18/lab3.git master

clean:
	rm -f *~ *.o lru-sequential lru-mutex lru-rw lru-fine
