// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team, ps2native
// SPDX-License-Identifier: GPL-3.0+
//
// ps2native host executable: boots PCSX2's VM without BIOS/disc and with the
// natively recompiled program as the EE CPU (see NativeCpu.cpp).
// Host callback implementations are taken from pcsx2-gsrunner/Main.cpp.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#include "common/RedtapeWindows.h"
#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/PrecompiledHeader.h"
#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/Config.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/Counters.h"

#include "NativeHost.h"

#include <dxgi1_6.h>
#pragma comment(lib, "dxgi.lib")

// Hybrid-graphics laptops: ask the NVIDIA/AMD drivers for the discrete GPU.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

static MemorySettingsInterface s_settings_interface;
static HWND s_hwnd = nullptr;
static DWORD s_main_thread_id;

// ------------------------------------------------------------------ CPU thread queue
static std::mutex s_queue_mutex;
static std::condition_variable s_queue_cv;
static std::deque<std::function<void()>> s_queue;
static std::atomic<u64> s_queue_done{0};
static u64 s_queue_posted = 0;

void NativeHostMain::RunOnCPUThread(std::function<void()> function, bool block)
{
	u64 ticket;
	{
		std::unique_lock lock(s_queue_mutex);
		s_queue.push_back(std::move(function));
		ticket = ++s_queue_posted;
	}
	ps2_event_flag = 1;
	if (block)
	{
		std::unique_lock lock(s_queue_mutex);
		s_queue_cv.wait(lock, [ticket]() { return s_queue_done.load() >= ticket; });
	}
}

void NativeHostMain::ProcessCPUThreadQueue()
{
	for (;;)
	{
		std::function<void()> fn;
		{
			std::unique_lock lock(s_queue_mutex);
			if (s_queue.empty())
				return;
			fn = std::move(s_queue.front());
			s_queue.pop_front();
		}
		fn();
		s_queue_done.fetch_add(1);
		s_queue_cv.notify_all();
	}
}

// ------------------------------------------------------------------ screenshots
void NativeHostMain::OnPresentFrame()
{
}

void NativeHostMain::SaveScreenshot(const char* path)
{
	// Captured by the GS at its next vsync; Shutdown() waits for the writer.
	if (MTGS::IsOpen())
		MTGS::RunOnGSThread([p = std::string(path)]() { GSQueueSnapshot(p); });
}

void NativeHostMain::Shutdown()
{
	if (MTGS::IsOpen())
	{
		MTGS::RunOnGSThread([]() { GSJoinSnapshotThreads(); });
		MTGS::WaitGS(false);
	}
	if (s_hwnd)
		PostMessageW(s_hwnd, WM_CLOSE, 0, 0);
}

// ------------------------------------------------------------------ Host callbacks
#include "_stubs.inc"

void Host::PumpMessagesOnCPUThread()
{
	NativeHostMain::ProcessCPUThreadQueue();
	if (ps2_cfg.reference && ps2_cfg.max_frames)
	{
		// Reference runs: same --frames / --dump-frame behaviour as native runs.
		static bool shot_taken = false;
		const u32 frame = g_FrameCount;
		if (ps2_cfg.dump_frame[0] && !shot_taken && frame + 3 >= static_cast<u32>(ps2_cfg.max_frames))
		{
			shot_taken = true;
			NativeHostMain::SaveScreenshot(ps2_cfg.dump_frame);
		}
		if (frame >= static_cast<u32>(ps2_cfg.max_frames))
			VMManager::SetState(VMState::Stopping);
	}
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);
	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;
		ret.replace(pos, pos + 2, count_str.view());
	}
	return ret;
}

// ------------------------------------------------------------------ configuration
static bool InitializeConfig()
{
	EmuFolders::SetAppRoot();
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
		return false;

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
	{
		Console.ErrorFmt("Hardware check failed: {}", error);
		return false;
	}

	const std::string roboto_path =
		EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
	const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
	if (roboto_data.empty())
	{
		Console.ErrorFmt("Failed to load font file '{}' (is the resources folder next to the exe?).", roboto_path);
		return false;
	}
	std::vector<ImGuiManager::FontInfo> fonts;
	ImGuiManager::FontInfo fi{};
	fi.data = roboto_data;
	fi.exclude_ranges = {};
	fi.face_name = nullptr;
	fi.is_emoji_font = false;
	fonts.push_back(fi);
	ImGuiManager::SetFonts(std::move(fonts));

	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);
	VMManager::SetDefaultSettings(si, true, true, true, true, true);
	VMManager::Internal::LoadStartupSettings();
	return true;
}

