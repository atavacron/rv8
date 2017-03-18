//
//  test-jit.cc
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cinttypes>
#include <csignal>
#include <csetjmp>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cwchar>
#include <climits>
#include <cfloat>
#include <cfenv>
#include <cstddef>
#include <limits>
#include <array>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <random>
#include <deque>
#include <map>
#include <thread>
#include <atomic>
#include <type_traits>

#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "histedit.h"

#include "host-endian.h"
#include "types.h"
#include "fmt.h"
#include "bits.h"
#include "sha512.h"
#include "format.h"
#include "meta.h"
#include "util.h"
#include "host.h"
#include "cmdline.h"
#include "config-parser.h"
#include "config-string.h"
#include "codec.h"
#include "strings.h"
#include "disasm.h"
#include "alu.h"
#include "fpu.h"
#include "pma.h"
#include "amo.h"
#include "processor-logging.h"
#include "processor-base.h"
#include "processor-impl.h"
#include "interp.h"
#include "processor-model.h"
#include "mmu-proxy.h"
#include "unknown-abi.h"
#include "processor-proxy.h"
#include "debug-cli.h"

#include "asmjit.h"

#include "fusion-decode.h"
#include "fusion-emitter.h"
#include "fusion-tracer.h"
#include "fusion-runloop.h"

#include "assembler.h"
#include "jit.h"

using namespace riscv;

using proxy_jit_rv64imafdc = fusion_runloop<processor_proxy
	<processor_rv64imafdc_model<fusion_decode,processor_rv64imafd,mmu_proxy_rv64>>>;

template <typename P>
struct rv_test_jit
{
	int total_tests = 0;
	int tests_passed = 0;

	rv_test_jit() : total_tests(0), tests_passed(0) {}

	void run_test(const char* test_name, P &emulator, addr_t pc, size_t step)
	{
		printf("\n=========================================================\n");
		printf("TEST: %s\n", test_name);
		typename P::ireg_t save_regs[P::ireg_count];
		size_t regfile_size = sizeof(typename P::ireg_t) * P::ireg_count;

		/* clear registers */
		memset(&emulator.ireg[0], 0, regfile_size);

		/* step the interpreter */
		printf("\n--[ interp ]---------------\n");
		emulator.log = proc_log_inst;
		emulator.pc = pc;
		emulator.step(step);

		/* save and reset registers */
		memcpy(&save_regs[0], &emulator.ireg[0], regfile_size);
		memset(&emulator.ireg[0], 0, regfile_size);

		/* compile the program buffer trace */
		printf("\n--[ jit ]------------------\n");
		emulator.log = proc_log_jit_trace;
		emulator.pc = pc;
		emulator.start_trace();

		/* reset registers */
		memset(&emulator.ireg[0], 0, regfile_size);

		/* run compiled trace */
		auto fn = emulator.trace_cache[pc];
		fn(static_cast<processor_rv64imafd*>(&emulator));

		/* print result */
		printf("\n--[ result ]---------------\n");
		bool pass = true;
		for (size_t i = 0; i < P::ireg_count; i++) {
			if (save_regs[i].r.xu.val != emulator.ireg[i].r.xu.val) {
				pass = false;
				printf("ERROR interp-%s=0x%016llx jit-%s=0x%016llx\n",
					rv_ireg_name_sym[i], save_regs[i].r.xu.val,
					rv_ireg_name_sym[i], emulator.ireg[i].r.xu.val);
			}
		}
		printf("%s\n", pass ? "PASS" : "FAIL");
		if (pass) tests_passed++;
		total_tests++;
	}

	void test_addi_1()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_a0, rv_ireg_zero, 0xde);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 1);
	}

	void test_add_1()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_a1, rv_ireg_zero, 0x7ff);
		asm_add(as, rv_ireg_a0, rv_ireg_zero, rv_ireg_a1);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 2);
	}

	void test_add_2()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_a1, rv_ireg_zero, 0x7ff);
		asm_add(as, rv_ireg_a0, rv_ireg_a1, rv_ireg_zero);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 2);
	}

	void test_add_3()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_a1, rv_ireg_zero, 0x7ff);
		asm_addi(as, rv_ireg_a0, rv_ireg_zero, 0x1);
		asm_add(as, rv_ireg_a1, rv_ireg_a1, rv_ireg_a0);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 3);
	}

	void test_add_4()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_s10, rv_ireg_zero, 0x7ff);
		asm_add(as, rv_ireg_s10, rv_ireg_zero, rv_ireg_s11);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 2);
	}

	void test_add_5()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_s10, rv_ireg_zero, 0x7ff);
		asm_add(as, rv_ireg_s10, rv_ireg_s11, rv_ireg_zero);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 2);
	}

	void test_add_6()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_a0, rv_ireg_zero, 0x7ff);
		asm_addi(as, rv_ireg_s11, rv_ireg_zero, 1);
		asm_add(as, rv_ireg_a0, rv_ireg_s9, rv_ireg_s11);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 3);
	}

	void test_add_7()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_a0, rv_ireg_zero, 0x7ff);
		asm_addi(as, rv_ireg_s11, rv_ireg_zero, 1);
		asm_add(as, rv_ireg_s9, rv_ireg_a0, rv_ireg_s11);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 3);
	}

	void test_add_8()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_s10, rv_ireg_zero, 0x7ff);
		asm_addi(as, rv_ireg_s11, rv_ireg_zero, 1);
		asm_add(as, rv_ireg_s9, rv_ireg_s10, rv_ireg_s11);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 3);
	}

	void test_slli_1()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_a0, rv_ireg_a0, 0xde);
		asm_slli(as, rv_ireg_a0, rv_ireg_a0, 8);
		asm_addi(as, rv_ireg_a0, rv_ireg_a0, 0xad);
		asm_slli(as, rv_ireg_a0, rv_ireg_a0, 8);
		asm_addi(as, rv_ireg_a0, rv_ireg_a0, 0xbe);
		asm_slli(as, rv_ireg_a0, rv_ireg_a0, 8);
		asm_addi(as, rv_ireg_a0, rv_ireg_a0, 0xef);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 7);
	}

	void test_sll_1()
	{
		P emulator;
		assembler as;

		asm_addi(as, rv_ireg_s9, rv_ireg_zero, 12);
		asm_addi(as, rv_ireg_s10, rv_ireg_zero, 0x7ff);
		asm_add(as, rv_ireg_s11, rv_ireg_zero, rv_ireg_s10);
		asm_sll(as, rv_ireg_s11, rv_ireg_s11, rv_ireg_s9);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 4);
	}

	void test_load_imm_1()
	{
		P emulator;
		assembler as;

		as.load_imm(rv_ireg_a0, 0xfeedcafebabe);
		asm_ebreak(as);
		as.link();

		run_test(__func__, emulator, (addr_t)as.get_section(".text")->buf.data(), 6);
	}

	void print_summary()
	{
		printf("\n%d/%d tests successful\n", tests_passed, total_tests);
	}
};

int main(int argc, char *argv[])
{
	rv_test_jit<proxy_jit_rv64imafdc> test;
	test.test_addi_1();
	test.test_add_1();
	test.test_add_2();
	test.test_add_3();
	test.test_add_4();
	test.test_add_5();
	test.test_add_6();
	test.test_add_7();
	test.test_add_8();
	test.test_slli_1();
	test.test_sll_1();
	test.test_load_imm_1();
	test.print_summary();
}
