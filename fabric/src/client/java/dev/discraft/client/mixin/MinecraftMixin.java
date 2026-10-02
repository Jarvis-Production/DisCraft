package dev.discraft.client.mixin;

import dev.discraft.client.DisClient;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
public abstract class MinecraftMixin {
	@Inject(method = "runTick", at = @At("HEAD"))
	private void discraft$beginFrame(boolean advanceGameTime, CallbackInfo ci) {
		DisClient.beginFrame();
	}

	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render()V", shift = At.Shift.AFTER)
	)
	private void discraft$afterRender(boolean advanceGameTime, CallbackInfo ci) {
		DisClient.afterRender();
	}

	@Inject(method = "renderFrame", at = @At("TAIL"))
	private void discraft$pace(boolean advanceGameTime, CallbackInfo ci) {
		DisClient.paceFrame();
	}
}