static GSRendererType ParseRenderer(const char* name)
{
	const std::string n = StringUtil::toLower(name);
	if (n == "vulkan" || n == "vk")
		return GSRendererType::VK;
	if (n == "dx11" || n == "d3d11")
		return GSRendererType::DX11;
	if (n == "dx12" || n == "d3d12")
		return GSRendererType::DX12;
	if (n == "opengl" || n == "gl")
		return GSRendererType::OGL;
	if (n == "software" || n == "sw")
		return GSRendererType::SW;
	if (n == "null")
		return GSRendererType::Null;
	return GSRendererType::Auto;
}

// ------------------------------------------------------------------ BIOS / real IOP
static bool s_lle = false;          // real IOP from the BIOS (audio, CD/DVD, pad, memcards)
static std::string s_bios_dir, s_bios_file;

static std::string PathToUTF8(const std::filesystem::path& p)
{
	const auto u8 = p.u8string();
	return std::string(u8.begin(), u8.end());
}

// Accepts a BIOS file or a folder containing one (a 0.5-8 MiB image).
static bool ResolveBios()
{
	namespace fs = std::filesystem;
	std::vector<std::string> candidates;
	if (ps2_cfg.bios_path[0])
		candidates.push_back(ps2_cfg.bios_path);
	candidates.push_back(std::string(ps2_cfg.host_root) + "\\bios");
	for (const std::string& c : candidates)
	{
		std::error_code ec;
		const fs::path p = fs::u8path(c);
		if (fs::is_regular_file(p, ec))
		{
			s_bios_dir = PathToUTF8(p.parent_path());
			s_bios_file = PathToUTF8(p.filename());
			return true;
		}
		if (fs::is_directory(p, ec))
		{
			for (const auto& e : fs::directory_iterator(p, ec))
			{
				const auto size = e.is_regular_file(ec) ? e.file_size(ec) : 0;
				if (size >= 512 * 1024 && size <= 8 * 1024 * 1024)
				{
					s_bios_dir = PathToUTF8(p);
					s_bios_file = PathToUTF8(e.path().filename());
					return true;
				}
			}
		}
	}
	return false;
}

bool NativeHost::UsesRealIOP()
{
	return s_lle;
}

// ps2native.ini next to the exe (also written by the launcher).
static std::string IniValue(const char* key, const char* def = "")
{
	static std::vector<std::pair<std::string, std::string>> s_ini;
	static bool s_loaded = false;
	if (!s_loaded)
	{
		s_loaded = true;
		if (std::FILE* f = FileSystem::OpenCFile(fmt::format("{}\\ps2native.ini", ps2_cfg.host_root).c_str(), "r"))
		{
			char line[1200];
			while (std::fgets(line, sizeof(line), f))
			{
				std::string_view l(line);
				const size_t eq = l.find('=');
				if (eq == std::string_view::npos || l[0] == '#' || l[0] == ';')
					continue;
				s_ini.emplace_back(StringUtil::StripWhitespace(l.substr(0, eq)), StringUtil::StripWhitespace(l.substr(eq + 1)));
			}
			std::fclose(f);
		}
	}
	for (const auto& [k, v] : s_ini)
		if (k == key)
			return v;
	return def;
}

// Keyboard layout for pad 1; each button can be rebound with key_<Button>=<Key>.
static void SetKeyboardPadBindings(SettingsInterface& si)
{
	static const std::pair<const char*, const char*> binds[] = {
		{"Up", "Up"}, {"Down", "Down"}, {"Left", "Left"}, {"Right", "Right"},
		{"Start", "Return"}, {"Select", "Backspace"},
		{"Triangle", "W"}, {"Circle", "D"}, {"Cross", "S"}, {"Square", "A"},
		{"L1", "1"}, {"R1", "3"}, {"L2", "Q"}, {"R2", "E"},
		{"L3", "Z"}, {"R3", "C"},
		{"LUp", "I"}, {"LDown", "K"}, {"LLeft", "J"}, {"LRight", "L"},
		{"RUp", "Numpad8"}, {"RDown", "Numpad2"}, {"RLeft", "Numpad4"}, {"RRight", "Numpad6"},
	};
	for (const auto& [button, def] : binds)
	{
		const std::string key = IniValue(fmt::format("key_{}", button).c_str(), def);
		if (!key.empty())
			si.AddToStringList("Pad1", button, fmt::format("Keyboard/{}", key).c_str());
	}
	// Rumble strength for controllers (vibration=0 turns it off).
	const float rumble = IniValue("vibration", "1") == "0" ? 0.0f : 1.0f;
	si.SetFloatValue("Pad1", "LargeMotorScale", rumble);
	si.SetFloatValue("Pad1", "SmallMotorScale", rumble);
}

