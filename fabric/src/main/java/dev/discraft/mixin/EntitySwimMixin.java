package dev.discraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.discraft.world.DisWater;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.material.FluidState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** Sprint-swimming starts in Dishonored's water too (see {@link DisWater}). */
@Mixin(Entity.class)
public abstract class EntitySwimMixin {
	@WrapOperation(
		method = "updateSwimming",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;getFluidState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/material/FluidState;")
	)
	private FluidState discraft$swimInDishonoredWater(Level level, BlockPos pos, Operation<FluidState> original) {
		FluidState state = original.call(level, pos);
		if (state.isEmpty() && DisWater.active()) {
			FluidState water = DisWater.fluidAt(level, pos);
			if (water != null) {
				return water;
			}
		}
		return state;
	}
}
