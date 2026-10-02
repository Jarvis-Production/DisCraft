package dev.discraft.client;

import dev.discraft.combat.DishonoredActorEntity;
import dev.discraft.link.DisLink;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.client.Minecraft;
import net.minecraft.world.entity.Entity;

/**
 * Puts the client's copies of the Dishonored actor stand-ins exactly where Dishonored has the actors this
 * frame, so the crosshair and melee reach line up with what's on screen (the server copy only
 * moves once per tick and reaches the client a tick or two later).
 */
final class ProxySync {
	private static final List<DisLink.Actor> ACTORS = new ArrayList<>();
	private static final Map<Integer, DisLink.Actor> BY_ID = new HashMap<>();

	private ProxySync() {
	}

	static void frame(Minecraft minecraft) {
		if (minecraft.level == null || !DisLink.readActors(ACTORS)) {
			return;
		}
		BY_ID.clear();
		for (DisLink.Actor a : ACTORS) {
			BY_ID.put(a.actorId(), a);
		}
		for (Entity entity : minecraft.level.entitiesForRendering()) {
			if (entity instanceof DishonoredActorEntity proxy) {
				DisLink.Actor a = BY_ID.get(proxy.actorId());
				if (a == null) {
					continue;
				}
				proxy.setSize(a.width(), a.height());
				proxy.setPos(a.x(), a.y(), a.z());
				proxy.xo = a.x();
				proxy.yo = a.y();
				proxy.zo = a.z();
				proxy.setYRot(a.yaw());
				proxy.yRotO = a.yaw();
			}
		}
	}
}
