package dev.discraft.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.discraft.client.InputBridge;
import dev.discraft.client.DisClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Keyboard state and mouse capture come from Dishonored while linked, not from SDL. */
@Mixin(InputConstants.class)
public abstract class InputConstantsMixin {
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void discraft$isKeyDown(int key, CallbackInfoReturnable<Boolean> cir) {
		if (DisClient.tookOver()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void discraft$grabMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (DisClient.tookOver()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void discraft$releaseMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (DisClient.tookOver()) {
			ci.cancel();
		}
	}
}
