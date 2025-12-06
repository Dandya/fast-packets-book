CC = gcc
CFLAGS = -Wall -std=c17 -O2
TARGETS = af_packet_classic af_packet_rings

all: $(TARGETS)

af_packet_%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm $(TARGETS)