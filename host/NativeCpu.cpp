// SPDX-License-Identifier: GPL-3.0+
//
// ps2native PCSX2 backend.
//
// The EE "CPU provider" plugged into PCSX2's VMManager runs the natively
// recompiled program.  Everything the program does to hardware goes through
// PCSX2's memory map (vtlb), so the DMAC, VIF, VU1 (microVU), GIF, GS
// (Vulkan / D3D11 / D3D12 / OpenGL / software), timers and vsync are PCSX2's
// own implementations.  EE kernel services and the IOP are HLE'd by the
// ps2native runtime, so no BIOS is required.
//
// Timing: the EE cycle counter follows host real time (294.912 MHz) and
// PCSX2's event scheduler is run at the runtime's safe points.

#include "pcsx2/PrecompiledHeader.h"
#include "pcsx2/Common.h"
#include "pcsx2/R5900.h"
#include "pcsx2/R3000A.h"
#include "pcsx2/Memory.h"
#include "pcsx2/VU.h"
#include "pcsx2/VUmicro.h"
#include "pcsx2/Hw.h"
#include "pcsx2/Dmac.h"
#include "pcsx2/Counters.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/VMManager.h"
#include "common/Console.h"

#include "NativeHost.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fmt/format.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

static constexpr double EE_CLOCK = 294912000.0;

static u64 s_cycle_base;
static double s_time_base;
static uint s_last_frame;
static bool s_started;
static u64 s_events, s_lost_cycles;
static std::atomic<bool> s_quit{false};

// --trace timing: where host time goes (be_update total, event tests, vsync work).
static double s_t_update, s_t_events, s_t_vblank;
static LARGE_INTEGER s_qpf;
static inline double Now()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	if (!s_qpf.QuadPart)
		QueryPerformanceFrequency(&s_qpf);
	return static_cast<double>(t.QuadPart) / static_cast<double>(s_qpf.QuadPart);
}

// ---------------------------------------------------------------- IOP
// The IOP is HLE'd at the SIF level by the runtime; PCSX2's IOP never runs.
static void NullIopReserve() {}
static void NullIopReset() {}
static s32 NullIopExecuteBlock(s32 eeCycles)
{
	psxRegs.cycle += static_cast<u32>(eeCycles) >> 3;
	return 0;
}
static void NullIopClear(u32, u32) {}
static void NullIopShutdown() {}

R3000Acpu g_native_iop_cpu = {NullIopReserve, NullIopReset, NullIopExecuteBlock, NullIopClear, NullIopShutdown};

// ---------------------------------------------------------------- EE
static void NativeReserve() {}
static void NativeShutdown() {}
static void NativeReset() {}
static void NativeStep() {}

// ---------------------------------------------------------------- --trace sampling profiler
// A helper thread interrupts the EE/CPU thread ~1000 times a second and
// records its instruction pointer; be_report() prints the hottest functions.
static HANDLE s_prof_target;
static std::mutex s_prof_mutex;
static std::unordered_map<u64, u32> s_prof_hits;
static u32 s_prof_total;

static void ProfilerThread()
{
	timeBeginPeriod(1);
	for (;;)
	{
		Sleep(1);
		if (SuspendThread(s_prof_target) == static_cast<DWORD>(-1))
			return;
		CONTEXT ctx = {};
		ctx.ContextFlags = CONTEXT_CONTROL;
		const BOOL ok = GetThreadContext(s_prof_target, &ctx);
		ResumeThread(s_prof_target); // resume before allocating (heap locks)
		if (ok)
		{
			std::lock_guard lock(s_prof_mutex);
			s_prof_hits[ctx.Rip]++;
			s_prof_total++;
		}
	}
}

