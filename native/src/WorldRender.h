#pragma once

#include "Game.h"

#include <d3d9.h>

#include <array>
#include <vector>

// Minecraft's blocks, entities and the block outline, drawn into Dishonored's own frame with
// Direct3D 9's fixed-function pipeline (no shader compiler needed), from the meshes Minecraft
// streams through the render ring. Render thread only.
namespace discraft::WorldRender
{
	// Applies pending render-ring messages (atlas, section meshes, entity textures, ...).
	void Drain(IDirect3DDevice9* a_device);
	// Draws everything as seen by a_view. a_depthMode: 0 own depth only, 1 the game's depth
	// buffer (normal Z), 2 the game's depth buffer (reversed Z).
	void Draw(IDirect3DDevice9* a_device, const CameraView& a_view, int a_width, int a_height, int a_depthMode);
	// Device reset: drop what lives in D3DPOOL_DEFAULT.
	void OnLostDevice();
	// Forget all meshes (Minecraft reconnected).
	void Clear();
	// Blocks Minecraft dug since the last call (Minecraft block coords). Any thread.
	void TakeNewDug(std::vector<std::array<int, 3>>& a_out);
}
