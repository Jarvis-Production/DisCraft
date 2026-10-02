package dev.discraft.client.mixin;

import com.mojang.blaze3d.platform.Window;
import dev.discraft.client.DisClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The MC window is hidden while linked; Dishonored has the real focus, so pretend we do too. */
@Mixin(Window.class)
public abstract class WindowMixin {
	@Inject(method = "isFocused", at = @At("HEAD"), cancellable = true)
	private void discraft$focused(CallbackInfoReturnable<Boolean> cir) {
		if (DisClient.tookOver()) {
			// Focused while Dishonored is connected; if Dishonored goes away, act unfocused so MC
			// never tries to grab the (hidden) mouse.
			cir.setReturnValue(DisClient.linked());
		}
	}

	@Inject(method = "isIconified", at = @At("HEAD"), cancellable = true)
	private void discraft$notIconified(CallbackInfoReturnable<Boolean> cir) {
		if (DisClient.linked()) {
			cir.setReturnValue(false);
		}
	}
}