static void ProfilerReport()
{
	std::unordered_map<u64, u32> hits;
	u32 total;
	{
		std::lock_guard lock(s_prof_mutex);
		hits.swap(s_prof_hits);
		total = s_prof_total;
		s_prof_total = 0;
	}
	if (!total)
		return;
	static bool sym_init = false;
	const HANDLE proc = GetCurrentProcess();
	if (!sym_init)
	{
		sym_init = true;
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(proc, nullptr, TRUE);
	}
	static std::unordered_map<u64, std::string> names;
	std::unordered_map<std::string, u32> by_name;
	for (const auto& [rip, n] : hits)
	{
		auto it = names.find(rip);
		if (it == names.end())
		{
			alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
			SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = 255;
			DWORD64 disp = 0;
			std::string name;
			if (SymFromAddr(proc, rip, &disp, sym))
				name = sym->Name;
			else
			{
				HMODULE mod = nullptr;
				char path[MAX_PATH] = "[jit]";
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCSTR>(rip), &mod))
					GetModuleFileNameA(mod, path, MAX_PATH);
				name = path;
				if (const size_t s = name.find_last_of('\\'); s != std::string::npos)
					name = "[" + name.substr(s + 1) + "]";
			}
			it = names.emplace(rip, std::move(name)).first;
		}
		by_name[it->second] += n;
	}
	std::vector<std::pair<u32, std::string>> top;
	for (auto& [name, n] : by_name)
		top.emplace_back(n, name);
	std::sort(top.begin(), top.end(), std::greater<>());
	std::string line;
	for (size_t i = 0; i < top.size() && i < 14; i++)
		line += fmt::format("\n      {:5.1f}%  {}", top[i].first * 100.0 / total, top[i].second);
	std::fprintf(stderr, "[ps2] host profile (%u samples):%s\n", total, line.c_str());
}

