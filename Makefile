CFLAGS += -std=c99 -Wall -Wextra -pedantic -O2 
LDFLAGS += -lm -lpthread 

ifdef DEBUG
	CFLAGS += -O0 -g -fsanitize=address
	LDFLAGS += -fsanitize=address
endif 

all: csbench

csbench: csbench.c csbench_perf.c | csbench_perf.h
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: csbench
	install csbench /usr/local/bin

clean:
	rm -f csbench