// Name of the high-performance GPU as PCSX2 lists adapters (DXGI description).
static std::string HighPerformanceAdapterName()
{
	IDXGIFactory6* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return {};
	std::string name;
	IDXGIAdapter1* adapter = nullptr;
	if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))))
	{
		DXGI_ADAPTER_DESC1 desc;
		if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
			name = StringUtil::WideStringToUTF8String(desc.Description);
		adapter->Release();
	}
	factory->Release();
	return name;
}

// The first game controller (XInput, DualShock 4, DualSense, Switch Pro, ...
// through SDL) drives pad 1 together with the keyboard, including rumble.
static bool MapFirstController()
{
	if (IniValue("controller", "1") == "0") // keyboard only
		return false;
	for (const auto& [identifier, name] : InputManager::EnumerateDevices())
	{
		if (!identifier.starts_with("SDL-"))
			continue;
		const auto mapping = InputManager::GetGenericBindingMapping(identifier);
		if (mapping.empty())
			continue;
		Pad::ClearPortBindings(s_settings_interface, 0);
		Pad::MapController(s_settings_interface, 0, mapping);
		SetKeyboardPadBindings(s_settings_interface);
		Console.WriteLnFmt("(ps2native) Controller: {} ({})", name, identifier);
		return true;
	}
	return false;
}

void NativeHostMain::OnControllerChanged()
{
	// Called from inside InputManager while input sources are (re)created, so
	// the device list may not be usable yet: remap later on the CPU thread.
	// Start-up mapping is done in CPUThreadMain.
	if (!s_lle || !VMManager::HasValidVM())
		return;
	NativeHostMain::RunOnCPUThread([]() {
		if (!VMManager::HasValidVM())
			return;
		Pad::ClearPortBindings(s_settings_interface, 0);
		SetKeyboardPadBindings(s_settings_interface);
		MapFirstController();
		VMManager::ReloadInputBindings(true);
	}, false);
}

