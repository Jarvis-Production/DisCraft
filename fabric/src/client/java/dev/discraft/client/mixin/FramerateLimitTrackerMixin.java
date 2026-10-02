package dev.discraft.client.mixin;

import com.mojang.blaze3d.platform.FramerateLimitTracker;
import dev.discraft.client.DisClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** DisClient.paceFrame() locks us to Dishonored's frame rate; don't let MC throttle on its own. */
@Mixin(FramerateLimitTracker.class)
public abstract class FramerateLimitTrackerMixin {
	@Inject(method = "getFramerateLimit", at = @At("HEAD"), cancellable = true)
	private void discraft$unlimited(CallbackInfoReturnable<Integer> cir) {
		if (DisClient.linked()) {
			cir.setReturnValue(260);
		}
	}
}
