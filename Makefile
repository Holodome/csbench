CFLAGS += -std=c99 -Wall -Wextra -pedantic -O2 -g -fsanitize=address
LDFLAGS += -fsanitize=address
DEPFLAGS = -MT $@ -MMD -MP -MF $*.d

OBJS := csbench.o

all: csbench

csbench: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c 
	$(CC) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(shell find . -name "*.o" \
		-o -name "*.d" \
		-o -name "*.i") csbench

-include $(OBJS:%.o=%.d)