static void SettingsOverride()
{
	MemorySettingsInterface& si = s_settings_interface;
	// Headless runs default to the null renderer unless one was requested
	// explicitly (e.g. for screenshot tests).
	GSRendererType renderer = ParseRenderer(ps2_cfg.renderer);
	if (ps2_cfg.no_window && renderer == GSRendererType::Auto)
		renderer = GSRendererType::Null;
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(renderer));

	// Pacing comes from the runtime's real-time EE clock.
	si.SetBoolValue("EmuCore/GS", "FrameLimitEnable", ps2_cfg.reference != 0);
	si.SetIntValue("EmuCore/GS", "VsyncEnable", ps2_cfg.vsync != 0);
	si.ClearSection("Hotkeys");

	// Presentation: internal resolution, aspect ratio, filtering.
	si.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(std::clamp(ps2_cfg.upscale, 1, 8)));
	si.SetStringValue("EmuCore/GS", "AspectRatio", ps2_cfg.widescreen ? "16:9" : "4:3");
	si.SetBoolValue("EmuCore/GS", "fxaa", ps2_cfg.fxaa != 0);
	si.SetIntValue("EmuCore/GS", "CASMode", ps2_cfg.cas > 0 ? static_cast<int>(GSCASMode::SharpenOnly) : 0);
	si.SetIntValue("EmuCore/GS", "CASSharpness", std::clamp(ps2_cfg.cas, 0, 100));
	si.SetIntValue("EmuCore/GS", "MaxAnisotropy", std::clamp(ps2_cfg.aniso, 0, 16));
	si.SetBoolValue("EmuCore/Speedhacks", "fastCDVD", ps2_cfg.fast_cdvd != 0);
	if (ps2_cfg.gpu)
	{
		const std::string adapter = HighPerformanceAdapterName();
		if (!adapter.empty())
		{
			Console.WriteLnFmt("(ps2native) GPU: {}", adapter);
			si.SetStringValue("EmuCore/GS", "Adapter", adapter.c_str());
		}
	}

	if (s_lle)
	{
		// Real IOP: PCSX2's CD/DVD, SPU2 audio, SIO2 pads and memory cards.
		si.SetStringValue("Folders", "Bios", s_bios_dir.c_str());
		si.SetStringValue("Filenames", "BIOS", s_bios_file.c_str());
		si.SetStringValue("Folders", "MemoryCards", ps2_cfg.mc_root);
		// Folder settings are only read at startup; set the live values too.
		EmuFolders::Bios = s_bios_dir;
		EmuFolders::MemoryCards = ps2_cfg.mc_root;
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::u8path(ps2_cfg.mc_root), ec);
		si.SetBoolValue("MemoryCards", "Slot1_Enable", true);
		si.SetStringValue("MemoryCards", "Slot1_Filename", "Mcd001.ps2");
		si.SetBoolValue("MemoryCards", "Slot2_Enable", false);
		Pad::ClearPortBindings(si, 0);
		SetKeyboardPadBindings(si);
		if (ps2_cfg.no_window)
			si.SetStringValue("SPU2/Output", "OutputModule", "nullout");
	}
	else
	{
		// HLE IOP: no SPU2 audio stream, PCSX2 pads or memory cards.
		si.SetStringValue("SPU2/Output", "OutputModule", "nullout");
		si.SetBoolValue("InputSources", "SDL", false);
		si.SetBoolValue("InputSources", "XInput", false);
		Pad::ClearPortBindings(si, 0);
		for (u32 i = 0; i < 2; i++)
		{
			si.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
			si.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
		}
	}

	si.SetBoolValue("Logging", "EnableSystemConsole", true);
	si.SetBoolValue("Logging", "EnableVerbose", ps2_verbose != 0);
	si.SetBoolValue("Logging", "EnableIOPConsole", ps2_verbose != 0);
	si.SetBoolValue("EmuCore/GS", "OsdShowFPS", false);
	si.SetIntValue("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
	si.SetBoolValue("UI", "StartFullscreen", false);
	si.SetBoolValue("EmuCore", "EnableDiscordPresence", false);
}

// ------------------------------------------------------------------ window
static bool s_fullscreen = false;
static RECT s_windowed_rect = {};

