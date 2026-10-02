package dev.discraft.client;

import dev.discraft.DisCraft;
import dev.discraft.client.render.WorldExporter;
import dev.discraft.link.Proto;
import dev.discraft.link.DisLink;
import dev.discraft.world.DisCollision;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.phys.Vec3;
import org.lwjgl.sdl.SDLVideo;

/**
 * Per-frame glue between the Minecraft client and Dishonored. Everything here runs on the render
 * thread, called from MinecraftMixin.
 */
public final class DisClient {
	private static final boolean SHOW_WINDOW = Boolean.getBoolean("discraft.showWindow");
	// Started by Dishonored (DisCraft's bundled instance passes -Ddiscraft.startHidden=true): no window and
	// no title-screen music from the first frame, even while Dishonored is paused (Alt-Tabbed) and the
	// two haven't linked up yet. Otherwise the window only goes once Dishonored is there.
	private static final boolean START_HIDDEN = Boolean.getBoolean("discraft.startHidden");
	private static boolean startedHidden;

	private static final DisLink.GameState game = new DisLink.GameState();
	private static final DisLink.McState mc = new DisLink.McState();
	private static volatile boolean linked;
	private static boolean tookOver;
	private static boolean windowHidden;
	private static int appliedViewportW, appliedViewportH;

	// Teleport / hold state: Dishonored decides where the player is after loads, doors and respawns.
	private static int lastTeleportSeq = -1;
	private static int teleportAck;
	private static boolean teleportPending;
	private static LocalPlayer lastPlayer;
	private static Vec3 holdPos;
	private static Vec3 unlinkedHold;
	private static long holdSince;
	private static long qpcFreq;
	private static LocalPlayer eyePlayer;
	private static float eyeSmoothed;
	private static long frameCounter;
	private static int lastPacedSeq;
	private static boolean gameStalled;
	private static int exporterErrors;

	private DisClient() {
	}

	public static boolean linked() {
		return linked;
	}

	/**
	 * True once Dishonored has connected in this session. From then on Minecraft never touches the
	 * real mouse or keyboard again (even if Dishonored closes), since its window is hidden.
	 */
	public static boolean tookOver() {
		return tookOver;
	}

	public static DisLink.GameState game() {
		return game;
	}

	/** Start of Minecraft.runTick: pull state and input from Dishonored before anything else runs. */
	public static void beginFrame() {
		DisLink.poll();
		quitWithDishonored(Minecraft.getInstance());
		if (START_HIDDEN && !startedHidden) {
			startedHidden = true;
			Minecraft minecraft = Minecraft.getInstance();
			hideWindowOnce(minecraft);
			minecraft.options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
			minecraft.getMusicManager().stopPlaying();
		}
		boolean nowLinked = DisLink.active();
		if (nowLinked) {
			DisLink.readGameState(game); // on a torn read we simply keep last frame's state
			dev.discraft.world.DisWater.refresh();
		} else {
			dev.discraft.world.DisWater.clear();
		}
		if (nowLinked != linked) {
			linked = nowLinked;
			DisCraft.LOG.info("DisCraft: Dishonored link {}", linked ? "up" : "down");
			if (linked) {
				tookOver = true;
				unlinkedHold = null;
				DisCollision.startConsumer();
				applyLinkedOptions();
			} else {
				InputBridge.releaseAll();
				LocalPlayer player = Minecraft.getInstance().player;
				unlinkedHold = player != null ? player.position() : null;
			}
		}
		if (!linked) {
			return;
		}

		Minecraft minecraft = Minecraft.getInstance();
		hideWindowOnce(minecraft);
		applyViewportSize(minecraft);
		MirrorWorld.openWhenReady(minecraft);

		if (game.menuOpen() || game.loading()) {
			InputBridge.releaseAll();
		}
		InputBridge.drain(minecraft);
		ProxySync.frame(minecraft);

		LocalPlayer player = minecraft.player;
		if (player == null) {
			lastPlayer = null;
			return;
		}

		// A new player object means we just joined or respawned: put it where Dishonored's player is.
		if (player != lastPlayer) {
			lastPlayer = player;
			teleportPending = true;
		}
		if (game.drives() && game.inGame() && !game.loading()) {
			followGame(player);
			return;
		}
		if (game.teleportSeq != lastTeleportSeq) {
			lastTeleportSeq = game.teleportSeq;
			teleportPending = true;
		}
		if (teleportPending && game.inGame() && !game.loading()) {
			requestTeleport(minecraft, game.x, game.y, game.z, game.yaw, game.pitch);
			teleportAck = game.teleportSeq;
			teleportPending = false;
			holdPos = new Vec3(game.x, game.y, game.z);
		}

		// Look direction is driven by Dishonored (zero-latency camera); MC uses it for everything else.
		if (minecraft.gui.screen() == null) {
			player.setYRot(game.yaw);
			player.setXRot(game.pitch);
			player.yRotO = game.yaw;
			player.xRotO = game.pitch;
		}
	}

