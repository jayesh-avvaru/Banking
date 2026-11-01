CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -pthread -Icommon -Iserver
SRV_SRC = server/server.c server/session.c server/bank.c server/storage.c server/auth.c common/netutil.c

all: bin/server bin/client bin/stress

bin:
	mkdir -p bin

bin/server: $(SRV_SRC) $(wildcard server/*.h common/*.h) | bin
	$(CC) $(CFLAGS) -o $@ $(SRV_SRC)

bin/client: client/client.c common/netutil.c $(wildcard common/*.h) | bin
	$(CC) $(CFLAGS) -o $@ client/client.c common/netutil.c

bin/stress: tests/stress.c common/netutil.c $(wildcard common/*.h) | bin
	$(CC) $(CFLAGS) -o $@ tests/stress.c common/netutil.c

clean:
	rm -rf bin

.PHONY: all clean