static void SetFullscreen(bool on)
{
	if (!s_hwnd || on == s_fullscreen)
		return;
	s_fullscreen = on;
	if (on)
	{
		GetWindowRect(s_hwnd, &s_windowed_rect);
		MONITORINFO mi = {sizeof(mi)};
		GetMonitorInfoW(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
		SetWindowLongPtrW(s_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(s_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
			mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		ShowCursor(FALSE);
	}
	else
	{
		SetWindowLongPtrW(s_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowPos(s_hwnd, nullptr, s_windowed_rect.left, s_windowed_rect.top,
			s_windowed_rect.right - s_windowed_rect.left, s_windowed_rect.bottom - s_windowed_rect.top,
			SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
		ShowCursor(TRUE);
	}
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_CLOSE:
			NativeHost::RequestQuit();
			return 0;
		case WM_SIZE:
			if (MTGS::IsOpen())
				NativeHostMain::RunOnCPUThread([]() { MTGS::UpdateDisplayWindow(); }, false);
			break;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYUP:
		{
			if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (lParam & (1 << 30)))
				return 0; // auto-repeat
			// Alt+Enter / F11: toggle borderless fullscreen.
			if ((msg == WM_SYSKEYDOWN && wParam == VK_RETURN && (lParam & (1 << 29))) ||
				(msg == WM_KEYDOWN && wParam == VK_F11))
			{
				SetFullscreen(!s_fullscreen);
				return 0;
			}
			const u32 vk = static_cast<u32>(wParam);
			const float value = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? 1.0f : 0.0f;
			NativeHostMain::RunOnCPUThread(
				[vk, value]() { InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(vk), value); }, false);
			return 0;
		}
		default:
			break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool CreatePlatformWindow()
{
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = L"ps2native";
	if (!RegisterClassExW(&wc))
		return false;

	// Default: a 720p window in the output aspect ratio.
	const int height = ps2_cfg.scale > 1 ? 448 * ps2_cfg.scale : 720;
	const int width = ps2_cfg.widescreen ? height * 16 / 9 : height * 4 / 3;
	RECT rc = {0, 0, width, height};
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	const std::wstring title = StringUtil::UTF8StringToWideString(fmt::format("{} - ps2native", ps2_game_name));
	s_hwnd = CreateWindowExW(0, L"ps2native", title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!s_hwnd)
		return false;
	ShowWindow(s_hwnd, SW_SHOW);
	UpdateWindow(s_hwnd);
	if (ps2_cfg.fullscreen)
		SetFullscreen(true);
	return true;
}

std::optional<WindowInfo> NativeHostMain::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_hwnd)
	{
		RECT rc = {};
		GetClientRect(s_hwnd, &rc);
		wi.surface_width = static_cast<u32>(rc.right - rc.left);
		wi.surface_height = static_cast<u32>(rc.bottom - rc.top);
		wi.surface_scale = 1.0f;
		wi.type = WindowInfo::Type::Win32;
		wi.window_handle = s_hwnd;
	}
	else
	{
		wi.type = WindowInfo::Type::Surfaceless;
	}
	return wi;
}

static constexpr UINT SHUTDOWN_MSG = WM_APP + 0x100;

// ------------------------------------------------------------------ CPU thread
static void CPUThreadMain(std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);
	if (VMManager::Internal::CPUThreadInitialize())
	{
		if (s_lle)
		{
			InputManager::ReloadDevices();
			MapFirstController();
		}
		VMManager::ApplySettings();
		if (!ps2_cfg.reference)
			VMManager::SetNativeHostCPUs(&g_native_ee_cpu, s_lle ? nullptr : &g_native_iop_cpu);
		else
			Console.WriteLn("(ps2native) reference mode: the disc runs fully emulated by PCSX2.");

		VMBootParameters params;
		if (ps2_cfg.iso_path[0])
		{
			params.filename = ps2_cfg.iso_path;
			params.source_type = CDVD_SourceType::Iso;
		}
		else
		{
			params.source_type = CDVD_SourceType::NoDisc;
		}
		params.fast_boot = ps2_cfg.reference != 0;
		Error error;
		if (VMManager::Initialize(params, &error) == VMBootResult::StartupSuccess)
		{
			VMManager::SetState(VMState::Running);
			while (VMManager::GetState() == VMState::Running)
				VMManager::Execute(); // runs the guest; returns only when it stops
			VMManager::Shutdown(false);
			ret->store(EXIT_SUCCESS);
		}
		else
		{
			Console.ErrorFmt("VM initialisation failed: {}", error.GetDescription());
		}
	}
	VMManager::Internal::CPUThreadShutdown();
	PostThreadMessageW(s_main_thread_id, SHUTDOWN_MSG, 0, 0);
}

int wmain(int argc, wchar_t** argv)
{
	std::vector<std::string> u8_args;
	for (int i = 0; i < argc; i++)
		u8_args.push_back(StringUtil::WideStringToUTF8String(argv[i]));
	std::vector<char*> u8_argptrs;
	for (auto& a : u8_args)
		u8_argptrs.push_back(a.data());
	u8_argptrs.push_back(nullptr);
	s_main_thread_id = GetCurrentThreadId();

	const int rc = ps2rt_parse_args(argc, u8_argptrs.data());
	if (rc)
		return rc == -1 ? 0 : rc;

	CrashHandler::Install();
	s_lle = ResolveBios();
	if (ps2_cfg.iso_path[0] && !s_lle)
		Console.Warning("(ps2native) A disc image was given but no BIOS was found: running with the HLE IOP.");
	if (!InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		return EXIT_FAILURE;
	}
	if (!ps2_cfg.no_window && !CreatePlatformWindow())
	{
		Console.Error("Failed to create window.");
		return EXIT_FAILURE;
	}
	SettingsOverride();

	std::atomic<int> thread_ret;
	std::thread cputhread(CPUThreadMain, &thread_ret);
	MSG msg;
	for (;;)
	{
		if (!GetMessageW(&msg, nullptr, 0, 0) || msg.message == SHUTDOWN_MSG)
			break;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	cputhread.join();
	if (s_hwnd)
		DestroyWindow(s_hwnd);
	return thread_ret.load();
}