	// Minecraft is started with Dishonored (the DisCraft plugin launches it), so it goes when that Dishonored has
	// closed for good: saved and shut down the normal way. -Ddiscraft.quitWithGame=false keeps it
	// running instead (development: restarting Dishonored without restarting Minecraft).
	private static final boolean QUIT_WITH_GAME = Boolean.parseBoolean(System.getProperty("discraft.quitWithGame", "true"));
	private static long gameGoneSince;
	private static long nextDishonoredCheck;
	// Started hidden by Dishonored but never connected: nobody can see or use this Minecraft, and it
	// would stop the next Dishonored from starting a fresh one ("already running"). It goes after this.
	private static final long NEVER_CONNECTED_QUIT_MS = 10 * 60 * 1000;
	private static final long STARTED_AT = System.currentTimeMillis();
	private static boolean gaveUpWaiting;

	private static void quitWithDishonored(Minecraft minecraft) {
		int pid = DisLink.gamePid();
		long now = System.currentTimeMillis();
		if (QUIT_WITH_GAME && START_HIDDEN && pid == 0 && !tookOver && !gaveUpWaiting && now - STARTED_AT > NEVER_CONNECTED_QUIT_MS) {
			gaveUpWaiting = true;
			DisCraft.LOG.warn("DisCraft: started hidden but Dishonored never connected in {} minutes; quitting", NEVER_CONNECTED_QUIT_MS / 60000);
			minecraft.stop();
			return;
		}
		if (!QUIT_WITH_GAME || pid == 0 || now < nextDishonoredCheck) {
			return;
		}
		nextDishonoredCheck = now + 1000;
		// Alive while its heartbeat (beaten from its own thread, menus and loading screens included)
		// is fresh, or while Windows says the process is still there. A process we may not open (a
		// game run as administrator, or behind a protected launcher) reads as absent, so that alone
		// never counts as closed while the heartbeat goes on.
		if (DisLink.active() || ProcessHandle.of(pid).map(ProcessHandle::isAlive).orElse(false)) {
			gameGoneSince = 0;
			return;
		}
		if (gameGoneSince == 0) {
			gameGoneSince = now;
		} else if (now - gameGoneSince > 5000) {
			DisCraft.LOG.info("DisCraft: Dishonored (pid {}) has closed; saving and quitting", pid);
			minecraft.stop();
		}
	}

	/** Called at the end of every client tick. */
	public static void clientTick(Minecraft minecraft) {
		MirrorWorld.tick(minecraft);
		DiscordPresence.tick(minecraft);
		DisDigClient.tick(minecraft);
		freezeWhileUnlinked(minecraft);
		holdUntilReady(minecraft);
		publishTick(minecraft);
	}

