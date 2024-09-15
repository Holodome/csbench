CFLAGS += -std=c99 -Wall -Wextra -pedantic -O2 -Werror -g -O0
LDFLAGS += -lm -lpthread 

ifdef DEBUG
	CFLAGS += -O0 -g -fsanitize=address
	LDFLAGS += -fsanitize=address
else
	CFLAGS += -DNDEBUG
endif 
ifdef DEBUG_THREAD
	CFLAGS += -O0 -g -fsanitize=thread
	LDFLAGS += -fsanitize=thread
endif
ifdef COV
	CFLAGS += --coverage -fprofile-arcs -ftest-coverage
endif

all: csbench

csbench: csbench.c csbench_perf.c csbench_plot.c csbench_utils.c \
		 csbench_analyze.c csbench_report.c csbench_run.c csbench_serialize.c \
		 csbench_cli.c csbench_html.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: csbench
	install csbench /usr/local/bin
	install -g 0 -o 0 -m 0644 docs/csbench.1 /usr/local/share/man/man1

clean:
	rm -f csbench

amalgamated:
	./scripts/amalgamated.pl

.PHONY: all install clean amalgamated
