#pragma once

#include "Coords.h"
#include "Link.h"
#include "ue3/UE3.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace discraft
{
	// What the camera the game rendered last showed, in Minecraft space (for drawing blocks).
	struct CameraView
	{
		bool          valid{ false };
		double        x{ 0 }, y{ 0 }, z{ 0 };  // eye
		double        yaw{ 0 }, pitch{ 0 }, roll{ 0 };  // Minecraft degrees
		float         fov{ 90.0f };                      // Unreal FOV (horizontal) degrees
		std::uint64_t frame{ 0 };
	};

	// State shared between the game thread (per-frame update, input) and the render thread.
	struct Runtime
	{
		// Minecraft is connected, in its world, has arrived where the game's player is, and the game
		// isn't in a menu: Minecraft's player drives the game's player.
		std::atomic<bool> puppeting{ false };
		// Keyboard and mouse go to Minecraft (puppeting, or Minecraft arriving after a teleport).
		std::atomic<bool> routeInput{ false };
		std::atomic<bool> mcScreenOpen{ false };
		std::atomic<bool> gameMenuOpen{ false };
		std::atomic<bool> mcInWorld{ false };
		// F11 (by default): DisCraft hands the controls back to Dishonored until pressed again.
		std::atomic<bool> suspended{ false };
		std::atomic<bool> mcCrosshair{ false };
		std::atomic<int>  mcGuiScale{ 0 };
		std::atomic<int>  mcCameraMode{ 0 };

		// Minecraft's virtual cursor (overlay pixels) while one of its screens is open.
		std::atomic<int> cursorX{ 0 };
		std::atomic<int> cursorY{ 0 };
		// The overlay size Minecraft renders, and the game's back buffer.
		std::atomic<int> overlayW{ 1280 }, overlayH{ 720 };
		std::atomic<int> screenW{ 0 }, screenH{ 0 };

		// Look direction in Minecraft degrees, integrated from raw mouse input (game thread).
		float yaw{ 0.0f };
		float pitch{ 0.0f };
		bool  lookInitialized{ false };
		float sensitivity{ 0.5f };

		// Unreal units per Minecraft block: the player's collision cylinder is 1.8 blocks tall.
		std::atomic<double> unitsPerBlock{ 50.0 };
		std::atomic<bool>   discovered{ false };
		std::atomic<bool>   installed{ false };
		std::atomic<std::uint64_t> lastTickMs{ 0 };
		std::atomic<ue3::Obj>      playerPawn{ 0 };

		std::mutex   viewLock;
		CameraView   view;
		// The player's feet this frame (Minecraft space; third-person body is drawn there).
		double feetX{ 0 }, feetY{ 0 }, feetZ{ 0 };
		bool   feetValid{ false };
	};

	Runtime& State();

	namespace Game
	{
		// Game thread, once UE3 discovery has run: engine bindings and the per-frame hook.
		bool Install();
		// Text for the top-left corner (empty: nothing to say). Any thread.
		std::wstring StatusText();
		void         SetDiscoveryFailed(bool a_failed);
		CameraView   View();
	}

	namespace Input
	{
		// Init thread: DirectInput hooks (before the game reads its devices).
		void InstallDirectInput();
		// Game thread: takes over the game window's messages.
		void Attach(void* a_hwnd);
		// Mouse movement since the last frame (raw counts), for looking around.
		void ConsumeLook(float& a_dx, float& a_dy);
		// Tells Minecraft to release every key and button.
		void ReleaseAll();
		// Requests from hotkeys, taken once per frame by the game thread.
		bool TakeUseRequest();
	}

	namespace Collision
	{
		// World change or Minecraft (re)connected: Minecraft drops everything; start over.
		void Reset(std::uint32_t a_epoch);
		// Game thread: traces part of the world around the player, sends finished regions.
		void PerFrame(ue3::Obj a_pawn, const Vec3d& a_feet, bool a_urgent);
	}

	namespace Actors
	{
		bool Install();
		// Game thread: actor table out, Minecraft's hits in, damage to the player bridged.
		void PerFrame(ue3::Obj a_controller, ue3::Obj a_pawn, bool a_puppeting, float a_delta);
		// Puppet off: give the player's pawn its normal health handling back.
		void Release(ue3::Obj a_pawn);
	}

	namespace Render
	{
		// Init thread: hooks Direct3D 9 (Present/Reset) before the game creates its device.
		void InstallEarly();
		// A short message in the corner for a few seconds (any thread).
		void Notify(const std::wstring& a_text, float a_seconds);
		void CycleDepthMode();
	}

	namespace Launcher
	{
		void StartMinecraft();

		enum class Status
		{
			kOff,         // not started by DisCraft (bStartWithGame = 0)
			kRunning,     // was already running
			kStarting,    // being started
			kSignIn,      // first start of the bundled Minecraft: Prism asks for a Microsoft account
			kNoLauncher,  // nothing to start it with
			kFailed,      // starting it failed
		};
		Status GetStatus();
		bool   MinecraftRunning();
		bool   PrismRunning();
	}
}