	/**
	 * Dishonored went quiet (a long loading screen, a stall, or it closed). Its collision around the
	 * player may be about to change (interior doors), so keep the player exactly where they were
	 * instead of letting them fall; Dishonored puts them where they belong when it's back.
	 */
	private static void freezeWhileUnlinked(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (linked || !tookOver || player == null) {
			return;
		}
		if (unlinkedHold == null) {
			unlinkedHold = player.position();
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(unlinkedHold.x, unlinkedHold.y, unlinkedHold.z);
		player.xo = unlinkedHold.x;
		player.yo = unlinkedHold.y;
		player.zo = unlinkedHold.z;
		player.resetFallDistance();
	}

	/**
	 * Hands Dishonored the raw physics tick (previous + latest feet, smoothed eye height, walk bob) with a
	 * QueryPerformanceCounter timestamp. Dishonored interpolates between them on its own frame clock,
	 * exactly like Minecraft's renderer does with partial ticks.
	 */
	private static void publishTick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (qpcFreq == 0) {
			qpcFreq = DisLink.qpcFrequency();
		}
		float tickMs = minecraft.level != null ? minecraft.level.tickRateManager().millisecondsPerTick() : 50.0F;
		// The tick really "happened" partial ticks ago (DeltaTracker keeps the remainder).
		float remainder = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
		mc.tickQpc = DisLink.qpc() - (long) (remainder * tickMs * qpcFreq / 1000.0);
		mc.tickMs = tickMs;
		mc.prevX = player.xo;
		mc.prevY = player.yo;
		mc.prevZ = player.zo;
		mc.curX = player.getX();
		mc.curY = player.getY();
		mc.curZ = player.getZ();
		// Same smoothing as Camera.tick(): eye height eases halfway toward the target each tick.
		if (player != eyePlayer) {
			eyePlayer = player;
			eyeSmoothed = player.getEyeHeight();
		}
		mc.eyeHeightO = eyeSmoothed;
		eyeSmoothed += (player.getEyeHeight() - eyeSmoothed) * 0.5F;
		mc.eyeHeightT = eyeSmoothed;
		boolean bob = minecraft.options.bobView().get();
		var avatar = player.avatarState();
		mc.walkDistO = bob ? avatar.getInterpolatedWalkDistance(0.0F) : 0.0F;
		mc.walkDist = bob ? avatar.getInterpolatedWalkDistance(1.0F) : 0.0F;
		mc.bobO = bob ? avatar.getInterpolatedBob(0.0F) : 0.0F;
		mc.bob = bob ? avatar.getInterpolatedBob(1.0F) : 0.0F;
		DisLink.writeMcState(mc);
	}

	/**
	 * Dishonored walks its own player (with its own collision and camera); Minecraft's player stands
	 * exactly there and looks where Dishonored's camera looks, every frame. The host stays in charge,
	 * like the universal-modder GTA V passthrough: no falling through floors Minecraft can't see.
	 */
	private static void followGame(LocalPlayer player) {
		holdPos = null;
		teleportPending = false;
		teleportAck = game.teleportSeq;
		double x = game.x, y = game.y, z = game.z;
		if (player.distanceToSqr(x, y, z) > 64.0 * 64.0) {
			requestTeleport(Minecraft.getInstance(), x, y, z, game.yaw, game.pitch);
		}
		player.setPos(x, y, z);
		player.xo = player.xOld = x;
		player.yo = player.yOld = y;
		player.zo = player.zOld = z;
		player.setDeltaMovement(Vec3.ZERO);
		player.resetFallDistance();
		player.setYRot(game.yaw);
		player.setXRot(game.pitch);
		player.yRotO = game.yaw;
		player.xRotO = game.pitch;
		player.yHeadRot = player.yHeadRotO = game.yaw;
		// Not flying: flying hands the controls to Minecraft (Dishonored watches MC_FLYING).
	}

