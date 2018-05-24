/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Tom St Denis <tom.stdenis@amd.com>
 *
 */
#include "umr.h"
#include <llvm-c/Disassembler.h>
#include <llvm-c/Target.h>

// assumes an instruction is at least 4 bytes long
// assumes we print 4 bytes per line so that a complete
// instruction might span more than 1 line

int umr_llvm_disasm(struct umr_asic *asic,
					uint8_t *inst, unsigned inst_bytes,
					uint64_t PC,
					char **disasm_text)
{
	LLVMDisasmContextRef disasm_ref;
	unsigned x, z, i;
	size_t n;
	char tmp[256], *cpuname;

	LLVMInitializeAllTargetInfos();
	LLVMInitializeAllTargetMCs();
	LLVMInitializeAllDisassemblers();

	// cpuname based on mesa usage
	cpuname = asic->asicname;
	if (asic->family == FAMILY_RV)
		cpuname = "gfx902";
	else if (asic->family > FAMILY_VI)
		cpuname = "gfx900";
	else if (!strcmp(cpuname, "polaris12") || !strcmp(cpuname, "vegam"))
		cpuname = "polaris11";
	else if (!strcmp(cpuname, "vega12"))
		cpuname = "gfx902";

	disasm_ref = LLVMCreateDisasmCPU(
			"amdgcn-mesa-mesa3d", cpuname,
			NULL, 0, NULL, NULL);

	if (!disasm_ref) {
		fprintf(stderr, "[ERROR]:  Could not create disassembler context\n");
		return -1;
	}

	for (i = x = 0; x < inst_bytes; x += n) {
		n = LLVMDisasmInstruction(
				disasm_ref,
				inst + x, inst_bytes - x,
				PC + x,
				tmp, sizeof(tmp));
		if (!n) {
			// invalid instruction, skip 4 bytes
			n = 4;
			disasm_text[i++] = strdup("...");
		} else {
			// valid instruction

			// if the instruction is longer than 4 bytes
			// then add ';;' to all but the first line
			disasm_text[i++] = strdup(tmp);
			for (z = 4; z < n; z += 4)
				disasm_text[i++] = strdup(";;");
		}
	}

	LLVMDisasmDispose(disasm_ref);
	return 0;
}

static struct umr_wave_data *find_wave(struct umr_wave_data *wd, unsigned vmid, uint64_t addr)
{
	while (wd) {
		uint64_t PC;
		PC = ((uint64_t)wd->ws.pc_hi << 32) | wd->ws.pc_lo;
		if (wd->ws.hw_id.vm_id == vmid && addr == PC)
			break;
		wd = wd->next;
	}
	return wd;
}


void umr_vm_disasm(struct umr_asic *asic, unsigned vmid, uint64_t addr, uint64_t PC, uint32_t size, uint32_t start_offset, struct umr_wave_data *wd)
{
	uint32_t *opcodes, x, y, nwave, wavehits;
	char **opcode_strs = NULL;
	struct umr_wave_data *pwd;

	wavehits = nwave = 0;
	pwd = wd;
	while (pwd) {
		++nwave;
		pwd = pwd->next;
	}

	opcodes = calloc(size/4, sizeof(*opcodes));
	if (!opcodes)
		goto error;

	opcode_strs = calloc(size/4, sizeof(opcode_strs[0]));
	if (!opcode_strs)
		goto error;

	umr_read_vram(asic, vmid, addr + start_offset, size, (void*)opcodes);
	umr_llvm_disasm(asic, (uint8_t *)opcodes, size, addr + start_offset, &opcode_strs[0]);

	for (y = 0, x = start_offset / 4; x < (start_offset + size)/4; x++, y++) {
		if (addr + 4 * x == PC)
			printf(" * ");
		else
			printf("   ");
		printf("pgm[%s%lu%s@%s0x%llx%s + %s0x%-4x%s] = %s0x%08lx%s\t%s%-60s%s\t",
			BLUE, (unsigned long)vmid, RST,
			YELLOW, (unsigned long long)addr, RST,
			YELLOW, (unsigned)x * 4, RST,
			BLUE, (unsigned long)opcodes[y], RST,
			GREEN, opcode_strs[y], RST);
		free(opcode_strs[y]);

		if (wd) {
			unsigned n;
			pwd = find_wave(wd, vmid, addr + x * 4);
			n = 0;
			while (pwd) {
				++n;
				++wavehits;
				if (asic->options.bitfields)
					printf("[se%u.sh%u.cu%u.simd%u.wave%u] ",
						(unsigned)pwd->se, (unsigned)pwd->sh, (unsigned)pwd->cu, (unsigned)pwd->ws.hw_id.simd_id, (unsigned)pwd->ws.hw_id.wave_id);
				pwd = find_wave(pwd->next, vmid, addr + x * 4);
			}
			if (n)
				printf("[%3u waves (%3u %%)]", n, (n * 100) / nwave);
		}
		printf("\n");
	}
	printf("End of disassembly.\n");

	if (wd && wavehits)
		printf("\t%u waves in this shader (out of %u active waves)\n", wavehits, nwave);

	free(opcode_strs);
	free(opcodes);
	return;
error:
	free(opcode_strs);
	fprintf(stderr, "[ERROR]: Out of memory\n");
}

// compute shader by looking for 5 s_endpgm opcodes in a row
#define S_ENDPGM 0xbf810000
#define S_ENDINV 0xbf9f0000

uint32_t umr_compute_shader_size(struct umr_asic *asic,
								 struct umr_shaders_pgm *shader)
{
	uint64_t addr;
	uint32_t buf[256/4]; // read 256 byte pages at a time
	uint32_t lastendpgm, endpgm_cnt, y, x;

	addr = shader->addr;
	endpgm_cnt = 0;
	x = 256 / 4;
	y = 0;
	lastendpgm = 0;
	for (;;) {
		if (x == 256/4) {
			// read next page
			// if we hit a fault just assume that's the end of the memory
			// mapped to the shader.  This is to account for
			// older UMDs that might not use the 5 ENDPGM postfix.
			if (umr_read_vram(asic, shader->vmid, addr, 256, &buf[0]) < 0)
				break;
			addr += 256;
			x = 0;
		}
		y += 4;
		if (buf[x] == S_ENDPGM || buf[x] == S_ENDINV) {
			lastendpgm = y - 4;
			++endpgm_cnt;
			if (endpgm_cnt == 5)
				break;
			if (asic->options.disasm_early_term)
				break;
		} else {
			endpgm_cnt = 0;
		}
		++x;
	}
	if (endpgm_cnt == 5)
		return y - 16; // remove last 4 endpgm's
	else
		return lastendpgm + 4; // assume the last endpgm seen was the end
}

