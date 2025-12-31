

#include <getopt.h>
#include <pcap/pcap.h>
#include <pcap/bpf.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "fcc.h"

const char*
get_bpf_size(u_short c) {
	switch (BPF_SIZE(c)) {
		case BPF_W:
			return "W";
		case BPF_H:
			return "H";
		case BPF_B:
			return "B";
		default:
			std::cout << "Unknown bpf size\n";
			std::exit(-2);
	}
}

const char*
get_bpf_mode(u_short c) {
	switch (BPF_MODE(c)) {
		case BPF_IMM:
			return "IMM";
		case BPF_ABS:
			return "ABS";
		case BPF_IND:
			return "IND";
		case BPF_MEM:
			return "MEM";
		case BPF_LEN:
			return "LEN";
		case BPF_MSH:
			return "MSH";
		default:
			std::cout << "Unknown bpf mode\n";
			std::exit(-3);
	}
}

const char*
get_bpf_operation(u_short c) {
	switch (BPF_OP(c)) {
		case BPF_ADD:
			return "ADD";
		case BPF_SUB:
			return "SUB";
		case BPF_MUL:
			return "MUL";
		case BPF_DIV:
			return "DIV";
		case BPF_OR:
			return "OR";
		case BPF_AND:
			return "AND";
		case BPF_LSH:
			return "LSH";
		case BPF_RSH:
			return "RSH";
		case BPF_NEG:
			return "NEG";
		case BPF_MOD:
			return "MOD";
		case BPF_XOR:
			return "XOR";
		default:
			std::cout << "Unknown bpf operation\n";
			std::exit(-4);
	}
}

const char*
get_bpf_src(u_short c) {
	switch (BPF_SRC(c)) {
		case BPF_X:
			return "X";
		case BPF_K:
			return "K";
		default:
			std::cout << "Unknown bpf source\n";
			std::exit(-5);
	}
}

const char*
get_bpf_compare(u_short c) {
	switch (BPF_OP(c)) {
		case BPF_JA:
			return "JA";
		case BPF_JEQ:
			return "JEQ";
		case BPF_JGT:
			return "JGT";
		case BPF_JGE:
			return "JGE";
		case BPF_JSET:
			return "JSET";
		default:
			std::cout << "Unknown bpf compire operation\n";
			std::exit(-6);
	}
}

const char*
get_bpf_ret_val(u_short c) {
	switch (BPF_RVAL(c)) {
		case BPF_K:
			return "K";
		case BPF_A:
			return "A";
		default:
			std::cout << "Unknown bpf return value\n";
			std::exit(-7);
	}
}

const char*
get_bpf_misc_op(u_short c) {
	switch (BPF_MISCOP(c)) {
		case BPF_TAX:
			return "K";
		case BPF_TXA:
			return "A";
		default:
			std::cout << "Unknown bpf misc operation\n";
			std::exit(-8);
	}
}



void
create_code(const bpf_program& bpf, const std::string& func_name, std::stringstream& code) {
	code << FUNC_HEADER << FUNC_START_BEFORE_NAME << func_name << FUNC_START_AFTER_NAME;
	for (u_int insn_indx = 0; insn_indx < bpf.bf_len; ++insn_indx) {
		code << "com_" << insn_indx << ":\n\tCOM_";
		u_short c = bpf.bf_insns[insn_indx].code;
		switch (BPF_CLASS(c)) {
			case BPF_LD: {
				code << "LD_" << get_bpf_size(c) << "_" << get_bpf_mode(c) << "(0, 0, " << bpf.bf_insns[insn_indx].k << ")\n";
				break;
			}
			case BPF_LDX: {
				code << "LDX_" << get_bpf_size(c) << "_" << get_bpf_mode(c) << "(0, 0, " << bpf.bf_insns[insn_indx].k << ")\n";
				break;
			}
			case BPF_ST: {
				code << "ST" << "(0, 0, " << bpf.bf_insns[insn_indx].k << ")\n";
				break;
			}
			case BPF_STX: {
				code << "STX" << "(0, 0, " << bpf.bf_insns[insn_indx].k << ")\n";
				break;
			}
			case BPF_ALU: {
				code << "ALU_" << get_bpf_operation(c) << "_" << get_bpf_src(c) << "(0, 0, " << bpf.bf_insns[insn_indx].k << ")\n";
				break;
			}
			case BPF_JMP: {
				code << "JMP_" << get_bpf_compare(c) << "_" << get_bpf_src(c) << "(com_" << insn_indx + bpf.bf_insns[insn_indx].jt + 1  
						<< ", com_" << insn_indx + bpf.bf_insns[insn_indx].jf + 1 << ", " << bpf.bf_insns[insn_indx].k << ")\n";
				break;
			}
			case BPF_RET: {
				code << "RET_" << get_bpf_ret_val(c) << "(0, 0, " << bpf.bf_insns[insn_indx].k << ")\n";
				break;
			}
			case BPF_MISC: {
				code << "MISC_" << get_bpf_misc_op(c) << "(0, 0, 0)\n";
				break;
			}
			default: {
				std::cout << "Unknown bpf operation class\n";
				std::exit(-9);
			}
		}
	}

	code << FUNC_END;
}

