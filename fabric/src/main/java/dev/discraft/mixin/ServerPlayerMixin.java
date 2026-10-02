package dev.discraft.mixin;

import dev.discraft.DisCraft;
import dev.discraft.combat.DisCombat;
import dev.discraft.combat.DishonoredActorEntity;
import dev.discraft.link.Proto;
import dev.discraft.link.DisLink;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.server.level.ServerPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ServerPlayer.class)
public abstract class ServerPlayerMixin {
	/** Critical hits on a Dishonored actor are flagged so Dishonored can play them up. */
	@Inject(method = "crit", at = @At("HEAD"))
	private void discraft$critDishonored(Entity entity, CallbackInfo ci) {
		if (entity instanceof DishonoredActorEntity proxy) {
			proxy.markCritical();
		}
	}

	/** Dying in Minecraft is dying in Dishonored: the host's through the link, a guest's through theirs. */
	@Inject(method = "die", at = @At("HEAD"))
	private void discraft$diesInDishonored(DamageSource source, CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		int attacker = DisCombat.attackerActorId(source);
		if (!dev.discraft.net.DisNet.isHost(self)) {
			if (net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.canSend(self, dev.discraft.net.DisNet.Died.TYPE)) {
				net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.send(self, new dev.discraft.net.DisNet.Died(attacker));
			}
			DisCraft.LOG.info("DisCraft: guest {} died ({}); telling their Dishonored", self.getPlainTextName(), source.getMsgId());
			return;
		}
		if (DisLink.active()) {
			DisLink.pushEvent(Proto.EV_PLAYER_DIED, attacker, 0, 0, 0, 0, 0);
			DisCraft.LOG.info("DisCraft: player died ({}); telling Dishonored", source.getMsgId());
		}
	}
}
