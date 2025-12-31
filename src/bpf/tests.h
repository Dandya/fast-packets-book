
#pragma once

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pcap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "settings.h"

// Структура статистики работы фильтра.
struct filter_stats {
	uint64_t filtered_packets;
	uint64_t true_filtered;
	uint64_t start_usec;
	uint64_t end_usec;
};

// Структура элемента списка пакетов для фильтрации.
struct pkt_desc {
	uint32_t len;
	uint32_t caplen;
	uint64_t offset_to_next;
	u_char data[];
};

// Структура списка пакетов.
struct pkt_descs_list {
	size_t count;
	size_t len;
	struct pkt_desc* descs;
};

#define PAGE_SIZE 4096
#define NEXT_PKT(pkt) (struct pkt_desc*)((uint8_t*)pkt + pkt->offset_to_next)
#define PAGE_ALIGN(addr) ((uintptr_t)(addr) & ~(PAGE_SIZE - 1))

// Вспомогательная функция получения времени в наносекундах.
static inline uint64_t get_usec() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

#define GET_SPEED(stat) (((double)(stat->filtered_packets)) / (stat->end_usec - stat->start_usec))

// Вспомогательная функция вывода статистики теста фильтра.
static inline void print_stat(const struct filter_stats* stat, const char* name) {
	uint64_t all_f = stat->filtered_packets;
	uint64_t true_f = stat->true_filtered;
	double min_speed = GET_SPEED(stat);
	double max_speed = GET_SPEED(stat);
	double mid_speed = GET_SPEED(stat);

	for (size_t i = 1; i < TESTS_COUNT; ++i) {
		const struct filter_stats* stat_i = stat + i;
		double speed = GET_SPEED(stat_i);
		if (speed < min_speed)
			min_speed = speed;
		else if (speed > max_speed)
			max_speed = speed;
		mid_speed += speed;
	}

	mid_speed /= TESTS_COUNT;

	printf("=============\n%s\n", name);
	printf("Filtered (all): %llu\n", all_f);
	printf("Filtered (true): %llu\n", true_f);
	printf("Min speed in usec: %lf\n", min_speed);
	printf("Mid speed in usec: %lf\n", mid_speed);
	printf("Max speed in usec: %lf\n", max_speed);
}