	/** Freeze the player until Dishonored's collision around them has arrived. */
	private static void holdUntilReady(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (game.drives() && game.inGame() && !game.loading()) {
			followGame(player);
			return;
		}
		if (!game.inGame() || game.loading()) {
			// Dishonored is on its main menu or a loading screen: park the player where they are.
			if (holdPos == null) {
				holdPos = player.position();
			}
			teleportPending = true;
		}
		if (holdPos == null) {
			holdSince = 0;
			return;
		}
		if (holdSince == 0) {
			holdSince = System.currentTimeMillis();
		}
		int bx = (int) Math.floor(holdPos.x), by = (int) Math.floor(holdPos.y), bz = (int) Math.floor(holdPos.z);
		boolean known = DisCollision.isKnown(bx, by - 1, bz) && DisCollision.isKnown(bx, by, bz)
			&& DisCollision.isKnown(bx, by - DisCollision.REGION_SIZE, bz);
		// Release once there is actual ground below (or after a timeout, e.g. when mid-air on purpose).
		boolean ready = known && (DisCollision.hasSolidBelow(bx, by, bz, 12) || System.currentTimeMillis() - holdSince > 6000);
		if (ready && game.inGame() && !game.loading()) {
			// Dishonored's feet can sit a fraction of a voxel inside our ground layer. Minecraft's
			// collision never pushes you out of a shape, so you'd drop through: lift out first.
			Vec3 safe = liftOutOfGeometry(player, holdPos);
			if (safe.y != holdPos.y) {
				player.setPos(safe.x, safe.y, safe.z);
				player.yo = safe.y;
				DisCraft.LOG.info("DisCraft: lifted player {} blocks out of the ground", String.format("%.3f", safe.y - holdPos.y));
			}
			holdPos = null;
			return;
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(holdPos.x, holdPos.y, holdPos.z);
		player.xo = holdPos.x;
		player.yo = holdPos.y;
		player.zo = holdPos.z;
		player.resetFallDistance();
	}

	private static Vec3 liftOutOfGeometry(LocalPlayer player, Vec3 pos) {
		// Stand on the exact Dishonored ground if it is slightly above the feet (up to 2.5 blocks).
		double ground = DisCollider.groundAt(pos.x, pos.y, pos.z, 2.5);
		return !Double.isNaN(ground) && ground > pos.y ? new Vec3(pos.x, ground, pos.z) : pos;
	}

	private static void requestTeleport(Minecraft minecraft, double x, double y, double z, float yaw, float pitch) {
		LocalPlayer player = minecraft.player;
		player.setPos(x, y, z);
		player.setDeltaMovement(Vec3.ZERO);
		player.resetFallDistance();
		var server = minecraft.getSingleplayerServer();
		if (server != null) {
			var uuid = player.getUUID();
			server.execute(() -> {
				ServerPlayer sp = server.getPlayerList().getPlayer(uuid);
				if (sp != null) {
					sp.teleportTo(x, y, z);
					sp.setYRot(yaw);
					sp.setXRot(pitch);
					sp.resetFallDistance();
				}
			});
		}
		DisCraft.LOG.info("DisCraft: teleported to {} {} {}", x, y, z);
	}

	/** After GameRenderer.render(): report the player to Dishonored and ship the overlay frame. */
	public static void afterRender() {
		if (!linked) {
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		int flags = 0;
		if (player != null && minecraft.level != null) {
			float partial = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
			Vec3 feet = player.getPosition(partial);
			Camera camera = minecraft.gameRenderer.mainCamera();
			flags |= Proto.MC_IN_WORLD;
			if (player.onGround()) {
				flags |= Proto.MC_ON_GROUND;
			}
			if (player.isShiftKeyDown()) {
				flags |= Proto.MC_SNEAKING;
			}
			if (player.isSprinting()) {
				flags |= Proto.MC_SPRINTING;
			}
			if (player.isDeadOrDying()) {
				flags |= Proto.MC_DEAD;
			}
			if (player.isSwimming()) {
				flags |= Proto.MC_SWIMMING;
			}
			if (player.getAbilities().flying) {
				flags |= Proto.MC_FLYING;
			}
			mc.x = feet.x;
			mc.y = feet.y;
			mc.z = feet.z;
			mc.yaw = player.getYRot();
			mc.pitch = player.getXRot();
			// The eye, not the camera: in third person Minecraft's camera sits behind or in front.
			Vec3 eye = camera.isDetached() ? player.getEyePosition(partial) : camera.position();
			mc.eyeHeight = (float) (eye.y - feet.y);
			mc.eyeX = eye.x;
			mc.eyeY = eye.y;
			mc.eyeZ = eye.z;
			mc.fov = camera.getFov();
			// Minecraft's F5 camera: Dishonored puts its camera where Minecraft's would be.
			mc.cameraMode = minecraft.options.getCameraType().ordinal();
			mc.cameraDistance = camera.isDetached() ? (float) camera.position().distanceTo(player.getEyePosition(partial)) : 0.0F;
			// Walk bob, exactly what GameRenderer.bobView() uses this frame.
			var entityState = minecraft.gameRenderer.gameRenderState().levelRenderState.cameraRenderState.entityRenderState;
			boolean bob = minecraft.options.bobView().get() && entityState.isPlayer;
			mc.bobPhase = bob ? entityState.backwardsInterpolatedWalkDistance : 0.0F;
			mc.bobAmount = bob ? entityState.bob : 0.0F;
		}
		if (minecraft.gui.screen() != null) {
			flags |= Proto.MC_SCREEN_OPEN;
		}
		mc.flags = flags;
		mc.sensitivity = minecraft.options.sensitivity().get().floatValue();
		mc.teleportAck = holdPos == null ? teleportAck : teleportAck - 1; // not "arrived" until we are released
		mc.guiScale = minecraft.getWindow().getGuiScale();
		mc.frameCounter = ++frameCounter;
		DisLink.writeMcState(mc);

		if ((flags & Proto.MC_IN_WORLD) != 0) {
			try {
				WorldExporter.frame(minecraft, minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false));
			} catch (RuntimeException e) {
				if (exporterErrors++ < 5) {
					DisCraft.LOG.error("DisCraft: world export failed", e);
				}
			}
			FrameExporter.capture(minecraft);
		}
	}

	/** End of the frame: render at most once per Dishonored frame instead of spinning freely. */
	public static void paceFrame() {
		if (!linked) {
			return;
		}
		if (gameStalled && (DisLink.gameStateSeq() >>> 1) == lastPacedSeq) {
			return; // Dishonored is paused (menu / alt-tab): don't block every frame waiting for it
		}
		gameStalled = false;
		long deadline = System.nanoTime() + 25_000_000L;
		// GameState.seq advances by 2 per Dishonored frame (odd while writing).
		while ((DisLink.gameStateSeq() >>> 1) == lastPacedSeq && System.nanoTime() < deadline) {
			Thread.onSpinWait();
			if (deadline - System.nanoTime() > 2_000_000L) {
				Thread.yield();
			}
		}
		int seqNow = DisLink.gameStateSeq() >>> 1;
		gameStalled = seqNow == lastPacedSeq;
		lastPacedSeq = seqNow;
	}

	private static void applyLinkedOptions() {
		Minecraft minecraft = Minecraft.getInstance();
		var options = minecraft.options;
		options.pauseOnLostFocus = false;
		options.vignette().set(false);
		options.enableVsync().set(false);
		options.framerateLimit().set(260);
		// Minecraft doesn't draw the world itself; these only decide how far out placed blocks,
		// arrows and Dishonored NPC stand-ins stay loaded and simulated.
		options.renderDistance().set(8);
		options.simulationDistance().set(8);
		options.autoJump().set(false);
		options.onboardAccessibility = false;
		if (options.tutorialStep != net.minecraft.client.tutorial.TutorialSteps.NONE) {
			minecraft.getTutorial().setStep(net.minecraft.client.tutorial.TutorialSteps.NONE);
		}
		options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
		options.save();
	}

	private static void hideWindowOnce(Minecraft minecraft) {
		if (windowHidden || SHOW_WINDOW) {
			return;
		}
		windowHidden = true;
		SDLVideo.SDL_HideWindow(minecraft.getWindow().handle());
		DisCraft.LOG.info("DisCraft: game window hidden (run with -Ddiscraft.showWindow=true to keep it)");
	}

	private static void applyViewportSize(Minecraft minecraft) {
		int w = Math.min(game.viewportW, Proto.MAX_OVERLAY_W);
		int h = Math.min(game.viewportH, Proto.MAX_OVERLAY_H);
		if (w <= 0 || h <= 0 || (w == appliedViewportW && h == appliedViewportH)) {
			return;
		}
		appliedViewportW = w;
		appliedViewportH = h;
		minecraft.getWindow().setWindowed(w, h);
		DisCraft.LOG.info("DisCraft: sizing overlay to Dishonored viewport {}x{}", w, h);
	}
}
