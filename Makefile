CFLAGS += -std=c99 -Wall -Wextra -pedantic -O2 #-O0 -g -fsanitize=address
LDFLAGS += -lm -lpthread #-fsanitize=address

all: csbench

csbench: csbench.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: csbench
	install csbench /usr/local/bin

clean:
	rm -f csbench