struct fcc_args {
	const char* filter = nullptr;
	const char* name = nullptr;
	int link = DLT_EN10MB;
	int snaplen = 3000;
	int optimize = 1;
};

struct option long_options[] = {
	{"filter", required_argument, nullptr, 'f'},
	{"name", required_argument, nullptr, 'n'},
	{"link", required_argument, nullptr, 'l'},
	{"snaplen", required_argument, nullptr, 's'},
	{"optimize", required_argument, nullptr, 'o'},
	{"help", no_argument, nullptr, 'h'},
	{nullptr, 0, nullptr, 0}  // End of array
};

void
parse_opts(int argc, char** argv, fcc_args& args) {
	int option_index = 0;
	int c;
	
	while ((c = getopt_long(argc, argv, "f:n:l:s:h", long_options, &option_index)) != -1) {
		switch (c) {
			case 'f':
				args.filter = optarg;
				break;
			case 'n':
				args.name = optarg;
				break;
			case 'l':
				try {
					args.link = std::stoi(optarg);
				} catch (const std::exception& e) {
					std::cerr << "Error: link parameter must be an integer" << std::endl;
					std::exit(-1);
				}
				break;
			case 's':
				try {
					args.snaplen = std::stoi(optarg);
				} catch (const std::exception& e) {
					std::cerr << "Error: snaplen parameter must be an integer" << std::endl;
					std::exit(-1);
				}
				break;
			case 'o':
				try {
					args.optimize = std::stoi(optarg);
				} catch (const std::exception& e) {
					std::cerr << "Error: optimize parameter must be an integer" << std::endl;
					std::exit(-1);
				}
				break;
			case 'h':
				std::cout << "Usage: " << argv[0] << " [options]\n"
						<< "Options:\n"
						<< "  -f, --filter STRING   Filter string (required)\n"
						<< "  -n, --name STRING     Name (required)\n"
						<< "  -l, --link INT        Integer link (default DLT_EN10MB = 1)\n"
						<< "  -s, --snaplen INT     Integer snaplen (default 3000)\n"
						<< "  -o, --optimize INT    Optimize BPF code (default 1)\n"
						<< "  -h, --help            Show this help message\n"
						<< "See link types in https://www.tcpdump.org/linktypes.html\n";
				std::exit(0);
			default:
				std::cerr << "Unknown error parsing arguments" << std::endl;
				std::exit(-1);
		}
	}
	
	if (!args.filter || !args.name) {
		std::cerr << "Error: All parameters (filter, name, link) are required\n";
		std::cerr << "Use --help for usage information\n";
		std::exit(-1);
	}
}

int
main(int argc, char** argv) {
	fcc_args args;
	std::stringstream code;
	pcap_t *handle;
	bpf_program bpf;

	parse_opts(argc, argv, args);

	handle = pcap_open_dead(args.link, 3000);
	if (handle == NULL) {
		fprintf(stderr, "Open error pcap\n");
		return -11;
	}

	if (pcap_compile(handle, &bpf, args.filter, args.optimize, args.link) == -1) {
		fprintf(stderr, "Unsupported filter: %s\n", pcap_geterr(handle));
		pcap_close(handle);
		return -12;
	}

	create_code(bpf, args.name, code);

	std::cout << code.rdbuf();

	pcap_close(handle);
	pcap_freecode(&bpf);

	return 0;
}