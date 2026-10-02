#pragma once

#include "discraft_protocol.h"

#include <cstdint>
#include <functional>

namespace discraft
{
	// Owner of the shared-memory mapping (the game creates it; Minecraft opens it).
	// Ported from SkyCraft's Link (https://github.com/chasmlol/SkyCraft, MIT).
	class Link
	{
	public:
		static Link& Get();

		bool Create();
		[[nodiscard]] bool Valid() const { return base_ != nullptr; }

		// True if Minecraft has touched its heartbeat recently.
		[[nodiscard]] bool          McAlive() const;
		void                        Heartbeat();
		// Process id Minecraft wrote when it opened the mapping (changes when Minecraft restarts).
		[[nodiscard]] std::uint32_t McPid() const;

		// Seqlock write of game -> MC state. Called once per game frame (MC paces on seq).
		void WriteGameState(const proto::GameState& a_state);
		void WriteWaterGrid(const proto::WaterGrid& a_grid);
		// Seqlock read of MC -> game state. Returns false if no consistent snapshot was obtained.
		bool ReadMcState(proto::McState& a_out) const;

		// Input ring (producer side). Drops the event if MC has fallen a full ring behind.
		void PushInput(proto::InputType a_type, std::uint16_t a_code, std::int32_t a_a = 0, std::int32_t a_b = 0, std::int32_t a_c = 0);

		// Collision ring (producer side, one thread only). Returns false if the ring is full.
		bool                        WriteCollision(proto::ColType a_type, const void* a_payload, std::uint32_t a_bytes);
		// Bytes free in the collision ring.
		[[nodiscard]] std::uint64_t CollisionSpace() const;

		// Actor table (producer, game thread).
		void WriteActors(const proto::ActorRecord* a_records, std::uint32_t a_count);
		// Event ring (consumer, game thread). Returns false when empty.
		bool PopEvent(proto::McEvent& a_out);
		// World entities + block outline (seqlock read, render thread).
		bool ReadWorldEntities(proto::WorldEntities& a_out) const;
		// Render ring (consumer, render thread): calls a_fn(type, payload, bytes) for each pending
		// message, up to about a_maxBytes of payload. The payload points into shared memory.
		void DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& a_fn, std::uint64_t a_maxBytes);

		// Overlay triple buffer (consumer side). If a newer frame is available, swaps it into the
		// front slot and returns true. FrontPixels/FrontHeader describe the current front slot.
		bool                                       AcquireOverlayFrame();
		void                                       ResetOverlay();
		[[nodiscard]] const std::uint8_t*          FrontPixels() const;
		[[nodiscard]] const proto::OverlaySlotHdr* FrontHeader() const;

	private:
		template <class T>
		T* At(std::uint64_t a_off) const
		{
			return reinterpret_cast<T*>(base_ + a_off);
		}

		void*         mapping_{ nullptr };
		std::uint8_t* base_{ nullptr };
		std::uint32_t overlayFront_{ 2 };
	};
}
