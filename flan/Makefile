all: flan.o libflan.so test bw_test rw_test flan_fuse

libflan.so:
	gcc -g -o libflan.so -lflexalloc flan.o -shared

flan.o:
	gcc -g -lflexalloc -fPIC -Wall -c flan.c

test:
	gcc -g -Wall test.c -o flan_test -L. -lflan -lflexalloc

rw_test:
	gcc -g -Wall rw_test.c -o flan_rwtest -L. -lflan -lflexalloc

bw_test:
	gcc -g -Wall bw_test.c -o flan_bwtest -L. -lflan -lflexalloc

flan_fuse:
	gcc -g -Wall flan_fuse.c `pkg-config fuse3 --cflags --libs` -L. -lflan -lflexalloc -o flan_fuse

clean:
	rm -f libflan.so flan.o flan_test flan_bwtest flan_rwtest flan_fuse

install:
	cp libflan.so /usr/local/lib/x86_64-linux-gnu/
	cp flan.h /usr/local/include/
