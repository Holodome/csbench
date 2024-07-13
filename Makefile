CFLAGS += -std=c99 -Wall -Wextra -pedantic -O2 -Werror
LDFLAGS += -lm -lpthread 

ifdef DEBUG
	CFLAGS += -O0 -g -fsanitize=address
	LDFLAGS += -fsanitize=address
endif 
ifdef DEBUG_THREAD
	CFLAGS += -O0 -g -fsanitize=thread
	LDFLAGS += -fsanitize=thread
endif
ifdef COV
	CFLAGS += --coverage -fprofile-arcs -ftest-coverage
endif

all: csbench

csbench: csbench.c csbench_perf.c csbench_plot.c csbench_utils.c csbench_analyze.c csbench_report.c csbench_run.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: csbench
	install csbench /usr/local/bin

clean:
	rm -f csbench

amalgamated:
	./scripts/amalgamated.pl

.PHONY: all csbench install clean