static void NativeExecute()
{
	if (s_started)
	{
		// The guest never returns into the VM loop; if we get here the guest
		// has finished. Keep the VM loop from spinning.
		VMManager::SetState(VMState::Stopping);
		return;
	}
	s_started = true;
	if (ps2_cfg.trace_syscalls &&
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_prof_target,
			THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
		std::thread(ProfilerThread).detach();
	ps2rt_run(); // does not return
}

static void NativeExitExecution()
{
	// Native code cannot be unwound; pause/stop requests are honoured at the
	// runtime's safe points (see be_update).
}

static void NativeCancelInstruction() {}
static void NativeClear(u32, u32) {}

R5900cpu g_native_ee_cpu = {NativeReserve, NativeShutdown, NativeReset, NativeStep, NativeExecute,
	NativeExitExecution, NativeCancelInstruction, NativeClear};

void NativeHost::RequestQuit()
{
	s_quit.store(true);
	ps2_event_flag = 1;
}

// ---------------------------------------------------------------- memory
static inline u32 vtlb_addr(u32 a)
{
	// Uncached (0x2xxxxxxx) / accelerated (0x3xxxxxxx) views are only mapped
	// once the BIOS sets up the TLB; fold them onto the physical identity map.
	if (a >= 0x20000000u && a < 0x80000000u)
		return a & 0x1FFFFFFFu;
	return a;
}

extern "C" {

int ps2_cpcond0(void* c);
const unsigned char* ps2_get_elf_image(uint32_t* size);

// --trace: log accesses to the IPU (MPEG decoder) and its DMA channels.
static int s_io_logged;
static inline bool IoTraced(u32 a)
{
	a &= 0x1FFFFFFFu;
	static const bool gs_only = std::getenv("PS2NATIVE_IO_GS") != nullptr;
	if (gs_only)
		return a == 0x12001000u || a == 0x12001010u;
	return (a >= 0x10002000u && a < 0x10002040u) || (a >= 0x10007000u && a < 0x10007020u) ||
		   (a >= 0x1000B000u && a < 0x1000B500u) || a == 0x12001000u || a == 0x12001010u;
}
static void IoLog(const char* op, u32 a, u64 v)
{
	static u32 last_a;
	static u64 last_v;
	static const char* last_op;
	if (a == last_a && v == last_v && op == last_op)
		return; // collapse polling loops
	if (op[0] == 'r' && ((a & 0x1FFFFFFFu) == 0x10002000u || (a & 0x1FFFFFFFu) == 0x10002010u))
		return; // IPU busy polling
	last_a = a;
	last_v = v;
	last_op = op;
	if (s_io_logged < 6000)
	{
		s_io_logged++;
		std::fprintf(stderr, "[io %8.3f] %s %08X = %016llX\n", ps2_host_time(), op, a, static_cast<unsigned long long>(v));
	}
}
#define IO_TRACE(op, a, v) do { if (ps2_cfg.trace_syscalls && IoTraced(a)) IoLog(op, a, v); } while (0)

// ---------------------------------------------------------------- busy-wait skipping
// Native code polls hardware far faster than the hardware advances here: the
// EE clock follows real time and PCSX2's events only run at the runtime's
// safe points (~1 ms).  When the game spins on a busy flag (IPU command,
// running DMA channel), give the hardware its time right away by running the
// event scheduler up to the next event, like the cycles a real EE spends in
// the loop.  The EE may run ahead of real time by at most kSpinLead;
// be_update() then simply waits for the wall clock to catch up.
static constexpr s64 kSpinLead = static_cast<s64>(EE_CLOCK * 0.004); // 4 ms
static u64 s_spin_skips;
void be_update(void);

static u64 WallCycle()
{
	return s_cycle_base + static_cast<u64>((ps2_host_time() - s_time_base) * EE_CLOCK);
}

static void SpinSkip()
{
	if (static_cast<s64>(cpuRegs.cycle - WallCycle()) > kSpinLead)
	{
		be_update(); // already far ahead: let real time catch up (input, vsync work)
		return;
	}
	s64 step = static_cast<s64>(cpuRegs.nextEventCycle - cpuRegs.cycle);
	step = std::clamp<s64>(step, 32, 20000);
	cpuRegs.cycle += static_cast<u64>(step);
	if (static_cast<s64>(cpuRegs.cycle - cpuRegs.nextEventCycle) >= 0)
	{
		_cpuEventTest_Shared();
		s_events++;
	}
	s_spin_skips++;
}

// Is this read a poll of a busy hardware flag?
static inline bool BusyPoll(u32 a, u64 v)
{
	a &= 0x1FFFFFFFu;
	if (a == 0x10002004u || a == 0x10002010u) // IPU_CMD high word / IPU_CTRL: BUSY in bit 31
		return (v & 0x80000000u) != 0;
	// Dn_CHCR with STR set (sceDmaSync and friends)
	if (a >= 0x10008000u && a < 0x1000E000u && (a & 0x3FFu) == 0)
		return (v & 0x100u) != 0;
	return false;
}

uint8_t ps2_io_read8(uint32_t a)
{
	const u8 v = memRead8(vtlb_addr(a));
	IO_TRACE("r8", a, v);
	return v;
}
uint16_t ps2_io_read16(uint32_t a)
{
	const u16 v = memRead16(vtlb_addr(a));
	IO_TRACE("r16", a, v);
	return v;
}
uint32_t ps2_io_read32(uint32_t a)
{
	const u32 v = memRead32(vtlb_addr(a));
	IO_TRACE("r32", a, v);
	if (BusyPoll(a, v))
		SpinSkip();
	return v;
}
uint64_t ps2_io_read64(uint32_t a)
{
	const u64 v = memRead64(vtlb_addr(a));
	IO_TRACE("r64", a, v);
	if (((a & 0x1FFFFFFFu) == 0x10002000u && (v >> 63)) || BusyPoll(a, v))
		SpinSkip();
	return v;
}
void ps2_io_read128(uint32_t a, void* v)
{
	memRead128(vtlb_addr(a), static_cast<mem128_t*>(v));
	IO_TRACE("r128", a, *static_cast<u64*>(v));
}
void ps2_io_write8(uint32_t a, uint8_t v)
{
	if ((a & 0x1FFFFFFFu) == 0x1000F180u) // EE SIO TX: console output
	{
		std::fputc(static_cast<char>(v), stdout);
		if (v == '\n')
			std::fflush(stdout);
		return;
	}
	IO_TRACE("w8", a, v);
	memWrite8(vtlb_addr(a), v);
}
void ps2_io_write16(uint32_t a, uint16_t v)
{
	IO_TRACE("w16", a, v);
	memWrite16(vtlb_addr(a), v);
}
void ps2_io_write32(uint32_t a, uint32_t v)
{
	IO_TRACE("w32", a, v);
	memWrite32(vtlb_addr(a), v);
}
void ps2_io_write64(uint32_t a, uint64_t v)
{
	IO_TRACE("w64", a, v);
	memWrite64(vtlb_addr(a), v);
}
void ps2_io_write128(uint32_t a, const void* v)
{
	IO_TRACE("w128", a, *static_cast<const u64*>(v));
	memWrite128(vtlb_addr(a), static_cast<const mem128_t*>(v));
}

const unsigned char* ps2_get_elf_image(uint32_t* size)
{
	HRSRC res = FindResourceA(nullptr, "PS2ELF", MAKEINTRESOURCEA(10) /* RT_RCDATA */);
	if (!res)
		return nullptr;
	HGLOBAL h = LoadResource(nullptr, res);
	*size = SizeofResource(nullptr, res);
	return static_cast<const unsigned char*>(LockResource(h));
}

// ---------------------------------------------------------------- backend
void be_gs_set_imr(uint32_t v);

void be_init(void)
{
	ps2_ram = eeMem->Main;
	ps2_spr = eeMem->Scratch;
	ps2_vu0_mem = vuRegs[0].Mem;

	static const uint32_t ram_views[] = {0x00000000, 0x20000000, 0x30000000, 0x80000000, 0xA0000000};
	for (uint32_t base : ram_views)
		ps2_map_range(base, eeMem->Main, PS2_RAM_SIZE);
	ps2_map_range(0x70000000, eeMem->Scratch, Ps2MemSize::Scratch);
	// VU memory, hardware registers and the GS stay unmapped so accesses go
	// through PCSX2's handlers (micro memory writes must invalidate microVU).

	// The real kernel would leave EE interrupts enabled; here the runtime's
	// kernel HLE dispatches handlers itself, so keep PCSX2 from raising CPU
	// exceptions.
	cpuRegs.CP0.n.Status.val = 0;

	// What the BIOS would have set up.
	memWrite32(0x1000E000, 1);       // D_CTRL.DMAE
	memWrite32(0x10001810, 0x0103);  // T3 configured (kernel alarm timer)
	be_gs_set_imr(0x7F00);           // GS interrupts masked (kernel default)

	s_cycle_base = cpuRegs.cycle;
	s_time_base = ps2_host_time();
	s_last_frame = g_FrameCount;
	Console.WriteLn("(ps2native) PCSX2 backend ready: RAM %p, scratchpad %p", eeMem->Main, eeMem->Scratch);
}

int be_iop_lle(void)
{
	return NativeHost::UsesRealIOP() ? 1 : 0;
}

uint32_t be_intc_pending(void) { return psHu32(INTC_STAT) & psHu32(INTC_MASK); }
void be_intc_ack(uint32_t bits) { psHu32(INTC_STAT) &= ~bits; }

int be_intc_set_mask(int cause, int on)
{
	const u32 bit = 1u << (cause & 15);
	const int old = (psHu32(INTC_MASK) & bit) != 0;
	if (on)
		psHu32(INTC_MASK) |= bit;
	else
		psHu32(INTC_MASK) &= ~bit;
	ps2_event_flag = 1;
	return old;
}

uint32_t be_dmac_pending(void)
{
	const u32 stat = psHu32(DMAC_STAT);
	return stat & (stat >> 16) & 0x3FF;
}

void be_dmac_ack(uint32_t bits) { psHu32(DMAC_STAT) &= ~(bits & 0xFFFF); }

int be_dmac_set_mask(int ch, int on)
{
	const u32 bit = 1u << (16 + (ch & 15));
	const int old = (psHu32(DMAC_STAT) & bit) != 0;
	if (on)
		psHu32(DMAC_STAT) |= bit;
	else
		psHu32(DMAC_STAT) &= ~bit;
	ps2_event_flag = 1;
	return old;
}

int be_dmac_masked_in(int ch) { return (psHu32(DMAC_STAT) >> (16 + ch)) & 1; }

void be_dmac_raise(int ch)
{
	hwDmacIrq(ch);
	ps2_event_flag = 1;
}

int ps2_cpcond0(void* c)
{
	(void)c;
	const u32 cpc = psHu32(DMAC_PCR) & 0x3FF;
	return (psHu32(DMAC_STAT) & cpc) == cpc;
}

static void handle_vm_state()
{
	// Pause: block here (the guest is frozen at a safe point).
	while (VMManager::GetState() == VMState::Paused && !s_quit.load())
	{
		NativeHostMain::ProcessCPUThreadQueue();
		Sleep(10);
		s_time_base = ps2_host_time() - static_cast<double>(cpuRegs.cycle - s_cycle_base) / EE_CLOCK;
	}
	if (VMManager::GetState() == VMState::Stopping)
		s_quit.store(true);
}

void be_update(void)
{
	const double t_enter = Now();
	const double now = ps2_host_time();
	u64 target = s_cycle_base + static_cast<u64>((now - s_time_base) * EE_CLOCK);

	// If the guest stalled for a long time (debugger, window drag), do not
	// replay seconds of hardware events: rebase the clock instead.
	if (target > cpuRegs.cycle + static_cast<u64>(EE_CLOCK / 10))
	{
		s_lost_cycles += target - cpuRegs.cycle;
		s_cycle_base = cpuRegs.cycle;
		s_time_base = now;
		target = cpuRegs.cycle + static_cast<u64>(EE_CLOCK / 60);
	}
	// Advance the EE clock event by event, like a CPU executing up to its
	// next branch test: the IOP, counters and DMA each get their time slice
	// in order instead of one huge slice per host poll.
	for (int guard = 0; guard < 200000 && static_cast<s64>(target - cpuRegs.cycle) > 0; guard++)
	{
		const s64 to_event = static_cast<s64>(cpuRegs.nextEventCycle - cpuRegs.cycle);
		if (to_event > 0)
			cpuRegs.cycle += std::min<s64>(to_event, static_cast<s64>(target - cpuRegs.cycle));
		if (static_cast<s64>(cpuRegs.cycle - cpuRegs.nextEventCycle) >= 0)
		{
			const double t0 = Now();
			_cpuEventTest_Shared();
			s_t_events += Now() - t0;
			s_events++;
		}
	}
	NativeHostMain::ProcessCPUThreadQueue(); // input events, window resizes

	if (g_FrameCount != s_last_frame)
	{
		s_last_frame = g_FrameCount;
		NativeHostMain::ProcessCPUThreadQueue();
		handle_vm_state();
		const double t0 = Now();
		kernel_on_vblank();
		s_t_vblank += Now() - t0;
	}
	s_t_update += Now() - t_enter;
}

void be_report(void)
{
	const double emu = static_cast<double>(cpuRegs.cycle) / EE_CLOCK;
	Console.WriteLn("(ps2native) host %.1fs, emulated %.1fs, lost %.1fs, %llu event tests, frame %u",
		ps2_host_time(), emu, static_cast<double>(s_lost_cycles) / EE_CLOCK,
		static_cast<unsigned long long>(s_events), g_FrameCount);
	static double last_host, last_update, last_events, last_vblank;
	const double host = ps2_host_time(), span = host - last_host;
	auto pct = [span](double cur, double last) { return span > 0 ? (cur - last) * 100.0 / span : 0.0; };
	std::fprintf(stderr, "[ps2] host %.1fs emulated %.1fs lost %.1fs events %llu frame %u | "
		"be_update %.0f%% (event tests %.0f%%, vblank %.0f%%) spin skips %llu\n",
		host, emu, static_cast<double>(s_lost_cycles) / EE_CLOCK,
		static_cast<unsigned long long>(s_events), g_FrameCount,
		pct(s_t_update, last_update), pct(s_t_events, last_events), pct(s_t_vblank, last_vblank),
		static_cast<unsigned long long>(s_spin_skips));
	ProfilerReport();
	last_host = host;
	last_update = s_t_update;
	last_events = s_t_events;
	last_vblank = s_t_vblank;
}

double be_next_event_time(void)
{
	const s64 delta = static_cast<s64>(cpuRegs.nextEventCycle - s_cycle_base);
	return s_time_base + static_cast<double>(delta) / EE_CLOCK;
}

void be_set_crt(int interlace, int mode, int ffmd)
{
	gsIsInterlaced = interlace & 1;
	switch (mode & 0xFF)
	{
		case 3: gsSetVideoMode(GS_VideoMode::PAL); break;
		case 0x50: gsSetVideoMode(GS_VideoMode::SDTV_480P); break;
		case 0x52: gsSetVideoMode(GS_VideoMode::HDTV_720P); break;
		case 0x53: gsSetVideoMode(GS_VideoMode::HDTV_1080I); break;
		default: gsSetVideoMode(GS_VideoMode::NTSC); break;
	}
	// SMODE2 as the BIOS programs it: INT, FFMD.
	memWrite64(0x12000020, static_cast<u64>((interlace & 1) | ((ffmd & 1) << 1)));
}

// IMR is write-only on the GS: like the real kernel, keep a shadow copy for
// GsGetIMR (reading the register returns unrelated data).
static u32 s_gs_imr = 0x7F00;
uint32_t be_gs_imr(void) { return s_gs_imr; }
void be_gs_set_imr(uint32_t v)
{
	s_gs_imr = v & 0x7F00;
	memWrite64(0x12001010, s_gs_imr);
}
uint64_t be_gs_csr(void) { return memRead64(0x12001000); }
int be_should_quit(void) { return s_quit.load() ? 1 : 0; }

void be_save_frame(const char* path)
{
	NativeHostMain::SaveScreenshot(path);
}

int be_vu0_exec(vu0_state* st, uint32_t addr)
{
	// Load the runtime's macro-mode register file into PCSX2's VU0, run the
	// micro program to its E bit, and hand the registers back.  Real hardware
	// runs VU0 concurrently with the EE; COP2 accesses after VCALLMS interlock,
	// so finishing the program here is observably equivalent for those.
	VURegs& vu = vuRegs[0];
	for (int r = 1; r < 32; r++)
		std::memcpy(vu.VF[r].UL, st->vf[r], 16);
	std::memcpy(vu.ACC.UL, st->acc, 16);
	for (int r = 1; r < 16; r++)
		vu.VI[r].UL = st->vi[r] & 0xFFFF;
	vu.VI[REG_Q].UL = st->q;
	vu.VI[REG_I].UL = st->i;
	vu.VI[REG_P].UL = st->p;
	vu.VI[REG_R].UL = st->r;
	vu.VI[REG_STATUS_FLAG].UL = st->status;
	vu.VI[REG_MAC_FLAG].UL = st->mac;
	vu.VI[REG_CLIP_FLAG].UL = st->clip;

	vu0ExecMicro(addr);
	vu0Finish();

	for (int r = 0; r < 32; r++)
		std::memcpy(st->vf[r], vu.VF[r].UL, 16);
	std::memcpy(st->acc, vu.ACC.UL, 16);
	for (int r = 0; r < 16; r++)
		st->vi[r] = vu.VI[r].UL & 0xFFFF;
	st->q = vu.VI[REG_Q].UL;
	st->i = vu.VI[REG_I].UL;
	st->p = vu.VI[REG_P].UL;
	st->r = vu.VI[REG_R].UL;
	st->status = vu.VI[REG_STATUS_FLAG].UL;
	st->mac = vu.VI[REG_MAC_FLAG].UL;
	st->clip = vu.VI[REG_CLIP_FLAG].UL;
	return 1;
}

void be_shutdown(int code)
{
	(void)code;
	std::fflush(stdout);
	NativeHostMain::Shutdown();
}

} // extern "C"
