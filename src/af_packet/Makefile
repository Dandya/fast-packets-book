CC = gcc
CFLAGS = -Wall -std=gnu17 -O2
TARGETS = af_packet_classic

all: $(TARGETS)

af_packet_%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm $(TARGETS)