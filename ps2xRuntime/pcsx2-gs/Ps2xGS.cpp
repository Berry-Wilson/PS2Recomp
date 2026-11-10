// ps2x-gs: PCSX2's GS (the PS2 graphics synthesizer, drawn by the video card) as a DLL for PS2Recomp
// ports. Modelled on sor-gs from Shadow of Rome Recompiled (korvonordico, GPL-3.0+).
//
// The host side (Host:: stubs, settings) comes from pcsx2-gsrunner/Main.cpp, included below with its
// entry points renamed. The runtime loads this DLL and talks to it only through the plain C functions
// in ps2x_gs_api.h.
//
// Like PCSX2's own MTGS, the GS runs on a thread of its own, which owns the video card device. The
// runtime's threads only copy what they send (GIF packets, the privileged registers at each vsync)
// into a frame; the GS thread replays the frames in order. A sender waits only when it is more than
// three frames ahead.
// SPDX-License-Identifier: GPL-3.0+

// The runner's BeginPresentFrame (frame dumps for its own tests) is renamed out of the way: this
// DLL defines Host::BeginPresentFrame itself, below, to draw the runtime's overlay text.
#define wmain ps2xgs_unused_wmain
#define main ps2xgs_unused_main
#define BeginPresentFrame ps2xgs_runner_BeginPresentFrame
#include "pcsx2-gsrunner/Main.cpp" // from the PCSX2 source root, on the include path
#undef BeginPresentFrame
#undef main
#undef wmain

#include "imgui.h"

#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/Renderers/Common/GSRenderer.h"
#include "pcsx2/GameDatabase.h"
#include "pcsx2/Memory.h"

