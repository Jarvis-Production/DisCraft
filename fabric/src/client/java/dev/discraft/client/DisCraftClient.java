package dev.discraft.client;

import dev.discraft.combat.DisCombat;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.rendering.v1.EntityRendererRegistry;
import net.minecraft.client.renderer.entity.NoopRenderer;

public final class DisCraftClient implements ClientModInitializer {
	@Override
	public void onInitializeClient() {
		dev.discraft.link.DisLink.announceRunning();
		DiscordPresence.start();
		// SkyCraft's "Skyrim destruction" pause-menu button isn't registered: Dishonored's side can't
		// cut holes in its geometry, so nothing there is diggable.
		// Multiplayer without editing files: the host opens their world to LAN (O, Open to LAN) and
		// e4mc gives them a link; friends type /join <link> in chat, and /leave to come back.
		net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback.EVENT.register((dispatcher, context) -> {
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("join")
				.then(net.fabricmc.fabric.api.client.command.v2.ClientCommands.argument("link", com.mojang.brigadier.arguments.StringArgumentType.greedyString())
					.executes(c -> {
						String link = com.mojang.brigadier.arguments.StringArgumentType.getString(c, "link");
						c.getSource().sendFeedback(net.minecraft.network.chat.Component.literal("Joining " + link.trim() + "..."));
						// After the chat screen has closed: this leaves the current world.
						net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.joinFriend(net.minecraft.client.Minecraft.getInstance(), link));
						return 1;
					})));
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("leave").executes(c -> {
				net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.leaveFriend(net.minecraft.client.Minecraft.getInstance()));
				return 1;
			}));
		});
		ClientTickEvents.END_CLIENT_TICK.register(DisClient::clientTick);
		// A guest in a friend's world: dying there kills this player's own Dishonored character.
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.registerGlobalReceiver(dev.discraft.net.DisNet.Died.TYPE, (payload, context) -> {
			if (dev.discraft.link.DisLink.active()) {
				dev.discraft.link.DisLink.pushEvent(dev.discraft.link.Proto.EV_PLAYER_DIED, payload.attackerActorId(), 0, 0, 0, 0, 0);
			}
		});
		// Dishonored draws the real NPC; its Minecraft stand-in is only a hitbox.
		EntityRendererRegistry.register(DisCombat.DISHONORED_ACTOR, NoopRenderer::new);
		// Players (client-side movement AND the integrated server's re-check of it) use the smooth
		// triangle collider, never Dishonored's voxels; otherwise the server sees the smooth position
		// dip into a voxel and teleports the player back every few ticks.
		dev.discraft.world.DisCollision.setSmoothCollider(e -> e instanceof net.minecraft.world.entity.player.Player && DisClient.linked());
	}
}
