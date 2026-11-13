// SPDX-License-Identifier: GPL-3.0+
// ps2native host <-> runtime glue.
//
// The C runtime's own headers are not included here: its `u128` and `R5900`
// names collide with PCSX2's.  The runtime symbols needed by the host are
// declared with neutral types instead (C linkage ignores parameter types).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

struct WindowInfo;

extern "C" {
// runtime core
int ps2rt_parse_args(int argc, char** argv);
void ps2rt_run(void);
void ps2_map_range(uint32_t vaddr, uint8_t* host, uint32_t size);
double ps2_host_time(void);
void kernel_on_vblank(void);
extern uint8_t* ps2_ram;
extern uint8_t* ps2_spr;
extern uint8_t* ps2_vu0_mem;
extern volatile int ps2_event_flag;
extern int ps2_verbose;

// subset of struct ps2_config (runtime/src/internal.h) used by the host
struct ps2_config_view
{
	char host_root[1024];
	char cdrom_root[1024];
	char mc_root[1024];
	char elf_name[256];
	char renderer[32];
	int scale;
	int no_window;
	int max_frames;
	int trace_syscalls;
	char dump_frame[1024];
	char iso_path[1024];
	char bios_path[1024];
	char boot_path[256];
	int hle_iop;
	int reference;
	int upscale;
	int widescreen;
	int fullscreen;
	int fast_cdvd;
	int fxaa;
	int cas;
	int aniso;
	int vsync;
	int gpu;
};
extern struct ps2_config_view ps2_cfg;

// Mirrors vu0_state in runtime/src/internal.h.
struct vu0_state
{
	uint32_t vf[32][4];
	uint32_t acc[4];
	uint32_t vi[16];
	uint32_t q, i, p, r, status, mac, clip;
};
extern const char ps2_game_name[];
}

#define PS2_RAM_SIZE (32u * 1024 * 1024)

struct R5900cpu;
struct R3000Acpu;
extern R5900cpu g_native_ee_cpu;
extern R3000Acpu g_native_iop_cpu;

namespace NativeHost
{
	void RequestQuit();
	bool UsesRealIOP();
}

namespace NativeHostMain
{
	std::optional<WindowInfo> GetPlatformWindowInfo();
	void OnPresentFrame();
	void RunOnCPUThread(std::function<void()> function, bool block);
	void ProcessCPUThreadQueue();
	void SaveScreenshot(const char* path);
	void OnControllerChanged();
	void ScriptedInput(uint32_t frame);
	void Shutdown();
}
