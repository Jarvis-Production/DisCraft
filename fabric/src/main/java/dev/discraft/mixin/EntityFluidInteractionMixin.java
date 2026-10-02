package dev.discraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.discraft.world.DisWater;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.EntityFluidInteraction;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.material.FluidState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Entities meet Dishonored's water (lakes, rivers, the sea) as Minecraft water: swimming, floating,
 * slow movement, drowning, splashes. See {@link DisWater}.
 */
@Mixin(EntityFluidInteraction.class)
public abstract class EntityFluidInteractionMixin {
	@WrapOperation(
		method = "update",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/entity/EntityFluidInteraction;hasFluidAndLoaded(Lnet/minecraft/world/level/Level;IIIIII)Z"
		)
	)
	private static boolean discraft$gameWaterNearby(Level level, int x0, int y0, int z0, int x1, int y1, int z1, Operation<Boolean> original) {
		return original.call(level, x0, y0, z0, x1, y1, z1) || DisWater.anyIn(x0, y0, z0, x1, y1, z1);
	}

	@WrapOperation(
		method = "update",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/BlockGetter;getFluidState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/material/FluidState;")
	)
	private FluidState discraft$gameWater(BlockGetter level, BlockPos pos, Operation<FluidState> original) {
		FluidState state = original.call(level, pos);
		if (state.isEmpty() && DisWater.active()) {
			FluidState water = DisWater.fluidAt(level, pos);
			if (water != null) {
				return water;
			}
		}
		return state;
	}

	@WrapOperation(
		method = "update",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/material/FluidState;getHeight(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)F"
		)
	)
	private float discraft$gameWaterHeight(FluidState state, BlockGetter level, BlockPos pos, Operation<Float> original) {
		float height = DisWater.active() ? DisWater.substitutedHeight(level, pos) : -1.0F;
		return height >= 0.0F ? height : original.call(state, level, pos);
	}

	@WrapOperation(
		method = "update",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/material/FluidState;getHeightForCamera(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)F"
		)
	)
	private float discraft$gameWaterEyeHeight(FluidState state, BlockGetter level, BlockPos pos, Operation<Float> original) {
		float height = DisWater.active() ? DisWater.substitutedHeight(level, pos) : -1.0F;
		return height >= 0.0F ? height : original.call(state, level, pos);
	}
}
