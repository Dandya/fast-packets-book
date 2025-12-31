
#pragma once

const char* FUNC_HEADER = R"(
#include <stdint.h>

#include <pcap/pcap.h>
#include <pcap/bpf.h>

#define EXTRACT_SHORT(k) ntohs(k)
#define EXTRACT_LONG(k)	ntohl(k)

#define COM_RET_K(jt, jf, k) \
		return k;

#define COM_RET_A(jt, jf, k) \
		return A;

#define COM_LD_W_ABS(jt, jf, k) \
		if (k > buflen || sizeof(int32_t) > buflen - k) \
			return 0; \
		A = EXTRACT_LONG(*(uint32_t*)(p + k));

#define COM_LD_H_ABS(jt, jf, k) \
		if (k > buflen || sizeof(int16_t) > buflen - k) \
			return 0; \
		A = EXTRACT_SHORT(*(uint16_t*)(p + k));
		
#define COM_LD_B_ABS(jt, jf, k) \
		if (k >= buflen) \
			return 0; \
		A = p[k];

#define COM_LD_W_LEN(jt, jf, k) \
		A = wirelen;
		
#define COM_LDX_W_LEN(jt, jf, k) \
		X = wirelen;
		
#define COM_LD_W_IND(jt, jf, k) \
		K = X + k; \
		if (k > buflen || X > buflen - k || sizeof(int32_t) > buflen - K) \
			return 0; \
		A = EXTRACT_LONG(*(uint32_t*)(p + K));
		
#define COM_LD_H_IND(jt, jf, k) \
		K = X + k; \
		if (k > buflen || X > buflen - k || sizeof(int16_t) > buflen - K) \
			return 0; \
		A = EXTRACT_SHORT(*(uint16_t*)(p + K));
		
#define COM_LD_B_IND(jt, jf, k) \
		K = X + k; \
		if (k >= buflen || X >= buflen - k) \
			return 0; \
		A = p[K];
		
#define COM_LDX_B_MSH(jt, jf, k) \
		if (k >= buflen) \
			return 0; \
		X = (p[k] & 0xf) << 2;
		
#define COM_LD_IMM(jt, jf, k) \
		A = k;
		
#define COM_LDX_IMM(jt, jf, k) \
		X = k;
		
#define COM_LD_MEM(jt, jf, k) \
		A = mem[k];
		
#define COM_LDX_MEM(jt, jf, k) \
		X = mem[k];
		
#define COM_ST(jt, jf, k) \
		mem[k] = A;
		
#define COM_STX(jt, jf, k) \
		mem[k] = X;
		
#define COM_JMP_JA(jt, jf, k) \
		goto k;
		
#define COM_JMP_JGT_K(jt, jf, k) \
		if (A > k) goto jt; else goto jf;
		
#define COM_JMP_JGE_K(jt, jf, k) \
		if (A >= k) goto jt; else goto jf;
		
#define COM_JMP_JEQ_K(jt, jf, k) \
		if (A == k) goto jt; else goto jf;
		
#define COM_JMP_JSET_K(jt, jf, k) \
		if (A & k) goto jt; else goto jf;
		
#define COM_JMP_JGT_X(jt, jf, k) \
		if (A > X) goto jt; else goto jf;
		
#define COM_JMP_JGE_X(jt, jf, k) \
		if (A >= X) goto jt; else goto jf;
		
#define COM_JMP_JEQ_X(jt, jf, k) \
		if (A == X) goto jt; else goto jf;
		
#define COM_JMP_JSET_X(jt, jf, k) \
		if (A & X) goto jt; else goto jf;
		
#define COM_ALU_ADD_X(jt, jf, k) \
		A += X;
		
#define COM_ALU_SUB_X(jt, jf, k) \
		A -= X;
		
#define COM_ALU_MUL_X(jt, jf, k) \
		A *= X;
		
#define COM_ALU_DIV_X(jt, jf, k) \
		if (X == 0) \
			return 0; \
		A /= X;
		
#define COM_ALU_MOD_X(jt, jf, k) \
		if (X == 0) \
			return 0; \
		A %= X;
		
#define COM_ALU_AND_X(jt, jf, k) \
		A &= X;
		
#define COM_ALU_OR_X(jt, jf, k) \
		A |= X;
		
#define COM_ALU_XOR_X(jt, jf, k) \
		A ^= X;
		
#define COM_ALU_LSH_X(jt, jf, k) \
		if (X < 32) A <<= X; else A = 0;
		
#define COM_ALU_RSH_X(jt, jf, k) \
		if (X < 32) A >>= X; else A = 0;
		
#define COM_ALU_ADD_K(jt, jf, k) \
		A += k;
		
#define COM_ALU_SUB_K(jt, jf, k) \
		A -= k;
		
#define COM_ALU_MUL_K(jt, jf, k) \
		A *= k;
		
#define COM_ALU_DIV_K(jt, jf, k) \
		A /= k;
		
#define COM_ALU_MOD_K(jt, jf, k) \
		A %= k;
		
#define COM_ALU_AND_K(jt, jf, k) \
		A &= k;
		
#define COM_ALU_OR_K(jt, jf, k) \
		A |= k;
		
#define COM_ALU_XOR_K(jt, jf, k) \
		A ^= k;
		
#define COM_ALU_LSH_K(jt, jf, k) \
		A <<= k;
		
#define COM_ALU_RSH_K(jt, jf, k) \
		A >>= k;
		
#define COM_ALU_NEG(jt, jf, k) \
		A = (0U - A);
		
#define COM_MISC_TAX(jt, jf, k) \
		X = A;
		
#define COM_MISC_TXA(jt, jf, k) \
		A = X;

)";

const char* FUNC_START_BEFORE_NAME = R"(u_int
)";

const char* FUNC_START_AFTER_NAME = R"((const u_char* p, u_int wirelen, u_int buflen) {
	register uint32_t A, X;
	register bpf_u_int32 K;
	uint32_t mem[BPF_MEMWORDS];

)";

const char* FUNC_END = R"(
	return 0;
}
)";