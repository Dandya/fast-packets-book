
#include <stdlib.h>
#include <string.h>

#include <bpf/libbpf.h>
#include <bpf/btf.h>

// Вспомогательная функция чтения инструкций первой eBPF функции из ELF-файла. 
int read_ebpf_elf(const char *path, struct bpf_insn** insns, uint32_t* count) {
	struct bpf_object *obj;
	struct bpf_program *prog;
	
	// Открываем ELF файл.
	obj = bpf_object__open(path);
	if (!obj) {
		fprintf(stderr, "Failed to open BPF object\n");
		return -1;
	}
	
	// Проходим по всем функциям в ELF.
	bpf_object__for_each_program(prog, obj) {
		// Получаем набор инструкций ebpf данной функции.
		const struct bpf_insn* prog_insns = bpf_program__insns(prog);
		size_t insn_cnt = bpf_program__insn_cnt(prog);
		*insns = calloc(insn_cnt, sizeof(struct bpf_insn));
		if (!*insns) {
			bpf_object__close(obj);
			fprintf(stderr, "Failed to get memory\n");
			return -1;
		}
		*count = insn_cnt;
		// Копируем набор для вызывающей функции.
		memcpy(*insns, prog_insns, insn_cnt * sizeof(struct bpf_insn));

		bpf_object__close(obj);
		return 0;
	}
	
	bpf_object__close(obj);
	return -2;
}