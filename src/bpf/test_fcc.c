
#include <dlfcn.h>

#include "settings.h"
#include "tests.h"

typedef uint32_t (*fcc_func_t)(const uint8_t*, uint32_t, uint32_t);

// Функция тестирования нативной фильтрации сетевого трафика.
void
test_fcc(struct pkt_descs_list* list) {
	struct filter_stats stats[TESTS_COUNT];
	void *lib_handle;
	fcc_func_t filter_fcc;
	char *error;
	
	lib_handle = dlopen("./fcc/filter_fcc.so", RTLD_NOW);
	if (!lib_handle) {
		fprintf(stderr, "%s\n", dlerror());
		return;
	}
	
	filter_fcc = (fcc_func_t)dlsym(lib_handle, "filter_fcc");
	
	if (!filter_fcc) {
		fprintf(stderr, "%s\n", dlerror());
		dlclose(lib_handle);
		return;
	}

	for (size_t s_i = 0; s_i < TESTS_COUNT; ++s_i) {
		struct filter_stats stat = {0, 0, 0, 0};
		struct pkt_desc* desc = list->descs;
		stat.start_usec = get_usec();
		for (size_t i = 0; i < list->count; ++i) {
			stat.filtered_packets++;

			if (filter_fcc(desc->data, desc->len, desc->caplen))
				stat.true_filtered++;

			desc = NEXT_PKT(desc);
		}
		stat.end_usec = get_usec();
		stats[s_i] = stat;
	}
	
	dlclose(lib_handle);
	print_stat(stats, "cBPF-native-fcc");
}