#include "ps2x_gs_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	enum : u32
	{
		REC_GIF1 = 1, // a GIF packet on PATH1
		REC_GIF2 = 2, // PATH2
		REC_GIF3 = 3, // PATH3
		REC_SNAP = 4, // path of a PNG to save, with its NUL
		REC_UPSCALE = 5, // a float: the new internal resolution multiplier
	};

	struct Frame
	{
		std::vector<u8> data; // records: [u32 kind][u32 bytes][8 zero bytes][payload, padded to 16 bytes]
		u8 regs[0x2000];      // the privileged registers at this vsync
		u32 field = 0;
		bool vsync = false;
		bool quit = false;
	};

	// Sender side, under s_in_lock.
	std::mutex s_in_lock;
	std::vector<u8> s_cur;
	alignas(64) u8 s_regs_in[0x2000] = {};
	bool s_open = false;
	bool s_sys_memory = false; // SysMemory::Allocate() has run

	// Between the two, under s_q_lock.
	std::mutex s_q_lock;
	std::condition_variable s_q_data, s_q_space, s_q_opened;
	std::deque<std::unique_ptr<Frame>> s_queue;
	std::vector<std::vector<u8>> s_spare;
	int s_open_result = 0; // 0 waiting, 1 open, -1 failed
	std::thread *s_gs_thread = nullptr;

	// GS thread side.
	alignas(64) u8 s_regs[0x2000] = {}; // the GS privileged registers, as at 0x12000000 on the PS2
	alignas(16) u8 s_path1[0x4000] = {}; // PATH1 packets are read from VU1 memory; this stands in for it
	GSRendererType s_type = GSRendererType::Auto;
	HWND s_parent = nullptr;
	HANDLE s_ui_ready = nullptr;
	int s_last_w = 0, s_last_h = 0;
	constexpr UINT WM_PS2XGS_QUIT = WM_APP + 0x200;
	u64 s_dropped = 0;

	LRESULT CALLBACK GsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == WM_CLOSE)
			return 0; // the runtime closes it, not the user
		if (msg == WM_PS2XGS_QUIT)
		{
			DestroyWindow(hwnd);
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	// The window lives on its own thread, which only pumps its messages. As a disabled child of the
	// game's window it never takes the keyboard or mouse, so the game's window keeps all input.
	DWORD WINAPI UiThread(LPVOID)
	{
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = GsWndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.lpszClassName = L"Ps2xGsWindow";
		RegisterClassExW(&wc);
		const DWORD ex = WS_EX_NOPARENTNOTIFY | (s_parent ? 0 : WS_EX_NOACTIVATE);
		const DWORD style = s_parent ? (WS_CHILD | WS_VISIBLE | WS_DISABLED) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		RECT rc = {0, 0, 960, 720};
		if (s_parent)
			GetClientRect(s_parent, &rc);
		else
			AdjustWindowRectEx(&rc, style, FALSE, ex);
		s_hwnd = CreateWindowExW(ex, wc.lpszClassName, L"PCSX2 GS", style, s_parent ? 0 : CW_USEDEFAULT,
			s_parent ? 0 : CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, s_parent, nullptr, wc.hInstance, nullptr);
		SetEvent(s_ui_ready);
		if (!s_hwnd)
			return 1;
		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		return 0;
	}

	bool InitializeHost(const Ps2xGsConfig &config)
	{
		EmuFolders::SetAppRoot();
		if (config.resources_dir && config.resources_dir[0])
			EmuFolders::Resources = config.resources_dir;
		else if (!EmuFolders::SetResourcesDirectory())
			return false;
		// Everything PCSX2 would write (shader cache, logs) goes to the given folder, never to the
		// player's Documents\PCSX2.
		EmuFolders::DataRoot = (config.data_dir && config.data_dir[0]) ? std::string(config.data_dir) :
		                                                                    Path::Combine(EmuFolders::AppRoot, "gs-data");
		FileSystem::CreateDirectoryPath(EmuFolders::DataRoot.c_str(), false);
		CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

		const char *error;
		if (!VMManager::PerformEarlyHardwareChecks(&error))
		{
			std::fprintf(stderr, "[ps2x-gs] %s\n", error);
			return false;
		}

		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (!roboto_data.empty())
		{
			std::vector<ImGuiManager::FontInfo> fonts;
			ImGuiManager::FontInfo fi{};
			fi.data = roboto_data;
			fi.exclude_ranges = {};
			fi.face_name = nullptr;
			fi.is_emoji_font = false;
			fonts.push_back(fi);
			ImGuiManager::SetFonts(std::move(fonts));
		}

		MemorySettingsInterface &si = s_settings_interface;
		Host::Internal::SetBaseSettingsLayer(&si);
		VMManager::SetDefaultSettings(si, true, true, true, true, true);
		VMManager::Internal::LoadStartupSettings();
		return true;
	}

	void ResizeIfNeeded()
	{
		if (s_parent)
		{
			RECT rc = {};
			GetClientRect(s_parent, &rc);
			if (rc.right != s_last_w || rc.bottom != s_last_h)
				SetWindowPos(s_hwnd, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
		}
		RECT cr = {};
		GetClientRect(s_hwnd, &cr);
		if (cr.right > 0 && cr.bottom > 0 && (cr.right != s_last_w || cr.bottom != s_last_h))
		{
			s_last_w = cr.right;
			s_last_h = cr.bottom;
			GSResizeDisplayWindow(cr.right, cr.bottom, 1.0f);
		}
	}

	void GsThread(bool vsync)
	{
		std::memset(s_regs, 0, sizeof(s_regs));
		const bool ok = GSopen(EmuConfig.GS, s_type, s_regs, vsync ? GSVSyncMode::FIFO : GSVSyncMode::Disabled, false);
		{
			std::lock_guard<std::mutex> lk(s_q_lock);
			s_open_result = ok ? 1 : -1;
		}
		s_q_opened.notify_all();
		if (!ok)
			return;

		for (;;)
		{
			std::unique_ptr<Frame> f;
			{
				std::unique_lock<std::mutex> lk(s_q_lock);
				s_q_data.wait(lk, [] { return !s_queue.empty(); });
				f = std::move(s_queue.front());
				s_queue.pop_front();
			}
			s_q_space.notify_all();
			if (f->quit)
				break;

			const u8 *p = f->data.data();
			const u8 *const end = p + f->data.size();
			while (p + 16 <= end)
			{
				u32 kind, bytes;
				std::memcpy(&kind, p, 4);
				std::memcpy(&bytes, p + 4, 4);
				p += 16;
				const size_t padded = (static_cast<size_t>(bytes) + 15) & ~static_cast<size_t>(15);
				if (padded > static_cast<size_t>(end - p))
					break;
				switch (kind)
				{
					case REC_GIF1:
						// PATH1 is read from VU1 memory up to the end of the packet. Packets longer than
						// VU1 memory cannot come from it, so they are sent as PATH3.
						if (bytes <= sizeof(s_path1))
						{
							std::memcpy(s_path1, p, bytes);
							std::memset(s_path1 + bytes, 0, sizeof(s_path1) - bytes);
							GSgifTransfer1(s_path1, 0);
						}
						else
							GSgifTransfer3(const_cast<u8 *>(p), bytes / 16);
						break;
					case REC_GIF2:
						GSgifTransfer2(const_cast<u8 *>(p), bytes / 16);
						break;
					case REC_GIF3:
						GSgifTransfer3(const_cast<u8 *>(p), bytes / 16);
						break;
					case REC_SNAP:
						GSQueueSnapshot(std::string(reinterpret_cast<const char *>(p)), 0);
						break;
					case REC_UPSCALE:
					{
						float upscale;
						std::memcpy(&upscale, p, sizeof(upscale));
						Pcsx2Config::GSOptions options = EmuConfig.GS;
						options.UpscaleMultiplier = upscale;
						GSUpdateConfig(options);
						EmuConfig.GS = options;
						std::fprintf(stderr, "[ps2x-gs] internal resolution %gx\n", upscale);
						break;
					}
				}
				p += padded;
			}
			if (f->vsync)
			{
				std::memcpy(s_regs, f->regs, sizeof(s_regs));
				ResizeIfNeeded();
				GSvsync(f->field, true);
			}
			f->data.clear();
			std::lock_guard<std::mutex> lk(s_q_lock);
			if (s_spare.size() < 4)
				s_spare.push_back(std::move(f->data));
		}
		GSclose();
	}

	// The runtime's overlay lines (ps2xgs_set_overlay), drawn on the GS thread at each present.
	constexpr unsigned kOverlayLines = 4;
	std::mutex s_overlay_lock;
	std::string s_overlay_text[kOverlayLines];
	u32 s_overlay_rgba[kOverlayLines] = {};

	// Same place and look as the runtime's own overlay: 20 px text from (8, 8), a line every 24 px,
	// with a black shadow one pixel down and right. Scaled with PCSX2's UI scale.
	void DrawOverlay()
	{
		std::lock_guard<std::mutex> lk(s_overlay_lock);
		if (!ImGui::GetCurrentContext())
			return;
		const float scale = std::max(ImGuiManager::GetGlobalScale(), 1.0f);
		ImDrawList *dl = ImGui::GetForegroundDrawList();
		ImFont *font = ImGui::GetFont();
		float y = 8.0f * scale;
		for (unsigned i = 0; i < kOverlayLines; ++i)
		{
			if (s_overlay_text[i].empty())
				continue;
			const char *text = s_overlay_text[i].c_str();
			dl->AddText(font, 20.0f * scale, ImVec2(8.0f * scale + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, 255), text);
			dl->AddText(font, 20.0f * scale, ImVec2(8.0f * scale, y), s_overlay_rgba[i], text);
			y += 24.0f * scale;
		}
	}

	void AppendRecord(u32 kind, const void *src, u32 bytes)
	{
		const size_t n = s_cur.size();
		const size_t padded = (static_cast<size_t>(bytes) + 15) & ~static_cast<size_t>(15);
		s_cur.resize(n + 16 + padded);
		u8 *d = s_cur.data() + n;
		std::memcpy(d, &kind, 4);
		std::memcpy(d + 4, &bytes, 4);
		std::memset(d + 8, 0, 8);
		std::memcpy(d + 16, src, bytes);
		if (padded > bytes)
			std::memset(d + 16 + bytes, 0, padded - bytes);
	}

	void Push(std::unique_ptr<Frame> f)
	{
		{
			std::unique_lock<std::mutex> lk(s_q_lock);
			// More than 3 frames ahead of the video card: wait, but never forever.
			if (s_queue.size() >= 3 && !s_q_space.wait_for(lk, std::chrono::seconds(2), [] { return s_queue.size() < 3; }))
			{
				if (s_dropped++ < 4)
					std::fprintf(stderr, "[ps2x-gs] the GS thread is not keeping up: frame dropped\n");
				return;
			}
			s_queue.push_back(std::move(f));
		}
		s_q_data.notify_one();
	}

	// An integer environment variable in [lo, hi]; false when unset or out of range.
	bool EnvInt(const char *name, int lo, int hi, int *out)
	{
		const char *value = std::getenv(name);
		if (!value || !value[0])
			return false;
		char *end = nullptr;
		const long v = std::strtol(value, &end, 10);
		if (end == value || *end != '\0' || v < lo || v > hi)
		{
			std::fprintf(stderr, "[ps2x-gs] ignoring %s=%s (expected %d-%d)\n", name, value, lo, hi);
			return false;
		}
		*out = static_cast<int>(v);
		return true;
	}

	// PS2X_PCSX2_* renderer switches. They are applied after the game database's fixes, so a switch
	// that is set wins over the database; one that is unset leaves PCSX2's value (or the database's).
	void ApplyRendererSwitches(const GameDatabaseSchema::GameEntry *game)
	{
		Pcsx2Config::GSOptions &gs = EmuConfig.GS;
		int v;

		// Blending accuracy, 0 minimum - 5 maximum. Unset: the level the game database recommends,
		// which PCSX2 itself only suggests in a message.
		if (EnvInt("PS2X_PCSX2_BLENDING", 0, 5, &v))
			gs.AccurateBlendingUnit = static_cast<AccBlendLevel>(v);
		else if (game)
		{
			for (const auto &[id, value] : game->gsHWFixes)
			{
				if (id == GameDatabaseSchema::GSHWFixId::RecommendedBlendingLevel && value > static_cast<int>(gs.AccurateBlendingUnit) &&
					value <= static_cast<int>(AccBlendLevel::Maximum))
					gs.AccurateBlendingUnit = static_cast<AccBlendLevel>(value);
			}
		}
		// Texture filtering: 0 nearest, 1 bilinear forced, 2 as the PS2 asks, 3 forced except sprites.
		if (EnvInt("PS2X_PCSX2_TEXTURE_FILTER", 0, 3, &v))
			gs.TextureFiltering = static_cast<BiFiltering>(v);
		// Trilinear filtering: 0 off, 1 as the PS2 asks, 2 forced.
		if (EnvInt("PS2X_PCSX2_TRILINEAR", 0, 2, &v))
			gs.TriFilter = static_cast<TriFiltering>(v);
		// Mipmapping as the game sets it up (1, PCSX2's default) or off (0).
		if (EnvInt("PS2X_PCSX2_MIPMAP", 0, 1, &v))
			gs.HWMipmap = v != 0;
		// Dithering: 0 off, 1 scaled to the internal resolution, 2 unscaled (default), 3 forced 32-bit.
		if (EnvInt("PS2X_PCSX2_DITHERING", 0, 3, &v))
			gs.Dithering = static_cast<u8>(v);
		// Upscaling fixes (only used above 1x): see GSHalfPixelOffset and GSNativeScaling.
		if (EnvInt("PS2X_PCSX2_HALF_PIXEL", 0, static_cast<int>(GSHalfPixelOffset::MaxCount) - 1, &v))
			gs.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(v);
		if (EnvInt("PS2X_PCSX2_ROUND_SPRITE", 0, 2, &v))
			gs.UserHacks_RoundSprite = static_cast<s8>(v);
		if (EnvInt("PS2X_PCSX2_NATIVE_SCALING", 0, static_cast<int>(GSNativeScaling::MaxCount) - 1, &v))
			gs.UserHacks_NativeScaling = static_cast<GSNativeScaling>(v);
		if (EnvInt("PS2X_PCSX2_ALIGN_SPRITE", 0, 1, &v))
			gs.UserHacks_AlignSpriteX = v != 0;
		if (EnvInt("PS2X_PCSX2_MERGE_SPRITE", 0, 1, &v))
			gs.UserHacks_MergePPSprite = v != 0;

		std::fprintf(stderr,
			"[ps2x-gs] blending %d, filter %d, trilinear %d, mipmap %d, dithering %d, half-pixel %d, round sprite %d, "
			"native scaling %d, align sprite %d, merge sprite %d\n",
			static_cast<int>(gs.AccurateBlendingUnit), static_cast<int>(gs.TextureFiltering), static_cast<int>(gs.TriFilter),
			gs.HWMipmap ? 1 : 0, static_cast<int>(gs.Dithering), static_cast<int>(gs.UserHacks_HalfPixelOffset),
			static_cast<int>(gs.UserHacks_RoundSprite), static_cast<int>(gs.UserHacks_NativeScaling),
			gs.UserHacks_AlignSpriteX ? 1 : 0, gs.UserHacks_MergePPSprite ? 1 : 0);
	}

	void ApplySettings(const Ps2xGsConfig &config)
	{
		MemorySettingsInterface &si = s_settings_interface;
		GSRunner::SettingsOverride();
		si.SetBoolValue("Logging", "EnableSystemConsole", false);
		si.SetBoolValue("Logging", "EnableVerbose", false);
		if (const char *lg = std::getenv("PS2X_PCSX2_LOG"); lg && lg[0] == '1')
			Log::SetFileOutputLevel(LOGLEVEL_DEBUG, Path::Combine(EmuFolders::DataRoot, "logs" FS_OSPATH_SEPARATOR_STR "ps2x-gs.log"));

		const bool osd = config.osd != 0;
		si.SetBoolValue("EmuCore/GS", "OsdShowFPS", osd);
		si.SetBoolValue("EmuCore/GS", "OsdShowResolution", osd);
		si.SetBoolValue("EmuCore/GS", "OsdShowGSStats", osd);
		si.SetBoolValue("EmuCore/GS", "OsdShowFrameTimes", osd);
		si.SetBoolValue("EmuCore/GS", "OsdShowGPU", osd);
		si.SetIntValue("EmuCore/GS", "OsdMessagesPos", static_cast<int>(osd ? OsdOverlayPos::TopLeft : OsdOverlayPos::None));
		// Screenshots at the resolution the GS draws at, not the window's.
		si.SetIntValue("EmuCore/GS", "ScreenshotSize", static_cast<int>(GSScreenshotSize::InternalResolution));
		si.SetFloatValue("EmuCore/GS", "upscale_multiplier", config.upscale < 1.0f ? 1.0f : config.upscale);
		// The runtime shows a progressive picture; PCSX2's deinterlacer would only blur it.
		si.SetIntValue("EmuCore/GS", "deinterlace_mode", static_cast<int>(GSInterlaceMode::Off));
		if (config.sharpness == 1 || config.sharpness == 2)
		{
			const bool high = config.sharpness == 2;
			si.SetIntValue("EmuCore/GS", "MaxAnisotropy", high ? 16 : 8);
			si.SetIntValue("EmuCore/GS", "CASMode", static_cast<int>(GSCASMode::SharpenOnly));
			si.SetIntValue("EmuCore/GS", "CASSharpness", high ? 80 : 40);
		}
		if (config.widescreen)
		{
			si.SetStringValue("EmuCore/GS", "AspectRatio", "16:9");
			si.SetStringValue("EmuCore/GS", "FMVAspectRatioSwitch", "Off");
		}
		{
			SettingsLoadWrapper slw(si);
			EmuConfig.LoadSave(slw);
		}
		// PCSX2's per-game fixes (GameIndex.yaml), the ones it applies when it boots the game itself.
		const GameDatabaseSchema::GameEntry *game = nullptr;
		if (config.serial && config.serial[0])
		{
			GameDatabase::ensureLoaded();
			game = GameDatabase::findGame(config.serial);
			if (game)
			{
				game->applyGSHardwareFixes(EmuConfig.GS);
				std::fprintf(stderr, "[ps2x-gs] applied PCSX2's fixes for %s (%s)\n", config.serial, game->name.c_str());
			}
		}
		ApplyRendererSwitches(game);
		EmuConfig.CurrentAspectRatio = EmuConfig.GS.AspectRatio;
	}
}

// PCSX2 calls this at the start of every present, while its ImGui frame for the picture is open.
namespace Host
{
	void BeginPresentFrame();
}

void Host::BeginPresentFrame()
{
	DrawOverlay();
}

#define PS2XGS_API extern "C" __declspec(dllexport)

PS2XGS_API void ps2xgs_set_overlay(unsigned line, const char *text, unsigned rgba)
{
	if (line >= kOverlayLines)
		return;
	std::lock_guard<std::mutex> lk(s_overlay_lock);
	s_overlay_text[line] = text ? text : "";
	s_overlay_rgba[line] = rgba;
}

PS2XGS_API int ps2xgs_open(const Ps2xGsConfig *config)
{
	std::lock_guard<std::mutex> in(s_in_lock);
	if (s_open)
		return 1;
	if (!config || config->size < sizeof(Ps2xGsConfig))
		return 0;
	if (!InitializeHost(*config))
		return 0;
	// The software renderer compiles its drawing code into memory that SysMemory reserves, which
	// PCSX2's own GS runner allocates at start-up (VMManager::Internal::CPUThreadInitialize).
	// Without it, the first software draw writes through a null code pointer.
	if (!s_sys_memory)
	{
		if (!SysMemory::Allocate())
		{
			std::fprintf(stderr, "[ps2x-gs] could not reserve PCSX2's host memory\n");
			return 0;
		}
		s_sys_memory = true;
	}
	ApplySettings(*config);

	s_parent = static_cast<HWND>(config->parent_window);
	s_ui_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE th = CreateThread(nullptr, 0, UiThread, nullptr, 0, nullptr);
	if (!th)
		return 0;
	CloseHandle(th);
	WaitForSingleObject(s_ui_ready, 10000);
	if (!s_hwnd)
		return 0;

	switch (config->renderer)
	{
		case PS2XGS_RENDERER_DX11: s_type = GSRendererType::DX11; break;
		case PS2XGS_RENDERER_DX12: s_type = GSRendererType::DX12; break;
		case PS2XGS_RENDERER_VULKAN: s_type = GSRendererType::VK; break;
		case PS2XGS_RENDERER_OPENGL: s_type = GSRendererType::OGL; break;
		case PS2XGS_RENDERER_SOFTWARE: s_type = GSRendererType::SW; break;
		default: s_type = GSRendererType::Auto; break;
	}
	s_open_result = 0;
	const bool vsync = config->vsync != 0;
	s_gs_thread = new std::thread([vsync] { GsThread(vsync); });
	{
		std::unique_lock<std::mutex> lk(s_q_lock);
		s_q_opened.wait(lk, [] { return s_open_result != 0; });
	}
	if (s_open_result < 0)
	{
		s_gs_thread->join();
		delete s_gs_thread;
		s_gs_thread = nullptr;
		PostMessageW(s_hwnd, WM_PS2XGS_QUIT, 0, 0);
		s_hwnd = nullptr;
		return 0;
	}
	std::memset(s_regs_in, 0, sizeof(s_regs_in));
	s_cur.reserve(4u << 20);
	s_open = true;
	std::fprintf(stderr, "[ps2x-gs] open: %s, %gx\n", Pcsx2Config::GSOptions::GetRendererName(s_type),
		static_cast<double>(EmuConfig.GS.UpscaleMultiplier));
	return 1;
}

PS2XGS_API void ps2xgs_gif(unsigned path, const void *data, unsigned bytes)
{
	std::lock_guard<std::mutex> in(s_in_lock);
	if (!s_open || !data || bytes < 16 || path < 1 || path > 3)
		return;
	AppendRecord(path == 1 ? REC_GIF1 : path == 2 ? REC_GIF2 : REC_GIF3, data, bytes & ~15u);
}

PS2XGS_API void *ps2xgs_regs()
{
	return s_regs_in;
}

PS2XGS_API void ps2xgs_vsync(unsigned field)
{
	auto f = std::make_unique<Frame>();
	std::vector<u8> next;
	{
		std::lock_guard<std::mutex> lk(s_q_lock);
		if (!s_spare.empty())
		{
			next = std::move(s_spare.back());
			s_spare.pop_back();
		}
	}
	if (next.capacity() < (1u << 20))
		next.reserve(4u << 20);
	{
		std::lock_guard<std::mutex> in(s_in_lock);
		if (!s_open)
			return;
		f->data.swap(s_cur);
		s_cur.swap(next);
		std::memcpy(f->regs, s_regs_in, sizeof(s_regs_in));
	}
	f->field = field;
	f->vsync = true;
	Push(std::move(f));
}

PS2XGS_API void ps2xgs_set_upscale(float upscale)
{
	std::lock_guard<std::mutex> in(s_in_lock);
	if (s_open)
		AppendRecord(REC_UPSCALE, &upscale, sizeof(upscale));
}

PS2XGS_API void ps2xgs_snapshot(const char *path)
{
	std::lock_guard<std::mutex> in(s_in_lock);
	if (s_open && path)
		AppendRecord(REC_SNAP, path, static_cast<u32>(std::strlen(path) + 1));
}

PS2XGS_API void ps2xgs_close()
{
	{
		std::lock_guard<std::mutex> in(s_in_lock);
		if (!s_open)
			return;
		s_open = false;
	}
	auto f = std::make_unique<Frame>();
	f->quit = true;
	Push(std::move(f));
	if (s_gs_thread)
	{
		s_gs_thread->join();
		delete s_gs_thread;
		s_gs_thread = nullptr;
	}
	if (s_hwnd)
		PostMessageW(s_hwnd, WM_PS2XGS_QUIT, 0, 0);
	s_hwnd = nullptr;
	if (s_sys_memory)
	{
		SysMemory::Release();
		s_sys_memory = false;
	}
}
