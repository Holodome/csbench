CFLAGS += -std=c99 -Wall -Wextra -pedantic -O2
DEPFLAGS = -MT $@ -MMD -MP -MF $*.d

OBJS := main.o

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

