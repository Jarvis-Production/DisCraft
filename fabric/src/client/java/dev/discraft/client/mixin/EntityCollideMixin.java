package dev.discraft.client.mixin;

import dev.discraft.client.DisClient;
import dev.discraft.client.DisCollider;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * After vanilla has collided the local player's movement with Minecraft blocks, collide it with
 * Dishonored's exact triangles (smooth slopes instead of voxel stair-steps).
 */
@Mixin(Entity.class)
public abstract class EntityCollideMixin {
	@Inject(method = "collide", at = @At("RETURN"), cancellable = true)
	private void discraft$smoothDishonoredCollision(Vec3 movement, CallbackInfoReturnable<Vec3> cir) {
		if ((Object) this instanceof LocalPlayer player && DisClient.linked() && !player.noPhysics) {
			cir.setReturnValue(DisCollider.collide(player, cir.getReturnValue()));
		}
	}
}
