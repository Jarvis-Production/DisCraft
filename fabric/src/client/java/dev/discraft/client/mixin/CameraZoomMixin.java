package dev.discraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.discraft.world.DisClip;
import net.minecraft.client.Camera;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * The third-person camera (F5) pulls in against Dishonored's walls, terrain and trees as well as
 * Minecraft blocks, the way Minecraft's own camera does against blocks.
 */
@Mixin(Camera.class)
public abstract class CameraZoomMixin {
	@WrapOperation(
		method = "getMaxZoom",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private BlockHitResult discraft$zoomAgainstDishonored(Level level, ClipContext context, Operation<BlockHitResult> original) {
		return DisClip.refine(context.getFrom(), context.getTo(), original.call(level, context), DisClip.Use.PROJECTILE);
	}
}
