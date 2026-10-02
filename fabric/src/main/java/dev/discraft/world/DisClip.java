package dev.discraft.world;

import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/**
 * Makes Minecraft ray casts (arrows and other projectiles, the crosshair pick) hit Dishonored's exact
 * collision triangles. Vanilla still clips against real Minecraft blocks; whichever is nearer wins.
 */
public final class DisClip {
	private DisClip() {
	}

	public enum Use {
		/** A projectile: the hit cell is the one the surface is in (it sticks there). */
		PROJECTILE,
		/** The player's crosshair: the hit cell is where a block placed against the surface goes. */
		PICK
	}

	private static final ThreadLocal<List<DisTri>> SCRATCH = ThreadLocal.withInitial(ArrayList::new);

	public static BlockHitResult refine(Vec3 from, Vec3 to, BlockHitResult vanilla, Use use) {
		DisRay.Hit hit = cast(from, to);
		if (use == Use.PICK) {
			// The walls of a dug hole (Dishonored ground the crosshair meets from inside the hole).
			double limit = hit != null ? hit.t() : 1.0;
			if (vanilla.getType() != HitResult.Type.MISS) {
				limit = Math.min(limit, Math.sqrt(from.distanceToSqr(vanilla.getLocation()) / Math.max(from.distanceToSqr(to), 1e-9)));
			}
			DisRay.Hit wall = digWall(from, to, limit);
			if (wall != null) {
				hit = wall;
				vanilla = BlockHitResult.miss(to, Direction.UP, BlockPos.containing(to));
			}
		}
		if (hit == null) {
			return vanilla;
		}
		Vec3 location = new Vec3(hit.x(), hit.y(), hit.z());
		if (vanilla.getType() != HitResult.Type.MISS && from.distanceToSqr(vanilla.getLocation()) <= from.distanceToSqr(location)) {
			return vanilla;
		}
		Direction face = Direction.values()[DisRay.dominantFace(hit.nx(), hit.ny(), hit.nz())];
		int[] cell = use == Use.PICK ? DisRay.placementCell(hit) : DisRay.surfaceCell(hit);
		return new DishonoredHitResult(location, face, new BlockPos(cell[0], cell[1], cell[2]), hit);
	}

	private static final DisTri STONE_WALL = new DisTri(new float[9], 0, dev.discraft.link.Proto.TRI_DIGGABLE | (dev.discraft.link.Proto.DIG_STONE << dev.discraft.link.Proto.TRI_MATERIAL_SHIFT));

	/**
	 * Where the segment, having gone through dug cells, first enters an undug cell inside Dishonored's
	 * geometry (the wall of a hole, drawn by the client's DigWalls), before segment parameter
	 * {@code limit}; or null.
	 */
	static DisRay.Hit digWall(Vec3 from, Vec3 to, double limit) {
		DisDig.DugLookup dug = DisDig.clientDug;
		if (dug == null) {
			return null;
		}
		double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
		int x = (int) Math.floor(from.x), y = (int) Math.floor(from.y), z = (int) Math.floor(from.z);
		int stepX = dx > 0 ? 1 : -1, stepY = dy > 0 ? 1 : -1, stepZ = dz > 0 ? 1 : -1;
		double tDeltaX = dx == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dx);
		double tDeltaY = dy == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dy);
		double tDeltaZ = dz == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dz);
		double tMaxX = dx == 0 ? Double.POSITIVE_INFINITY : ((dx > 0 ? x + 1 - from.x : from.x - x) * tDeltaX);
		double tMaxY = dy == 0 ? Double.POSITIVE_INFINITY : ((dy > 0 ? y + 1 - from.y : from.y - y) * tDeltaY);
		double tMaxZ = dz == 0 ? Double.POSITIVE_INFINITY : ((dz > 0 ? z + 1 - from.z : from.z - z) * tDeltaZ);
		boolean wasDug = dug.isDug(x, y, z);
		DisDig.Probe probe = null;
		for (int i = 0; i < 64; i++) {
			double t;
			double nx = 0, ny = 0, nz = 0;
			if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
				t = tMaxX;
				tMaxX += tDeltaX;
				x += stepX;
				nx = -stepX;
			} else if (tMaxY <= tMaxZ) {
				t = tMaxY;
				tMaxY += tDeltaY;
				y += stepY;
				ny = -stepY;
			} else {
				t = tMaxZ;
				tMaxZ += tDeltaZ;
				z += stepZ;
				nz = -stepZ;
			}
			if (t > limit) {
				return null;
			}
			boolean isDug = dug.isDug(x, y, z);
			if (wasDug && !isDug) {
				double px = from.x + dx * t, py = from.y + dy * t, pz = from.z + dz * t;
				if (probe == null) {
					probe = new DisDig.Probe().around(Math.min(from.x, to.x), Math.min(from.y, to.y), Math.min(from.z, to.z), Math.max(from.x, to.x), Math.max(from.y, to.y),
						Math.max(from.z, to.z));
				}
				if (probe.test(px - nx * 0.02, py - ny * 0.02, pz - nz * 0.02) > DisDig.AIR) {
					return new DisRay.Hit(t, px, py, pz, nx, ny, nz, probe.surface != null ? probe.surface : STONE_WALL);
				}
			}
			wasDug = isDug;
		}
		return null;
	}

	/** Nearest Dishonored triangle hit on the segment, or null. */
	public static DisRay.Hit cast(Vec3 from, Vec3 to) {
		List<DisTri> tris = SCRATCH.get();
		tris.clear();
		DisCollision.trianglesNear(new AABB(from, to).inflate(0.01), tris);
		if (tris.isEmpty()) {
			return null;
		}
		DisRay.Hit hit = DisRay.cast(tris, from.x, from.y, from.z, to.x, to.y, to.z);
		tris.clear();
		return hit;
	}

	/** A hit on Dishonored geometry (not a Minecraft block). Keeps the exact surface normal and triangle. */
	public static final class DishonoredHitResult extends BlockHitResult {
		public final double nx, ny, nz;
		public final DisRay.Hit hit;

		public DishonoredHitResult(Vec3 location, Direction direction, BlockPos pos, DisRay.Hit hit) {
			super(location, direction, pos, false);
			this.nx = hit.nx();
			this.ny = hit.ny();
			this.nz = hit.nz();
			this.hit = hit;
		}
	}
}
