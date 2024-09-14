// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.
#include "g_local.h"

/*
=================
fire_hit

Used for all impact (hit/punch/slash) attacks
=================
*/
bool fire_hit(gentity_t *self, vec3_t aim, int damage, int kick) {
	trace_t tr;
	vec3_t	forward, right, up;
	vec3_t	v;
	vec3_t	point;
	float	range;
	vec3_t	dir;

	// see if enemy is in range
	range = distance_between_boxes(self->enemy->absmin, self->enemy->absmax, self->absmin, self->absmax);
	if (range > aim[0])
		return false;

	if (!(aim[1] > self->mins[0] && aim[1] < self->maxs[0])) {
		// this is a side hit so adjust the "right" value out to the edge of their bbox
		if (aim[1] < 0)
			aim[1] = self->enemy->mins[0];
		else
			aim[1] = self->enemy->maxs[0];
	}

	point = closest_point_to_box(self->s.origin, self->enemy->absmin, self->enemy->absmax);

	// check that we can hit the point on the bbox
	tr = gi.traceline(self->s.origin, point, self, MASK_PROJECTILE);

	if (tr.fraction < 1) {
		if (!tr.ent->takedamage)
			return false;
		// if it will hit any client/monster then hit the one we wanted to hit
		if ((tr.ent->svflags & SVF_MONSTER) || (tr.ent->client))
			tr.ent = self->enemy;
	}

	// check that we can hit the player from the point
	tr = gi.traceline(point, self->enemy->s.origin, self, MASK_PROJECTILE);

	if (tr.fraction < 1) {
		if (!tr.ent->takedamage)
			return false;
		// if it will hit any client/monster then hit the one we wanted to hit
		if ((tr.ent->svflags & SVF_MONSTER) || (tr.ent->client))
			tr.ent = self->enemy;
	}

	AngleVectors(self->s.angles, forward, right, up);
	point = self->s.origin + (forward * range);
	point += (right * aim[1]);
	point += (up * aim[2]);
	dir = point - self->enemy->s.origin;

	// do the damage
	T_Damage(tr.ent, self, self, dir, point, vec3_origin, damage, kick / 2, DAMAGE_NO_KNOCKBACK, MOD_HIT);

	if (!(tr.ent->svflags & SVF_MONSTER) && (!tr.ent->client))
		return false;

	MS_Adjust(self->owner->client, MSTAT_HITS, 1);

	// do our special form of knockback here
	v = (self->enemy->absmin + self->enemy->absmax) * 0.5f;
	v -= point;
	v.normalize();
	self->enemy->velocity += v * kick;
	if (self->enemy->velocity[2] > 0)
		self->enemy->groundentity = nullptr;
	return true;
}

// helper routine for piercing traces;
// mask = the input mask for finding what to hit
// you can adjust the mask for the re-trace (for water, etc).
// note that you must take care in your pierce callback to mark
// the entities that are being pierced.
void pierce_trace(const vec3_t &start, const vec3_t &end, gentity_t *ignore, pierce_args_t &pierce, contents_t mask) {
	int	   loop_count = MAX_ENTITIES;
	vec3_t own_start, own_end;
	own_start = start;
	own_end = end;

	while (--loop_count) {
		pierce.tr = gi.traceline(start, own_end, ignore, mask);

		// didn't hit anything, so we're done
		if (!pierce.tr.ent || pierce.tr.fraction == 1.0f)
			return;

		// hit callback said we're done
		if (!pierce.hit(mask, own_end))
			return;

		own_start = pierce.tr.endpos;
	}

	gi.Com_Print("runaway pierce_trace\n");
}

struct fire_lead_pierce_t : pierce_args_t {
	gentity_t *self;
	vec3_t		 start;
	vec3_t		 aimdir;
	int			 damage;
	int			 kick;
	int			 hspread;
	int			 vspread;
	mod_t		 mod;
	int			 te_impact;
	contents_t   mask;
	bool	     water = false;
	vec3_t	     water_start = {};
	gentity_t *chain = nullptr;

	inline fire_lead_pierce_t(gentity_t *self, vec3_t start, vec3_t aimdir, int damage, int kick, int hspread, int vspread, mod_t mod, int te_impact, contents_t mask) :
		pierce_args_t(),
		self(self),
		start(start),
		aimdir(aimdir),
		damage(damage),
		kick(kick),
		hspread(hspread),
		vspread(vspread),
		mod(mod),
		te_impact(te_impact),
		mask(mask) {}

	// we hit an entity; return false to stop the piercing.
	// you can adjust the mask for the re-trace (for water, etc).
	bool hit(contents_t &mask, vec3_t &end) override {
		// see if we hit water
		if (tr.contents & MASK_WATER) {
			int color;

			water = true;
			water_start = tr.endpos;

			// CHECK: is this compare ever true?
			if (te_impact != -1 && start != tr.endpos) {
				if (tr.contents & CONTENTS_WATER) {
					// FIXME: this effectively does nothing..
					if (strcmp(tr.surface->name, "brwater") == 0)
						color = SPLASH_BROWN_WATER;
					else
						color = SPLASH_BLUE_WATER;
				} else if (tr.contents & CONTENTS_SLIME)
					color = SPLASH_SLIME;
				else if (tr.contents & CONTENTS_LAVA)
					color = SPLASH_LAVA;
				else
					color = SPLASH_UNKNOWN;

				if (color != SPLASH_UNKNOWN) {
					gi.WriteByte(svc_temp_entity);
					gi.WriteByte(TE_SPLASH);
					gi.WriteByte(8);
					gi.WritePosition(tr.endpos);
					gi.WriteDir(tr.plane.normal);
					gi.WriteByte(color);
					gi.multicast(tr.endpos, MULTICAST_PVS, false);
				}

				// change bullet's course when it enters water
				vec3_t dir, forward, right, up;
				dir = end - start;
				dir = vectoangles(dir);
				AngleVectors(dir, forward, right, up);
				float r = crandom() * hspread * 2;
				float u = crandom() * vspread * 2;
				end = water_start + (forward * 8192);
				end += (right * r);
				end += (up * u);
			}

			// re-trace ignoring water this time
			mask &= ~MASK_WATER;
			return true;
		}

		// did we hit an hurtable entity?
		if (tr.ent->takedamage) {
			T_Damage(tr.ent, self, self, aimdir, tr.endpos, tr.plane.normal, damage, kick, mod.id == MOD_TESLA ? DAMAGE_ENERGY : DAMAGE_BULLET, mod);

			if (self->owner)
				MS_Adjust(self->owner->client, MSTAT_HITS, 1);

			// only deadmonster is pierceable, or actual dead monsters
			// that haven't been made non-solid yet
			if ((tr.ent->svflags & SVF_DEADMONSTER) ||
				(tr.ent->health <= 0 && (tr.ent->svflags & SVF_MONSTER))) {
				if (!mark(tr.ent))
					return false;

				return true;
			}
		} else {
			// send gun puff / flash
			// don't mark the sky
			if (te_impact != -1 && !(tr.surface && ((tr.surface->flags & SURF_SKY) || strncmp(tr.surface->name, "sky", 3) == 0))) {
				gi.WriteByte(svc_temp_entity);
				gi.WriteByte(te_impact);
				gi.WritePosition(tr.endpos);
				gi.WriteDir(tr.plane.normal);
				gi.multicast(tr.endpos, MULTICAST_PVS, false);

				if (self->client)
					PlayerNoise(self, tr.endpos, PNOISE_IMPACT);
			}
		}

		// hit a solid, so we're stopping here

		return false;
	}
};

/*
=================
fire_lead

This is an internal support routine used for bullet/pellet based weapons.
=================
*/
static void fire_lead(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int damage, int kick, int te_impact, int hspread, int vspread, mod_t mod) {
	fire_lead_pierce_t args = {
		self,
		start,
		aimdir,
		damage,
		kick,
		hspread,
		vspread,
		mod,
		te_impact,
		MASK_PROJECTILE | MASK_WATER
	};

	if (self->client)
		MS_Adjust(self->client, MSTAT_SHOTS, 1);

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		args.mask &= ~CONTENTS_PLAYER;

	// special case: we started in water.
	if (gi.pointcontents(start) & MASK_WATER) {
		args.water = true;
		args.water_start = start;
		args.mask &= ~MASK_WATER;
	}

	// check initial firing position
	pierce_trace(self->s.origin, start, self, args, args.mask);

	// we're clear, so do the second pierce
	if (args.tr.fraction == 1.f) {
		args.restore();

		vec3_t end, dir, forward, right, up;
		dir = vectoangles(aimdir);
		AngleVectors(dir, forward, right, up);

		float r = crandom() * hspread;
		float u = crandom() * vspread;
		end = start + (forward * 8192);
		end += (right * r);
		end += (up * u);

		pierce_trace(args.tr.endpos, end, self, args, args.mask);
	}

	// if went through water, determine where the end is and make a bubble trail
	if (args.water && te_impact != -1) {
		vec3_t pos, dir;

		dir = args.tr.endpos - args.water_start;
		dir.normalize();
		pos = args.tr.endpos + (dir * -2);
		if (gi.pointcontents(pos) & MASK_WATER)
			args.tr.endpos = pos;
		else
			args.tr = gi.traceline(pos, args.water_start, args.tr.ent != world ? args.tr.ent : nullptr, MASK_WATER);

		pos = args.water_start + args.tr.endpos;
		pos *= 0.5f;

		gi.WriteByte(svc_temp_entity);
		gi.WriteByte(TE_BUBBLETRAIL);
		gi.WritePosition(args.water_start);
		gi.WritePosition(args.tr.endpos);
		gi.multicast(pos, MULTICAST_PVS, false);
	}
}

/*
=================
fire_bullet

Fires a single round.  Used for machinegun and chaingun.  Would be fine for
pistols, rifles, etc....
=================
*/
void fire_bullet(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int damage, int kick, int hspread, int vspread, mod_t mod) {
	fire_lead(self, start, aimdir, damage, kick, mod.id == MOD_TESLA ? -1 : TE_GUNSHOT, hspread, vspread, mod);
}

/*
=================
fire_shotgun

Shoots shotgun pellets.  Used by shotgun and super shotgun.
=================
*/
void fire_shotgun(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int damage, int kick, int hspread, int vspread, int count, mod_t mod) {
	for (int i = 0; i < count; i++)
		fire_lead(self, start, aimdir, damage, kick, TE_SHOTGUN, hspread, vspread, mod);
}

/*
=================
fire_blaster

Fires a single blaster bolt.  Used by the blaster and hyper blaster.
=================
*/
TOUCH(blaster_touch) (gentity_t *ent, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	vec3_t origin;
	if (other == ent->owner)
		return;

	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(ent);
		return;
	}

	// PMM - crash prevention
	if (ent->owner && ent->owner->client)
		PlayerNoise(ent->owner, ent->s.origin, PNOISE_IMPACT);

	// calculate position for the explosion entity
	origin = ent->s.origin + tr.plane.normal;

	if (other->takedamage) {
		T_Damage(other, ent, ent->owner, ent->velocity, ent->s.origin, tr.plane.normal, ent->dmg, 1, DAMAGE_ENERGY | DAMAGE_STAT_ONCE, static_cast<mod_id_t>(ent->style));

		MS_Adjust(ent->owner->client, MSTAT_HITS, 1);
	} else {
	}

	if (ent->splash_damage)
		T_RadiusDamage(ent, ent->owner, (float)ent->splash_damage, other, ent->splash_radius, DAMAGE_ENERGY, MOD_HYPERBLASTER);

	gi.WriteByte(svc_temp_entity);
	gi.WriteByte((ent->style != MOD_BLUEBLASTER) ? TE_BLASTER : TE_BLUEHYPERBLASTER);
	gi.WritePosition(ent->splash_damage ? origin : ent->s.origin);
	gi.WriteDir(tr.plane.normal);
	gi.multicast(ent->s.origin, MULTICAST_PHS, false);

	G_FreeEntity(ent);
}

void fire_blaster(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, effects_t effect, mod_t mod) {
	gentity_t *bolt;
	trace_t	 tr;

	bolt = G_Spawn();
	bolt->svflags = SVF_PROJECTILE;
	bolt->s.origin = start;
	bolt->s.old_origin = start;
	bolt->s.angles = vectoangles(dir);
	bolt->velocity = dir * speed;
	bolt->movetype = MOVETYPE_FLYMISSILE;
	bolt->clipmask = MASK_PROJECTILE;
	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		bolt->clipmask &= ~CONTENTS_PLAYER;
	bolt->flags |= FL_DODGE;
	bolt->solid = SOLID_BBOX;
	bolt->s.effects |= effect;
	bolt->s.modelindex = gi.modelindex("models/objects/laser/tris.md2");
	bolt->s.sound = gi.soundindex("misc/lasfly.wav");
	bolt->owner = self;
	bolt->touch = blaster_touch;
	bolt->style = mod.id;
	
	bolt->nextthink = level.time + 2_sec;
	bolt->think = G_FreeEntity;
	bolt->dmg = damage;
	if (RS(RS_Q3A) && mod.id == MOD_HYPERBLASTER) {
		bolt->s.scale = 100;
		bolt->splash_radius = 30;		//20;
		bolt->splash_damage = 20;			//15;
	}
	bolt->classname = "bolt";
	gi.linkentity(bolt);

	tr = gi.traceline(self->s.origin, bolt->s.origin, bolt, bolt->clipmask);
	if (tr.fraction < 1.0f) {
		bolt->s.origin = tr.endpos + (tr.plane.normal * 1.f);
		bolt->touch(bolt, tr.ent, tr, false);
	}
}

/*
=================
fire_greenblaster

Fires a single green blaster bolt. Used by monsters, generally.
=================
*/
static TOUCH(blaster2_touch) (gentity_t *self, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	mod_t mod;
	int	  damagestat;

	if (other == self->owner)
		return;

	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(self);
		return;
	}

	if (self->owner && self->owner->client)
		PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

	if (other->takedamage) {
		// the only time players will be firing blaster2 bolts will be from the
		// defender sphere.
		if (self->owner && self->owner->client)
			mod = MOD_DEFENDER_SPHERE;
		else
			mod = MOD_BLASTER2;

		if (self->owner) {
			damagestat = self->owner->takedamage;
			self->owner->takedamage = false;
			if (self->dmg >= 5)
				T_RadiusDamage(self, self->owner, (float)(self->dmg * 2), other, self->splash_radius, DAMAGE_ENERGY, MOD_UNKNOWN);
			T_Damage(other, self, self->owner, self->velocity, self->s.origin, tr.plane.normal, self->dmg, 1, DAMAGE_ENERGY | DAMAGE_STAT_ONCE, mod);
			self->owner->takedamage = damagestat;

			MS_Adjust(self->owner->client, MSTAT_HITS, 1);
		} else {
			if (self->dmg >= 5)
				T_RadiusDamage(self, self->owner, (float)(self->dmg * 2), other, self->splash_radius, DAMAGE_ENERGY, MOD_UNKNOWN);
			T_Damage(other, self, self->owner, self->velocity, self->s.origin, tr.plane.normal, self->dmg, 1, DAMAGE_ENERGY | DAMAGE_STAT_ONCE, mod);
		}
	} else {
		// PMM - yeowch this will get expensive
		if (self->dmg >= 5)
			T_RadiusDamage(self, self->owner, (float)(self->dmg * 2), self->owner, self->splash_radius, DAMAGE_ENERGY, MOD_UNKNOWN);

		gi.WriteByte(svc_temp_entity);
		gi.WriteByte(TE_BLASTER2);
		gi.WritePosition(self->s.origin);
		gi.WriteDir(tr.plane.normal);
		gi.multicast(self->s.origin, MULTICAST_PHS, false);
	}

	G_FreeEntity(self);
}

void fire_greenblaster(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, effects_t effect, bool hyper) {
	gentity_t *bolt;
	trace_t	 tr;

	bolt = G_Spawn();
	bolt->svflags |= SVF_PROJECTILE;
	bolt->s.origin = start;
	bolt->s.old_origin = start;
	bolt->s.angles = vectoangles(dir);
	bolt->velocity = dir * speed;
	bolt->movetype = MOVETYPE_FLYMISSILE;
	bolt->clipmask = MASK_PROJECTILE;
	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		bolt->clipmask &= ~CONTENTS_PLAYER;
	bolt->flags |= FL_DODGE;
	bolt->solid = SOLID_BBOX;
	bolt->s.effects |= effect;
	bolt->s.modelindex = gi.modelindex("models/objects/laser/tris.md2");
	bolt->owner = self;
	bolt->touch = blaster2_touch;
	if (effect)
		bolt->s.effects |= EF_TRACKER;
	bolt->splash_radius = 128;
	bolt->s.skinnum = 2;
	bolt->s.scale = 2.5f;

	bolt->nextthink = level.time + 2_sec;
	bolt->think = G_FreeEntity;
	bolt->dmg = damage;
	bolt->classname = "bolt";
	gi.linkentity(bolt);

	tr = gi.traceline(self->s.origin, bolt->s.origin, bolt, bolt->clipmask);
	if (tr.fraction < 1.0f) {
		bolt->s.origin = tr.endpos + (tr.plane.normal * 1.f);
		bolt->touch(bolt, tr.ent, tr, false);
	}
}

/*
=================
fire_blueblaster
=================
*/
void fire_blueblaster(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, effects_t effect) {
	gentity_t *bolt;
	trace_t	 tr;

	bolt = G_Spawn();
	bolt->svflags |= SVF_PROJECTILE;
	bolt->s.origin = start;
	bolt->s.old_origin = start;
	bolt->s.angles = vectoangles(dir);
	bolt->velocity = dir * speed;
	bolt->movetype = MOVETYPE_FLYMISSILE;
	bolt->clipmask = MASK_PROJECTILE;
	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		bolt->clipmask &= ~CONTENTS_PLAYER;
	bolt->flags |= FL_DODGE;
	bolt->solid = SOLID_BBOX;
	bolt->s.effects |= effect;
	bolt->s.modelindex = gi.modelindex("models/objects/laser/tris.md2");
	bolt->s.sound = gi.soundindex("misc/lasfly.wav");
	bolt->s.skinnum = 1;
	bolt->owner = self;
	bolt->touch = blaster_touch;
	bolt->style = MOD_BLUEBLASTER;

	bolt->nextthink = level.time + 2_sec;
	bolt->think = G_FreeEntity;
	bolt->dmg = damage;
	bolt->classname = "bolt";
	gi.linkentity(bolt);

	tr = gi.traceline(self->s.origin, bolt->s.origin, bolt, bolt->clipmask);
	if (tr.fraction < 1.0f) {
		bolt->s.origin = tr.endpos + (tr.plane.normal * 1.f);
		bolt->touch(bolt, tr.ent, tr, false);
	}
}

constexpr spawnflags_t SPAWNFLAG_GRENADE_HAND = 1_spawnflag;
constexpr spawnflags_t SPAWNFLAG_GRENADE_HELD = 2_spawnflag;

/*
=================
fire_grenade
=================
*/
static THINK(Grenade_Explode) (gentity_t *ent) -> void {
	vec3_t origin;
	mod_t  mod;

	if (ent->owner->client)
		PlayerNoise(ent->owner, ent->s.origin, PNOISE_IMPACT);

	// FIXME: if we are onground then raise our Z just a bit since we are a point?
	if (ent->enemy) {
		float  points;
		vec3_t v;
		vec3_t dir;

		v = ent->enemy->mins + ent->enemy->maxs;
		v = ent->enemy->s.origin + (v * 0.5f);
		v = ent->s.origin - v;
		points = ent->dmg - 0.5f * v.length();
		dir = ent->enemy->s.origin - ent->s.origin;
		if (ent->spawnflags.has(SPAWNFLAG_GRENADE_HAND))
			mod = MOD_HANDGRENADE;
		else
			mod = MOD_GRENADE;
		T_Damage(ent->enemy, ent, ent->owner, dir, ent->s.origin, vec3_origin, (int)points, (int)points, DAMAGE_RADIUS | DAMAGE_STAT_ONCE, mod);

		MS_Adjust(ent->owner->client, MSTAT_HITS, 1);
	}

	if (ent->spawnflags.has(SPAWNFLAG_GRENADE_HELD))
		mod = MOD_HELD_GRENADE;
	else if (ent->spawnflags.has(SPAWNFLAG_GRENADE_HAND))
		mod = MOD_HG_SPLASH;
	else
		mod = MOD_G_SPLASH;
	T_RadiusDamage(ent, ent->owner, (float)ent->dmg, ent->enemy, ent->splash_radius, DAMAGE_NONE | DAMAGE_STAT_ONCE, mod);

	origin = ent->s.origin + (ent->velocity * -0.02f);
	gi.WriteByte(svc_temp_entity);
	if (ent->waterlevel) {
		if (ent->groundentity)
			gi.WriteByte(TE_GRENADE_EXPLOSION_WATER);
		else
			gi.WriteByte(TE_ROCKET_EXPLOSION_WATER);
	} else {
		if (ent->groundentity)
			gi.WriteByte(TE_GRENADE_EXPLOSION);
		else
			gi.WriteByte(TE_ROCKET_EXPLOSION);
	}
	gi.WritePosition(origin);
	gi.multicast(ent->s.origin, MULTICAST_PHS, false);

	G_FreeEntity(ent);
}

static TOUCH(Grenade_Touch) (gentity_t *ent, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	if (other == ent->owner)
		return;

	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(ent);
		return;
	}

	if (!other->takedamage) {
		if (ent->spawnflags.has(SPAWNFLAG_GRENADE_HAND)) {
			if (frandom() > 0.5f)
				gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/hgrenb1a.wav"), 1, ATTN_NORM, 0);
			else
				gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/hgrenb2a.wav"), 1, ATTN_NORM, 0);
		} else {
			gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/grenlb1b.wav"), 1, ATTN_NORM, 0);
		}
		return;
	}

	ent->enemy = other;
	Grenade_Explode(ent);
}

static THINK(Grenade4_Think) (gentity_t *self) -> void {
	if (level.time >= self->timestamp) {
		Grenade_Explode(self);
		return;
	}

	if (self->velocity) {
		float p = self->s.angles.x;
		float z = self->s.angles.z;
		float speed_frac = clamp(self->velocity.lengthSquared() / (self->speed * self->speed), 0.f, 1.f);
		self->s.angles = vectoangles(self->velocity);
		self->s.angles.x = LerpAngle(p, self->s.angles.x, speed_frac);
		self->s.angles.z = z + (gi.frame_time_s * 360 * speed_frac);
	}

	self->nextthink = level.time + FRAME_TIME_S;
}

void fire_grenade(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int damage, int speed, gtime_t timer, float damage_radius, float right_adjust, float up_adjust, bool monster) {
	gentity_t *grenade;
	vec3_t	 dir;
	vec3_t	 forward, right, up;

	dir = vectoangles(aimdir);
	AngleVectors(dir, forward, right, up);

	grenade = G_Spawn();
	grenade->s.origin = start;
	grenade->velocity = aimdir * speed;

	if (up_adjust) {
		float gravityAdjustment = level.gravity / 800.f;
		grenade->velocity += up * up_adjust * gravityAdjustment;
	}

	if (right_adjust)
		grenade->velocity += right * right_adjust;

	grenade->movetype = MOVETYPE_BOUNCE;
	grenade->clipmask = MASK_PROJECTILE;
	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		grenade->clipmask &= ~CONTENTS_PLAYER;
	grenade->solid = SOLID_BBOX;
	grenade->svflags |= SVF_PROJECTILE;
	grenade->flags |= (FL_DODGE | FL_TRAP);
	grenade->s.effects |= EF_GRENADE;
	grenade->speed = speed;
	grenade->s.scale = 1.25f;

	if (monster) {
		grenade->avelocity = { crandom() * 360, crandom() * 360, crandom() * 360 };
		grenade->s.modelindex = gi.modelindex("models/objects/grenade/tris.md2");
		grenade->nextthink = level.time + timer;
		grenade->think = Grenade_Explode;
		grenade->s.effects |= EF_GRENADE_LIGHT;
	} else {
		grenade->s.modelindex = gi.modelindex("models/objects/grenade4/tris.md2");
		grenade->s.angles = vectoangles(grenade->velocity);
		grenade->nextthink = level.time + FRAME_TIME_S;
		grenade->timestamp = level.time + timer;
		grenade->think = Grenade4_Think;
		grenade->s.renderfx |= RF_MINLIGHT;
	}
	grenade->owner = self;
	grenade->touch = Grenade_Touch;
	grenade->dmg = damage;
	grenade->splash_radius = damage_radius;
	grenade->classname = "grenade";

	gi.linkentity(grenade);
}

void fire_handgrenade(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int damage, int speed, gtime_t timer, float damage_radius, bool held) {
	gentity_t *grenade;
	vec3_t	 dir;
	vec3_t	 forward, right, up;

	dir = vectoangles(aimdir);
	AngleVectors(dir, forward, right, up);

	grenade = G_Spawn();
	grenade->s.origin = start;
	grenade->velocity = aimdir * speed;

	float gravityAdjustment = level.gravity / 800.f;

	grenade->velocity += up * (200 + crandom() * 10.0f) * gravityAdjustment;
	grenade->velocity += right * (crandom() * 10.0f);

	grenade->avelocity = { crandom() * 360, crandom() * 360, crandom() * 360 };
	grenade->movetype = MOVETYPE_BOUNCE;
	grenade->clipmask = MASK_PROJECTILE;
	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		grenade->clipmask &= ~CONTENTS_PLAYER;

	grenade->flags |= (FL_DODGE | FL_TRAP);

	grenade->solid = SOLID_BBOX;
	grenade->svflags |= SVF_PROJECTILE;

	grenade->s.effects |= EF_GRENADE;
	grenade->s.modelindex = gi.modelindex("models/objects/grenade3/tris.md2");
	grenade->s.scale = 1.25f;

	grenade->owner = self;
	grenade->touch = Grenade_Touch;
	grenade->nextthink = level.time + timer;
	grenade->think = Grenade_Explode;
	grenade->dmg = damage;
	grenade->splash_radius = damage_radius;
	grenade->classname = "hand_grenade";
	grenade->spawnflags = SPAWNFLAG_GRENADE_HAND;
	if (held)
		grenade->spawnflags |= SPAWNFLAG_GRENADE_HELD;
	grenade->s.sound = gi.soundindex("weapons/hgrenc1b.wav");

	if (timer <= 0_ms)
		Grenade_Explode(grenade);
	else {
		gi.sound(self, CHAN_WEAPON, gi.soundindex("weapons/hgrent1a.wav"), 1, ATTN_NORM, 0);
		gi.linkentity(grenade);
	}
}

/*
=================
fire_rocket
=================
*/
TOUCH(rocket_touch) (gentity_t *ent, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	vec3_t origin;
	if (other == ent->owner)
		return;

	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(ent);
		return;
	}

	if (ent->owner->client)
		PlayerNoise(ent->owner, ent->s.origin, PNOISE_IMPACT);

	// calculate position for the explosion entity
	origin = ent->s.origin + tr.plane.normal;

	if (other->takedamage) {
		T_Damage(other, ent, ent->owner, ent->velocity, ent->s.origin, tr.plane.normal, ent->dmg, RS(RS_MM) ? 50 : 0, DAMAGE_NONE | DAMAGE_STAT_ONCE, MOD_ROCKET);

		MS_Adjust(ent->owner->client, MSTAT_HITS, 1);
	} else {
		// don't throw any debris in net games
		if (!deathmatch->integer && !coop->integer) {
			if (tr.surface && !(tr.surface->flags & (SURF_WARP | SURF_TRANS33 | SURF_TRANS66 | SURF_FLOWING))) {
				ThrowGibs(ent, 2, {
					{ (size_t)irandom(5), "models/objects/debris2/tris.md2", GIB_METALLIC | GIB_DEBRIS }
					});
			}
		}
	}

	T_RadiusDamage(ent, ent->owner, (float)ent->splash_damage, other, ent->splash_radius, DAMAGE_NONE, MOD_R_SPLASH);

	gi.WriteByte(svc_temp_entity);
	if (ent->waterlevel)
		gi.WriteByte(TE_ROCKET_EXPLOSION_WATER);
	else
		gi.WriteByte(TE_ROCKET_EXPLOSION);
	gi.WritePosition(origin);
	gi.multicast(ent->s.origin, MULTICAST_PHS, false);

	G_FreeEntity(ent);
}

gentity_t *fire_rocket(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, float damage_radius, int radius_damage) {
	gentity_t *rocket;

	rocket = G_Spawn();
	rocket->s.origin = start;
	rocket->s.angles = vectoangles(dir);
	rocket->velocity = dir * speed;
	rocket->movetype = MOVETYPE_FLYMISSILE;
	rocket->svflags |= SVF_PROJECTILE;
	rocket->flags |= FL_DODGE;
	rocket->clipmask = MASK_PROJECTILE;
	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		rocket->clipmask &= ~CONTENTS_PLAYER;
	rocket->solid = SOLID_BBOX;
	rocket->s.effects |= EF_ROCKET;
	rocket->s.modelindex = gi.modelindex("models/objects/rocket/tris.md2");
	rocket->owner = self;
	rocket->touch = rocket_touch;
	rocket->nextthink = level.time + gtime_t::from_sec(8000.f / speed);
	rocket->think = G_FreeEntity;
	rocket->dmg = damage;
	rocket->splash_damage = radius_damage;
	rocket->splash_radius = damage_radius;
	rocket->s.sound = gi.soundindex("weapons/rockfly.wav");
	rocket->classname = "rocket";

	gi.linkentity(rocket);

	return rocket;
}

using search_callback_t = decltype(game_import_t::inPVS);

static bool binary_positional_search_r(const vec3_t &viewer, const vec3_t &start, const vec3_t &end, search_callback_t cb, int32_t split_num) {
	// check half-way point
	vec3_t mid = (start + end) * 0.5f;

	if (cb(viewer, mid, true))
		return true;

	// no more splits
	if (!split_num)
		return false;

	// recursively check both sides
	return binary_positional_search_r(viewer, start, mid, cb, split_num - 1) || binary_positional_search_r(viewer, mid, end, cb, split_num - 1);
}

// [Paril-KEX] simple binary search through a line to see if any points along
// the line (in a binary split) pass the callback
static bool binary_positional_search(const vec3_t &viewer, const vec3_t &start, const vec3_t &end, search_callback_t cb, int32_t num_splits) {
	// check start/end first
	if (cb(viewer, start, true) || cb(viewer, end, true))
		return true;

	// recursive split
	return binary_positional_search_r(viewer, start, end, cb, num_splits);
}

struct fire_rail_pierce_t : pierce_args_t {
	gentity_t *self;
	vec3_t	 aimdir;
	int		 damage;
	int		 kick;
	bool	 water = false;

	inline fire_rail_pierce_t(gentity_t *self, vec3_t aimdir, int damage, int kick) :
		pierce_args_t(),
		self(self),
		aimdir(aimdir),
		damage(damage),
		kick(kick) {}

	// we hit an entity; return false to stop the piercing.
	// you can adjust the mask for the re-trace (for water, etc).
	bool hit(contents_t &mask, vec3_t &end) override {
		if (tr.contents & (CONTENTS_SLIME | CONTENTS_LAVA)) {
			mask &= ~(CONTENTS_SLIME | CONTENTS_LAVA);
			water = true;
			return true;
		} else {
			// try to kill it first
			if ((tr.ent != self) && (tr.ent->takedamage))
				T_Damage(tr.ent, self, self, aimdir, tr.endpos, tr.plane.normal, damage, kick, DAMAGE_NONE | DAMAGE_STAT_ONCE, MOD_RAILGUN);

			// dead, so we don't need to care about checking pierce
			if (!tr.ent->inuse || (!tr.ent->solid || tr.ent->solid == SOLID_TRIGGER))
				return true;

			// rail goes through SOLID_BBOX entities (gibs, etc)
			if ((tr.ent->svflags & SVF_MONSTER) || (tr.ent->client) || (tr.ent->flags & FL_DAMAGEABLE) || (tr.ent->solid == SOLID_BBOX)) {
				if (!mark(tr.ent))
					return false;

				return true;
			}
		}

		return false;
	}
};

// [Paril-KEX] get the current unique unicast key
uint32_t GetUnicastKey() {
	static uint32_t key = 1;

	if (!key)
		return key = 1;

	return key++;
}

/*
=================
fire_rail
=================
*/
void fire_rail(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int damage, int kick) {
	fire_rail_pierce_t args = {
		self,
		aimdir,
		damage,
		kick
	};

	contents_t mask = MASK_PROJECTILE | CONTENTS_SLIME | CONTENTS_LAVA;

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		mask &= ~CONTENTS_PLAYER;

	vec3_t end = start + (aimdir * 8192);

	pierce_trace(start, end, self, args, mask);

	uint32_t unicast_key = GetUnicastKey();

	// send gun puff / flash
	// [Paril-KEX] this often makes double noise, so trying
	// a slightly different approach...
	for (auto player : active_clients()) {
		vec3_t org = player->s.origin + player->client->ps.viewoffset + vec3_t{ 0, 0, (float)player->client->ps.pmove.viewheight };

		if (binary_positional_search(org, start, args.tr.endpos, gi.inPHS, 3)) {
			gi.WriteByte(svc_temp_entity);
			gi.WriteByte((deathmatch->integer && g_instagib->integer) ? TE_RAILTRAIL2 : TE_RAILTRAIL);
			gi.WritePosition(start);
			gi.WritePosition(args.tr.endpos);
			gi.unicast(player, false, unicast_key);
		}
	}

	if (g_instagib->integer && g_instagib_splash->integer) {
		gentity_t *exp;

		exp = G_Spawn();
		exp->classname = "railsplash";
		exp->s.origin = args.tr.endpos;
		exp->s.angles = vectoangles(aimdir);
		exp->clipmask = MASK_PROJECTILE;
		exp->owner = self;
		exp->dmg = 180;
		exp->splash_damage = 120;
		exp->splash_radius = 120;

		gi.linkentity(exp);

		T_RadiusDamage(exp, exp->owner, exp->dmg, nullptr, exp->splash_radius, DAMAGE_NONE, MOD_RAILGUN_SPLASH);

		gi.WriteByte(svc_temp_entity);
		if (exp->waterlevel)
			gi.WriteByte(TE_ROCKET_EXPLOSION_WATER);
		else
			gi.WriteByte(TE_ROCKET_EXPLOSION);
		gi.WritePosition(exp->s.origin);
		gi.multicast(exp->s.origin, MULTICAST_PHS, false);

		G_FreeEntity(exp);

	}

	if (self->client)
		PlayerNoise(self, args.tr.endpos, PNOISE_IMPACT);
}

static vec3_t bfg_laser_pos(vec3_t p, float dist) {
	float theta = frandom(2 * PIf);
	float phi = acos(crandom());

	vec3_t d{
		sin(phi) * cos(theta),
		sin(phi) * sin(theta),
		cos(phi)
	};

	return p + (d * dist);
}

static THINK(bfg_laser_update) (gentity_t *self) -> void {
	if (level.time > self->timestamp || !self->owner->inuse) {
		G_FreeEntity(self);
		return;
	}

	self->s.origin = self->owner->s.origin;
	self->nextthink = level.time + 1_ms;
	gi.linkentity(self);
}

static void bfg_spawn_laser(gentity_t *self) {
	vec3_t end = bfg_laser_pos(self->s.origin, 256);
	trace_t tr = gi.traceline(self->s.origin, end, self, MASK_OPAQUE);

	if (tr.fraction == 1.0f)
		return;

	gentity_t *laser = G_Spawn();
	laser->s.frame = 3;
	laser->s.renderfx = RF_BEAM_LIGHTNING;
	laser->movetype = MOVETYPE_NONE;
	laser->solid = SOLID_NOT;
	laser->s.modelindex = MODELINDEX_WORLD; // must be non-zero
	laser->s.origin = self->s.origin;
	laser->s.old_origin = tr.endpos;
	laser->s.skinnum = 0xD0D0D0D0;
	laser->think = bfg_laser_update;
	laser->nextthink = level.time + 1_ms;
	laser->timestamp = level.time + 300_ms;
	laser->owner = self;
	gi.linkentity(laser);
}

/*
=================
fire_bfg
=================
*/
static THINK(bfg_explode) (gentity_t *self) -> void {
	gentity_t *ent;
	float	 points;
	vec3_t	 v;
	float	 dist;

	bfg_spawn_laser(self);

	if (self->s.frame == 0) {
		// the BFG effect
		ent = nullptr;
		while ((ent = findradius(ent, self->s.origin, self->splash_radius)) != nullptr) {
			if (!ent->takedamage)
				continue;
			if (ent == self->owner)
				continue;
			if (ent->client && ent->client->eliminated)
				continue;
			if (!CanDamage(ent, self))
				continue;
			if (!CanDamage(ent, self->owner))
				continue;
			// make tesla hurt by bfg
			if (!(ent->svflags & SVF_MONSTER) && !(ent->flags & FL_DAMAGEABLE) && (!ent->client) && (strcmp(ent->classname, "misc_explobox") != 0))
				continue;
			// don't target team mates during teamplay if we can't damage them
			if (CheckTeamDamage(ent, self->owner))
				continue;

			v = ent->mins + ent->maxs;
			v = ent->s.origin + (v * 0.5f);
			vec3_t centroid = v;
			v = self->s.origin - centroid;
			dist = v.length();
			points = self->splash_damage * (1.0f - sqrtf(dist / self->splash_radius));

			T_Damage(ent, self, self->owner, self->velocity, centroid, vec3_origin, (int)points, 0, DAMAGE_ENERGY | DAMAGE_STAT_ONCE, MOD_BFG_EFFECT);

			// Paril: draw BFG lightning laser to enemies
			gi.WriteByte(svc_temp_entity);
			gi.WriteByte(TE_BFG_ZAP);
			gi.WritePosition(self->s.origin);
			gi.WritePosition(centroid);
			gi.multicast(self->s.origin, MULTICAST_PHS, false);
		}
	}

	self->nextthink = level.time + 10_hz;
	self->s.frame++;
	if (self->s.frame == 5)
		self->think = G_FreeEntity;
}

static TOUCH(bfg_touch) (gentity_t *self, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	if (other == self->owner)
		return;

	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(self);
		return;
	}

	if (self->owner->client)
		PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

	// core explosion - prevents firing it into the wall/floor
	if (other->takedamage)
		T_Damage(other, self, self->owner, self->velocity, self->s.origin, tr.plane.normal, 200, 0, DAMAGE_ENERGY, MOD_BFG_BLAST);
	T_RadiusDamage(self, self->owner, 200, other, 100, DAMAGE_ENERGY | DAMAGE_STAT_ONCE, MOD_BFG_BLAST);

	gi.sound(self, CHAN_VOICE, gi.soundindex("weapons/bfg__x1b.wav"), 1, ATTN_NORM, 0);
	self->solid = SOLID_NOT;
	self->touch = nullptr;
	self->s.origin += self->velocity * (-1 * gi.frame_time_s);
	self->velocity = {};
	self->s.modelindex = gi.modelindex("sprites/s_bfg3.sp2");
	self->s.frame = 0;
	self->s.sound = 0;
	self->s.effects &= ~EF_ANIM_ALLFAST;
	self->think = bfg_explode;
	self->nextthink = level.time + 10_hz;
	self->enemy = other;

	gi.WriteByte(svc_temp_entity);
	gi.WriteByte(TE_BFG_BIGEXPLOSION);
	gi.WritePosition(self->s.origin);
	gi.multicast(self->s.origin, MULTICAST_PHS, false);
}


struct bfg_laser_pierce_t : pierce_args_t {
	gentity_t *self;
	vec3_t	 dir;
	int		 damage;

	inline bfg_laser_pierce_t(gentity_t *self, vec3_t dir, int damage) :
		pierce_args_t(),
		self(self),
		dir(dir),
		damage(damage) {}

	// we hit an entity; return false to stop the piercing.
	// you can adjust the mask for the re-trace (for water, etc).
	bool hit(contents_t &mask, vec3_t &end) override {
		// hurt it if we can
		if ((tr.ent->takedamage) && !(tr.ent->flags & FL_IMMUNE_LASER) && (tr.ent != self->owner))
			T_Damage(tr.ent, self, self->owner, dir, tr.endpos, vec3_origin, damage, 1, DAMAGE_ENERGY, MOD_BFG_LASER);

		// if we hit something that's not a monster or player we're done
		if (!(tr.ent->svflags & SVF_MONSTER) && !(tr.ent->flags & FL_DAMAGEABLE) && (!tr.ent->client)) {
			gi.WriteByte(svc_temp_entity);
			gi.WriteByte(TE_LASER_SPARKS);
			gi.WriteByte(4);
			gi.WritePosition(tr.endpos);
			gi.WriteDir(tr.plane.normal);
			gi.WriteByte(self->s.skinnum);
			gi.multicast(tr.endpos, MULTICAST_PVS, false);
			return false;
		}

		if (!mark(tr.ent))
			return false;

		return true;
	}
};

static THINK(bfg_think) (gentity_t *self) -> void {
	gentity_t *ent;
	vec3_t	 point;
	vec3_t	 dir;
	vec3_t	 start;
	vec3_t	 end;
	int		 dmg;
	trace_t	 tr;

	dmg = deathmatch->integer ? 5 : 10;

	bfg_spawn_laser(self);

	ent = nullptr;
	while ((ent = findradius(ent, self->s.origin, 256)) != nullptr) {
		if (ent == self)
			continue;
		if (ent == self->owner)
			continue;
		if (ent->client && ent->client->eliminated)
			continue;
		if (!ent->takedamage)
			continue;

		// make tesla hurt by bfg
		if (!(ent->svflags & SVF_MONSTER) && !(ent->flags & FL_DAMAGEABLE) && (!ent->client) && (strcmp(ent->classname, "misc_explobox") != 0))
			continue;
		// don't target team mates during teamplay if we can't damage them
		if (CheckTeamDamage(ent, self->owner))
			continue;

		point = (ent->absmin + ent->absmax) * 0.5f;

		dir = point - self->s.origin;
		dir.normalize();

		start = self->s.origin;
		end = start + (dir * 2048);

		// [Paril-KEX] don't fire a laser if we're blocked by the world
		tr = gi.traceline(start, point, nullptr, MASK_SOLID);

		if (tr.fraction < 1.0f)
			continue;

		bfg_laser_pierce_t args{
			self,
			dir,
			dmg
		};

		pierce_trace(start, end, self, args, CONTENTS_SOLID | CONTENTS_MONSTER | CONTENTS_PLAYER | CONTENTS_DEADMONSTER);

		gi.WriteByte(svc_temp_entity);
		gi.WriteByte(TE_BFG_LASER);
		gi.WritePosition(self->s.origin);
		gi.WritePosition(tr.endpos);
		gi.multicast(self->s.origin, MULTICAST_PHS, false);
	}

	self->nextthink = level.time + 10_hz;
}

void fire_bfg(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, float damage_radius) {
	gentity_t *bfg;

	bfg = G_Spawn();
	bfg->s.origin = start;
	bfg->s.angles = vectoangles(dir);
	bfg->velocity = dir * speed;
	bfg->movetype = MOVETYPE_FLYMISSILE;
	bfg->clipmask = MASK_PROJECTILE;
	bfg->svflags = SVF_PROJECTILE;
	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		bfg->clipmask &= ~CONTENTS_PLAYER;
	bfg->solid = SOLID_BBOX;
	bfg->s.effects |= EF_BFG | EF_ANIM_ALLFAST;
	bfg->s.modelindex = gi.modelindex("sprites/s_bfg1.sp2");
	bfg->owner = self;
	bfg->touch = bfg_touch;
	bfg->nextthink = level.time + gtime_t::from_sec(8000.f / speed);
	bfg->think = G_FreeEntity;
	bfg->splash_damage = damage;
	bfg->splash_radius = damage_radius;
	bfg->classname = "bfg blast";
	bfg->s.sound = gi.soundindex("weapons/bfg__l1a.wav");

	bfg->think = bfg_think;
	bfg->nextthink = level.time + FRAME_TIME_S;
	bfg->teammaster = bfg;
	bfg->teamchain = nullptr;

	gi.linkentity(bfg);
}

static TOUCH(disintegrator_touch) (gentity_t *self, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	gi.WriteByte(svc_temp_entity);
	gi.WriteByte(TE_WIDOWSPLASH);
	gi.WritePosition(self->s.origin - (self->velocity * 0.01f));
	gi.multicast(self->s.origin, MULTICAST_PHS, false);

	G_FreeEntity(self);

	if (other->svflags & (SVF_MONSTER | SVF_PLAYER)) {
		other->disintegrator_time += 50_sec;
		other->disintegrator = self->owner;
	}
}

void fire_disintegrator(gentity_t *self, const vec3_t &start, const vec3_t &forward, int speed) {
	gentity_t *bfg;

	bfg = G_Spawn();
	bfg->s.origin = start;
	bfg->s.angles = vectoangles(forward);
	bfg->velocity = forward * speed;
	bfg->movetype = MOVETYPE_FLYMISSILE;
	bfg->clipmask = MASK_PROJECTILE;
	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		bfg->clipmask &= ~CONTENTS_PLAYER;
	bfg->solid = SOLID_BBOX;
	bfg->s.effects |= EF_TAGTRAIL | EF_ANIM_ALL;
	bfg->s.renderfx |= RF_TRANSLUCENT;
	bfg->svflags |= SVF_PROJECTILE;
	bfg->flags |= FL_DODGE;
	bfg->s.modelindex = gi.modelindex("sprites/s_bfg1.sp2");
	bfg->owner = self;
	bfg->touch = disintegrator_touch;
	bfg->nextthink = level.time + gtime_t::from_sec(8000.f / speed);
	bfg->think = G_FreeEntity;
	bfg->classname = "disint ball";
	bfg->s.sound = gi.soundindex("weapons/bfg__l1a.wav");

	gi.linkentity(bfg);
}

// *************************
//  PLASMA BEAM
// *************************

static void fire_beams(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, const vec3_t &offset, int damage, int kick, int te_beam, int te_impact, mod_t mod) {
	trace_t	   tr;
	vec3_t	   dir;
	vec3_t	   forward, right, up;
	vec3_t	   end;
	vec3_t	   water_start, endpoint;
	bool	   water = false, underwater = false;
	contents_t content_mask = MASK_PROJECTILE | MASK_WATER;

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		content_mask &= ~CONTENTS_PLAYER;

	dir = vectoangles(aimdir);
	AngleVectors(dir, forward, right, up);

	float dist = RS(RS_MM) ? 768 : 8192;
	end = start + (forward * dist);

	if (gi.pointcontents(start) & MASK_WATER) {
		underwater = true;
		water_start = start;
		content_mask &= ~MASK_WATER;
	}

	tr = gi.traceline(start, end, self, content_mask);

	// see if we hit water
	if (tr.contents & MASK_WATER) {
		water = true;
		water_start = tr.endpos;

		if (start != tr.endpos) {
			gi.WriteByte(svc_temp_entity);
			gi.WriteByte(TE_HEATBEAM_SPARKS);
			gi.WritePosition(water_start);
			gi.WriteDir(tr.plane.normal);
			gi.multicast(tr.endpos, MULTICAST_PVS, false);
		}
		// re-trace ignoring water this time
		tr = gi.traceline(water_start, end, self, content_mask & ~MASK_WATER);
	}
	endpoint = tr.endpos;

	// halve the damage if target underwater
	if (water)
		damage = damage / 2;

	// send gun puff / flash
	if (!((tr.surface) && (tr.surface->flags & SURF_SKY))) {
		if (tr.fraction < 1.0f) {
			if (tr.ent->takedamage) {
				T_Damage(tr.ent, self, self, aimdir, tr.endpos, tr.plane.normal, damage, kick, DAMAGE_ENERGY, mod);
			} else {
				if ((!water) && !(tr.surface && (tr.surface->flags & SURF_SKY))) {
					// This is the truncated steam entry - uses 1+1+2 extra bytes of data
					gi.WriteByte(svc_temp_entity);
					gi.WriteByte(TE_HEATBEAM_STEAM);
					gi.WritePosition(tr.endpos);
					gi.WriteDir(tr.plane.normal);
					gi.multicast(tr.endpos, MULTICAST_PVS, false);

					if (self->client)
						PlayerNoise(self, tr.endpos, PNOISE_IMPACT);
				}
			}
		}
	}

	// if went through water, determine where the end and make a bubble trail
	if ((water) || (underwater)) {
		vec3_t pos;

		dir = tr.endpos - water_start;
		dir.normalize();
		pos = tr.endpos + (dir * -2);
		if (gi.pointcontents(pos) & MASK_WATER)
			tr.endpos = pos;
		else
			tr = gi.traceline(pos, water_start, tr.ent, MASK_WATER);

		pos = water_start + tr.endpos;
		pos *= 0.5f;

		gi.WriteByte(svc_temp_entity);
		gi.WriteByte(TE_BUBBLETRAIL2);
		gi.WritePosition(water_start);
		gi.WritePosition(tr.endpos);
		gi.multicast(pos, MULTICAST_PVS, false);
	}

	gi.WriteByte(svc_temp_entity);
	gi.WriteByte(te_beam);
	gi.WriteEntity(self);
	gi.WritePosition(start);
	gi.WritePosition(!underwater && !water ? tr.endpos : endpoint);
	gi.multicast(self->s.origin, MULTICAST_ALL, false);
}

/*
=================
fire_plasmabeam

Fires a single heat beam.
=================
*/
void fire_plasmabeam(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, const vec3_t &offset, int damage, int kick, bool monster) {
	if (monster)
		fire_beams(self, start, aimdir, offset, damage, kick, TE_MONSTER_HEATBEAM, TE_HEATBEAM_SPARKS, MOD_PLASMABEAM);
	else
		fire_beams(self, start, aimdir, offset, damage, kick, TE_HEATBEAM, TE_HEATBEAM_SPARKS, MOD_PLASMABEAM);
}

// *************************
// DISRUPTOR
// *************************

constexpr damageflags_t DISRUPTOR_DAMAGE_FLAGS = (DAMAGE_NO_POWER_ARMOR | DAMAGE_ENERGY | DAMAGE_NO_KNOCKBACK);
constexpr damageflags_t DISRUPTOR_IMPACT_FLAGS = (DAMAGE_NO_POWER_ARMOR | DAMAGE_ENERGY);

constexpr gtime_t DISRUPTOR_DAMAGE_TIME = 500_ms;

static THINK(disruptor_pain_daemon_think) (gentity_t *self) -> void {
	constexpr vec3_t pain_normal = { 0, 0, 1 };
	int				 hurt;

	if (!self->inuse)
		return;

	if ((level.time - self->timestamp) > DISRUPTOR_DAMAGE_TIME) {
		if (!self->enemy->client)
			self->enemy->s.effects &= ~EF_TRACKERTRAIL;
		G_FreeEntity(self);
	} else {
		if (self->enemy->health > 0) {
			vec3_t center = (self->enemy->absmax + self->enemy->absmin) * 0.5f;

			T_Damage(self->enemy, self, self->owner, vec3_origin, center, pain_normal,
				self->dmg, 0, DISRUPTOR_DAMAGE_FLAGS | DAMAGE_STAT_ONCE, MOD_TRACKER);

			// if we kill the player, we'll be removed.
			if (self->inuse) {
				// if we killed a monster, gib them.
				if (self->enemy->health < 1) {
					if (self->enemy->gib_health)
						hurt = -self->enemy->gib_health;
					else
						hurt = 500;

					T_Damage(self->enemy, self, self->owner, vec3_origin, center,
						pain_normal, hurt, 0, DISRUPTOR_DAMAGE_FLAGS | DAMAGE_STAT_ONCE, MOD_TRACKER);
				}

				self->nextthink = level.time + 10_hz;

				if (self->enemy->client)
					self->enemy->client->tracker_pain_time = self->nextthink;
				else
					self->enemy->s.effects |= EF_TRACKERTRAIL;
			}
		} else {
			if (!self->enemy->client)
				self->enemy->s.effects &= ~EF_TRACKERTRAIL;
			G_FreeEntity(self);
		}
	}
}

static void disruptor_pain_daemon_spawn(gentity_t *owner, gentity_t *enemy, int damage) {
	gentity_t *daemon;

	if (enemy == nullptr)
		return;

	daemon = G_Spawn();
	daemon->classname = "pain daemon";
	daemon->think = disruptor_pain_daemon_think;
	daemon->nextthink = level.time;
	daemon->timestamp = level.time;
	daemon->owner = owner;
	daemon->enemy = enemy;
	daemon->dmg = damage;
}

static void tracker_explode(gentity_t *self) {
	gi.WriteByte(svc_temp_entity);
	gi.WriteByte(TE_TRACKER_EXPLOSION);
	gi.WritePosition(self->s.origin);
	gi.multicast(self->s.origin, MULTICAST_PHS, false);

	G_FreeEntity(self);
}

static TOUCH(disruptor_touch) (gentity_t *self, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	float damagetime;

	if (other == self->owner)
		return;

	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(self);
		return;
	}

	if (self->client)
		PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

	if (other->takedamage) {
		if ((other->svflags & SVF_MONSTER) || other->client) {
			if (other->health > 0) // knockback only for living creatures
			{
				// PMM - kickback was times 4 .. reduced to 3
				// now this does no damage, just knockback
				T_Damage(other, self, self->owner, self->velocity, self->s.origin, tr.plane.normal,
					/* self->dmg */ 0, (self->dmg * 3), DISRUPTOR_IMPACT_FLAGS | DAMAGE_STAT_ONCE, MOD_TRACKER);

				if (!(other->flags & (FL_FLY | FL_SWIM)))
					other->velocity[2] += 140;

				damagetime = ((float)self->dmg) * 0.1f;
				damagetime = damagetime / DISRUPTOR_DAMAGE_TIME.seconds();

				disruptor_pain_daemon_spawn(self->owner, other, (int)damagetime);
			} else // lots of damage (almost autogib) for dead bodies
			{
				T_Damage(other, self, self->owner, self->velocity, self->s.origin, tr.plane.normal,
					self->dmg * 4, (self->dmg * 3), DISRUPTOR_IMPACT_FLAGS | DAMAGE_STAT_ONCE, MOD_TRACKER);
			}
		} else // full damage in one shot for inanimate objects
		{
			T_Damage(other, self, self->owner, self->velocity, self->s.origin, tr.plane.normal,
				self->dmg, (self->dmg * 3), DISRUPTOR_IMPACT_FLAGS | DAMAGE_STAT_ONCE, MOD_TRACKER);
		}
	}

	tracker_explode(self);
	return;
}

static THINK(disruptor_fly) (gentity_t *self) -> void {
	vec3_t dest;
	vec3_t dir;
	vec3_t center;

	if ((!self->enemy) || (!self->enemy->inuse) || (self->enemy->health < 1)) {
		tracker_explode(self);
		return;
	}

	// PMM - try to hunt for center of enemy, if possible and not client
	if (self->enemy->client) {
		dest = self->enemy->s.origin;
		dest[2] += self->enemy->viewheight;
	}
	// paranoia
	else if (!self->enemy->absmin || !self->enemy->absmax) {
		dest = self->enemy->s.origin;
	} else {
		center = (self->enemy->absmin + self->enemy->absmax) * 0.5f;
		dest = center;
	}

	dir = dest - self->s.origin;
	dir.normalize();
	self->s.angles = vectoangles(dir);
	self->velocity = dir * self->speed;
	self->monsterinfo.saved_goal = dest;

	self->nextthink = level.time + 10_hz;
}

void fire_disruptor(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, gentity_t *enemy) {
	gentity_t *bolt;
	trace_t	 tr;

	bolt = G_Spawn();
	bolt->s.origin = start;
	bolt->s.old_origin = start;
	bolt->s.angles = vectoangles(dir);
	bolt->velocity = dir * speed;
	bolt->svflags |= SVF_PROJECTILE;
	bolt->movetype = MOVETYPE_FLYMISSILE;
	bolt->clipmask = MASK_PROJECTILE;

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		bolt->clipmask &= ~CONTENTS_PLAYER;

	bolt->solid = SOLID_BBOX;
	bolt->speed = (float)speed;
	bolt->s.effects = EF_TRACKER;
	bolt->s.sound = gi.soundindex("weapons/disrupt.wav");
	bolt->s.modelindex = gi.modelindex("models/proj/disintegrator/tris.md2");
	bolt->touch = disruptor_touch;
	bolt->enemy = enemy;
	bolt->owner = self;
	bolt->dmg = damage;
	bolt->classname = "tracker";
	gi.linkentity(bolt);

	if (enemy) {
		bolt->nextthink = level.time + 10_hz;
		bolt->think = disruptor_fly;
	} else {
		bolt->nextthink = level.time + 10_sec;
		bolt->think = G_FreeEntity;
	}

	tr = gi.traceline(self->s.origin, bolt->s.origin, bolt, bolt->clipmask);
	if (tr.fraction < 1.0f) {
		bolt->s.origin = tr.endpos + (tr.plane.normal * 1.f);
		bolt->touch(bolt, tr.ent, tr, false);
	}
}

/*
========================
fire_flechette
========================
*/
static TOUCH(flechette_touch) (gentity_t *self, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	if (other == self->owner)
		return;

	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(self);
		return;
	}

	if (self->client)
		PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

	if (other->takedamage) {
		T_Damage(other, self, self->owner, self->velocity, self->s.origin, tr.plane.normal,
			self->dmg, (int)self->splash_radius, DAMAGE_NO_REG_ARMOR | DAMAGE_STAT_ONCE, MOD_ETF_RIFLE);
	} else {
		gi.WriteByte(svc_temp_entity);
		gi.WriteByte(TE_FLECHETTE);
		gi.WritePosition(self->s.origin);
		gi.WriteDir(tr.plane.normal);
		gi.multicast(self->s.origin, MULTICAST_PHS, false);
	}

	G_FreeEntity(self);
}

void fire_flechette(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, int kick) {
	gentity_t *flechette;

	flechette = G_Spawn();
	flechette->s.origin = start;
	flechette->s.old_origin = start;
	flechette->s.angles = vectoangles(dir);
	flechette->velocity = dir * speed;
	flechette->svflags |= SVF_PROJECTILE;
	flechette->movetype = MOVETYPE_FLYMISSILE;
	flechette->clipmask = MASK_PROJECTILE;
	flechette->flags |= FL_DODGE;

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		flechette->clipmask &= ~CONTENTS_PLAYER;

	flechette->solid = SOLID_BBOX;
	flechette->s.renderfx = RF_FULLBRIGHT;
	flechette->s.modelindex = gi.modelindex("models/proj/flechette/tris.md2");

	flechette->owner = self;
	flechette->touch = flechette_touch;
	flechette->nextthink = level.time + gtime_t::from_sec(8000.f / speed);
	flechette->think = G_FreeEntity;
	flechette->dmg = damage;
	flechette->splash_radius = (float)kick;

	gi.linkentity(flechette);

	trace_t tr = gi.traceline(self->s.origin, flechette->s.origin, flechette, flechette->clipmask);
	if (tr.fraction < 1.0f) {
		flechette->s.origin = tr.endpos + (tr.plane.normal * 1.f);
		flechette->touch(flechette, tr.ent, tr, false);
	}
}

// **************************
// PROX
// **************************

constexpr gtime_t PROX_TIME_TO_LIVE = 45_sec;
constexpr gtime_t PROX_TIME_DELAY = 500_ms;
constexpr float	  PROX_BOUND_SIZE = 96;
constexpr float	  PROX_DAMAGE_RADIUS = 192;
constexpr int32_t PROX_HEALTH = 20;
constexpr int32_t PROX_DAMAGE = 90;

static THINK(Prox_Explode) (gentity_t *ent) -> void {
	vec3_t	 origin;
	gentity_t *owner;

	// free the trigger field

	// PMM - changed teammaster to "mover" .. owner of the field is the prox
	if (ent->teamchain && ent->teamchain->owner == ent)
		G_FreeEntity(ent->teamchain);

	owner = ent;
	if (ent->teammaster) {
		owner = ent->teammaster;
		PlayerNoise(owner, ent->s.origin, PNOISE_IMPACT);
	}

	// play quad sound if appopriate
	if (ent->dmg > PROX_DAMAGE)
		gi.sound(ent, CHAN_ITEM, gi.soundindex("items/damage3.wav"), 1, ATTN_NORM, 0);

	ent->takedamage = false;
	T_RadiusDamage(ent, owner, (float)ent->dmg, ent, PROX_DAMAGE_RADIUS, DAMAGE_NONE, MOD_PROX);

	origin = ent->s.origin + (ent->velocity * -0.02f);
	gi.WriteByte(svc_temp_entity);
	if (ent->groundentity)
		gi.WriteByte(TE_GRENADE_EXPLOSION);
	else
		gi.WriteByte(TE_ROCKET_EXPLOSION);
	gi.WritePosition(origin);
	gi.multicast(ent->s.origin, MULTICAST_PHS, false);

	G_FreeEntity(ent);
}

static DIE(prox_die) (gentity_t *self, gentity_t *inflictor, gentity_t *attacker, int damage, const vec3_t &point, const mod_t &mod) -> void {
	// if set off by another prox, delay a little (chained explosions)
	if (strcmp(inflictor->classname, "prox_mine")) {
		self->takedamage = false;
		Prox_Explode(self);
	} else {
		self->takedamage = false;
		self->think = Prox_Explode;
		self->nextthink = level.time + FRAME_TIME_S;
	}
}

static TOUCH(Prox_Field_Touch) (gentity_t *ent, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	gentity_t *prox;

	if (deathmatch->integer && IsCombatDisabled())
		return;

	if (!(other->svflags & SVF_MONSTER) && !other->client)
		return;

	// trigger the prox mine if it's still there, and still mine.
	prox = ent->owner;

	// teammate avoidance
	if (CheckTeamDamage(prox->teammaster, other))
		return;

	if (!deathmatch->integer && other->client)
		return;

	if (other == prox) // don't set self off
		return;

	if (prox->think == Prox_Explode) // we're set to blow!
		return;

	if (prox->teamchain == ent) {
		gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/proxwarn.wav"), 1, ATTN_NORM, 0);
		prox->think = Prox_Explode;
		prox->nextthink = level.time + PROX_TIME_DELAY;
		return;
	}

	ent->solid = SOLID_NOT;
	G_FreeEntity(ent);
}

static THINK(prox_seek) (gentity_t *ent) -> void {
	if (level.time > gtime_t::from_sec(ent->wait)) {
		Prox_Explode(ent);
	} else {
		ent->s.frame++;
		if (ent->s.frame > 13)
			ent->s.frame = 9;
		ent->think = prox_seek;
		ent->nextthink = level.time + 10_hz;
	}
}

static THINK(prox_open) (gentity_t *ent) -> void {
	gentity_t *search;

	search = nullptr;

	if (ent->s.frame == 9) // end of opening animation
	{
		// set the owner to nullptr so the owner can walk through it.  needs to be done here so the owner
		// doesn't get stuck on it while it's opening if fired at point blank wall
		ent->s.sound = 0;

		if (deathmatch->integer)
			ent->owner = nullptr;

		if (ent->teamchain)
			ent->teamchain->touch = Prox_Field_Touch;
		while ((search = findradius(search, ent->s.origin, PROX_DAMAGE_RADIUS + 10)) != nullptr) {
			if (!search->classname) // tag token and other weird shit
				continue;

			// teammate avoidance
			if (CheckTeamDamage(search, ent->teammaster))
				continue;

			// if it's a monster or player with health > 0
			// or it's a player start point
			// and we can see it
			// blow up
			if (
				search != ent &&
				(
					(((search->svflags & SVF_MONSTER) || (deathmatch->integer && (search->client || (search->classname && !strcmp(search->classname, "prox_mine"))))) && (search->health > 0)) ||
					(deathmatch->integer &&
						((!strncmp(search->classname, "info_player_", 12)) ||
							(!strcmp(search->classname, "misc_teleporter_dest")) ||
							(!strncmp(search->classname, "item_flag_", 10))))) &&
				(visible(search, ent))) {
				gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/proxwarn.wav"), 1, ATTN_NORM, 0);
				Prox_Explode(ent);
				return;
			}
		}

		if (g_dm_strong_mines->integer)
			ent->wait = (level.time + PROX_TIME_TO_LIVE).seconds();
		else {
			switch (ent->dmg / PROX_DAMAGE) {
			case 1:
				ent->wait = (level.time + PROX_TIME_TO_LIVE).seconds();
				break;
			case 2:
				ent->wait = (level.time + 30_sec).seconds();
				break;
			case 4:
				ent->wait = (level.time + 15_sec).seconds();
				break;
			case 8:
				ent->wait = (level.time + 10_sec).seconds();
				break;
			default:
				ent->wait = (level.time + PROX_TIME_TO_LIVE).seconds();
				break;
			}
		}

		ent->think = prox_seek;
		ent->nextthink = level.time + 200_ms;
	} else {
		if (ent->s.frame == 0)
			gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/proxopen.wav"), 1, ATTN_NORM, 0);
		ent->s.frame++;
		ent->think = prox_open;
		ent->nextthink = level.time + 10_hz;
	}
}

static TOUCH(prox_land) (gentity_t *ent, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	gentity_t *field;
	vec3_t	   dir;
	vec3_t	   forward, right, up;
	movetype_t movetype = MOVETYPE_NONE;
	int		   stick_ok = 0;
	vec3_t	   land_point;

	// must turn off owner so owner can shoot it and set it off
	// moved to prox_open so owner can get away from it if fired at pointblank range into
	// wall
	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(ent);
		return;
	}

	if (tr.plane.normal) {
		land_point = ent->s.origin + (tr.plane.normal * -10.0f);
		if (gi.pointcontents(land_point) & (CONTENTS_SLIME | CONTENTS_LAVA)) {
			Prox_Explode(ent);
			return;
		}
	}

	constexpr float PROX_STOP_EPSILON = 0.1f;

	if (!tr.plane.normal || (other->svflags & SVF_MONSTER) || other->client || (other->flags & FL_DAMAGEABLE)) {
		if (other != ent->teammaster)
			Prox_Explode(ent);

		return;
	} else if (other != world) {
		// Here we need to check to see if we can stop on this entity.
		// Note that plane can be nullptr

		// PMM - code stolen from g_phys (ClipVelocity)
		vec3_t out;
		float  backoff, change;
		int	   i;

		if ((other->movetype == MOVETYPE_PUSH) && (tr.plane.normal[2] > 0.7f))
			stick_ok = 1;
		else
			stick_ok = 0;

		backoff = ent->velocity.dot(tr.plane.normal) * 1.5f;
		for (i = 0; i < 3; i++) {
			change = tr.plane.normal[i] * backoff;
			out[i] = ent->velocity[i] - change;
			if (out[i] > -PROX_STOP_EPSILON && out[i] < PROX_STOP_EPSILON)
				out[i] = 0;
		}

		if (out[2] > 60)
			return;

		movetype = MOVETYPE_BOUNCE;

		// if we're here, we're going to stop on an entity
		if (stick_ok) { // it's a happy entity
			ent->velocity = {};
			ent->avelocity = {};
		} else // no-stick.  teflon time
		{
			if (tr.plane.normal[2] > 0.7f) {
				Prox_Explode(ent);
				return;
			}
			return;
		}
	} else if (other->s.modelindex != MODELINDEX_WORLD)
		return;

	dir = vectoangles(tr.plane.normal);
	AngleVectors(dir, forward, right, up);

	if (gi.pointcontents(ent->s.origin) & (CONTENTS_LAVA | CONTENTS_SLIME)) {
		Prox_Explode(ent);
		return;
	}

	ent->svflags &= ~SVF_PROJECTILE;

	field = G_Spawn();

	field->s.origin = ent->s.origin;
	field->mins = { -PROX_BOUND_SIZE, -PROX_BOUND_SIZE, -PROX_BOUND_SIZE };
	field->maxs = { PROX_BOUND_SIZE, PROX_BOUND_SIZE, PROX_BOUND_SIZE };
	field->movetype = MOVETYPE_NONE;
	field->solid = SOLID_TRIGGER;
	field->owner = ent;
	field->classname = "prox_field";
	field->teammaster = ent;
	gi.linkentity(field);

	ent->velocity = {};
	ent->avelocity = {};
	// rotate to vertical
	dir[PITCH] = dir[PITCH] + 90;
	ent->s.angles = dir;
	ent->takedamage = true;
	ent->movetype = movetype; // either bounce or none, depending on whether we stuck to something
	ent->die = prox_die;
	ent->teamchain = field;
	ent->health = PROX_HEALTH;
	ent->nextthink = level.time;
	ent->think = prox_open;
	ent->touch = nullptr;
	ent->solid = SOLID_BBOX;

	gi.linkentity(ent);
}

static THINK(Prox_Think) (gentity_t *self) -> void {
	if (self->timestamp <= level.time) {
		Prox_Explode(self);
		return;
	}

	self->s.angles = vectoangles(self->velocity.normalized());
	self->s.angles[PITCH] -= 90;
	self->nextthink = level.time;
}

void fire_prox(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int prox_damage_multiplier, int speed) {
	gentity_t *prox;
	vec3_t	 dir;
	vec3_t	 forward, right, up;

	dir = vectoangles(aimdir);
	AngleVectors(dir, forward, right, up);

	prox = G_Spawn();
	prox->s.origin = start;
	prox->velocity = aimdir * speed;

	float gravityAdjustment = level.gravity / 800.f;

	prox->velocity += up * (200 + crandom() * 10.0f) * gravityAdjustment;
	prox->velocity += right * (crandom() * 10.0f);

	prox->s.angles = dir;
	prox->s.angles[PITCH] -= 90;
	prox->movetype = MOVETYPE_BOUNCE;
	prox->solid = SOLID_BBOX;
	prox->svflags |= SVF_PROJECTILE;
	prox->s.effects |= EF_GRENADE;
	prox->flags |= (FL_DODGE | FL_TRAP);
	prox->clipmask = MASK_PROJECTILE | CONTENTS_LAVA | CONTENTS_SLIME;

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		prox->clipmask &= ~CONTENTS_PLAYER;

	prox->s.renderfx |= RF_IR_VISIBLE;
	// FIXME - this needs to be bigger.  Has other effects, though.  Maybe have to change origin to compensate
	//  so it sinks in correctly.  Also in lavacheck, might have to up the distance
	prox->mins = { -6, -6, -6 };
	prox->maxs = { 6, 6, 6 };
	prox->s.modelindex = gi.modelindex("models/weapons/g_prox/tris.md2");
	prox->owner = self;
	prox->teammaster = self;
	prox->touch = prox_land;
	prox->think = Prox_Think;
	prox->nextthink = level.time;
	prox->dmg = PROX_DAMAGE * prox_damage_multiplier;
	prox->classname = "prox_mine";
	prox->flags |= FL_DAMAGEABLE;
	prox->flags |= FL_MECHANICAL;

	switch (prox_damage_multiplier) {
	case 1:
		prox->timestamp = level.time + PROX_TIME_TO_LIVE;
		break;
	case 2:
		prox->timestamp = level.time + 30_sec;
		break;
	case 4:
		prox->timestamp = level.time + 15_sec;
		break;
	case 8:
		prox->timestamp = level.time + 10_sec;
		break;
	default:
		prox->timestamp = level.time + PROX_TIME_TO_LIVE;
		break;
	}

	gi.linkentity(prox);
}

// *************************
// MELEE WEAPONS
// *************************

struct player_melee_data_t {
	gentity_t *self;
	const vec3_t &start;
	const vec3_t &aim;
	int reach;
};

static BoxEntitiesResult_t fire_player_melee_BoxFilter(gentity_t *check, void *data_v) {
	const player_melee_data_t *data = (const player_melee_data_t *)data_v;

	if (!check->inuse || !check->takedamage || check == data->self)
		return BoxEntitiesResult_t::Skip;

	// check distance
	vec3_t closest_point_to_check = closest_point_to_box(data->start, check->s.origin + check->mins, check->s.origin + check->maxs);
	vec3_t closest_point_to_self = closest_point_to_box(closest_point_to_check, data->self->s.origin + data->self->mins, data->self->s.origin + data->self->maxs);

	vec3_t dir = (closest_point_to_check - closest_point_to_self);
	float len = dir.normalize();

	if (len > data->reach)
		return BoxEntitiesResult_t::Skip;

	// check angle if we aren't intersecting
	vec3_t shrink{ 2, 2, 2 };
	if (!boxes_intersect(check->absmin + shrink, check->absmax - shrink, data->self->absmin + shrink, data->self->absmax - shrink)) {
		dir = (((check->absmin + check->absmax) / 2) - data->start).normalized();

		if (dir.dot(data->aim) < 0.70f)
			return BoxEntitiesResult_t::Skip;
	}

	return BoxEntitiesResult_t::Keep;
}

bool fire_player_melee(gentity_t *self, const vec3_t &start, const vec3_t &aim, int reach, int damage, int kick, mod_t mod) {
	constexpr size_t MAX_HIT = 4;

	vec3_t reach_vec{ float(reach - 1), float(reach - 1), float(reach - 1) };
	gentity_t *targets[MAX_HIT];

	player_melee_data_t data{
		self,
		start,
		aim,
		reach
	};

	// find all the things we could maybe hit
	size_t num = gi.BoxEntities(self->absmin - reach_vec, self->absmax + reach_vec, targets, q_countof(targets), AREA_SOLID, fire_player_melee_BoxFilter, &data);

	if (!num)
		return false;

	bool was_hit = false;

	for (size_t i = 0; i < num; i++) {
		gentity_t *hit = targets[i];

		if (!hit->inuse || !hit->takedamage)
			continue;
		else if (!CanDamage(self, hit))
			continue;

		// do the damage
		vec3_t closest_point_to_check = closest_point_to_box(start, hit->s.origin + hit->mins, hit->s.origin + hit->maxs);

		if (hit->svflags & SVF_MONSTER)
			hit->pain_debounce_time -= random_time(5_ms, 75_ms);

		if (mod.id == MOD_CHAINFIST)
			T_Damage(hit, self, self, aim, closest_point_to_check, -aim, damage, kick / 2,
				DAMAGE_DESTROY_ARMOR | DAMAGE_NO_KNOCKBACK, mod);
		else
			T_Damage(hit, self, self, aim, closest_point_to_check, -aim, damage, kick / 2, DAMAGE_NO_KNOCKBACK, mod);

		was_hit = true;
	}

	return was_hit;
}

// *************************
// NUKE
// *************************

constexpr gtime_t NUKE_DELAY = 4_sec;
constexpr gtime_t NUKE_TIME_TO_LIVE = 6_sec;
constexpr float	  NUKE_RADIUS = 512;
constexpr int32_t NUKE_DAMAGE = 400;
constexpr gtime_t NUKE_QUAKE_TIME = 3_sec;
constexpr float	  NUKE_QUAKE_STRENGTH = 100;

static THINK(Nuke_Quake) (gentity_t *self) -> void {
	uint32_t i;
	gentity_t *e;

	if (self->last_move_time < level.time) {
		gi.positioned_sound(self->s.origin, self, CHAN_AUTO, self->noise_index, 0.75, ATTN_NONE, 0);
		self->last_move_time = level.time + 500_ms;
	}

	for (i = 1, e = g_entities + i; i < globals.num_entities; i++, e++) {
		if (!e->inuse)
			continue;
		if (!e->client)
			continue;
		if (!e->groundentity)
			continue;

		e->groundentity = nullptr;
		e->velocity[0] += crandom() * 150;
		e->velocity[1] += crandom() * 150;
		e->velocity[2] = self->speed * (100.0f / e->mass);
	}

	if (level.time < self->timestamp)
		self->nextthink = level.time + FRAME_TIME_S;
	else
		G_FreeEntity(self);
}

void Nuke_Explode(gentity_t *ent) {
	float dmg = ent->dmg;
	float splash_radius = ent->splash_radius;

	if (!dmg)
		dmg = 400;

	if (!splash_radius)
		dmg = 512;

	if (ent->teammaster->client)
		PlayerNoise(ent->teammaster, ent->s.origin, PNOISE_IMPACT);

	T_RadiusNukeDamage(ent, ent->teammaster, dmg, ent, splash_radius, MOD_NUKE);

	if (ent->dmg > NUKE_DAMAGE)
		gi.sound(ent, CHAN_ITEM, gi.soundindex("items/damage3.wav"), 1, ATTN_NORM, 0);

	gi.sound(ent, CHAN_NO_PHS_ADD | CHAN_VOICE, gi.soundindex("weapons/grenlx1a.wav"), 1, ATTN_NONE, 0);

	gi.WriteByte(svc_temp_entity);
	gi.WriteByte(TE_EXPLOSION1_BIG);
	gi.WritePosition(ent->s.origin);
	gi.multicast(ent->s.origin, MULTICAST_PHS, false);

	gi.WriteByte(svc_temp_entity);
	gi.WriteByte(TE_NUKEBLAST);
	gi.WritePosition(ent->s.origin);
	gi.multicast(ent->s.origin, MULTICAST_ALL, false);

	// become a quake
	ent->svflags |= SVF_NOCLIENT;
	ent->noise_index = gi.soundindex("world/rumble.wav");
	ent->think = Nuke_Quake;
	ent->speed = NUKE_QUAKE_STRENGTH;
	ent->timestamp = level.time + NUKE_QUAKE_TIME;
	ent->nextthink = level.time + FRAME_TIME_S;
	ent->last_move_time = 0_ms;
}

static DIE(nuke_die) (gentity_t *self, gentity_t *inflictor, gentity_t *attacker, int damage, const vec3_t &point, const mod_t &mod) -> void {
	self->takedamage = false;
	if ((attacker) && !(strcmp(attacker->classname, "nuke"))) {
		G_FreeEntity(self);
		return;
	}
	Nuke_Explode(self);
}

static THINK(Nuke_Think) (gentity_t *ent) -> void {
	float			attenuation, default_atten = 1.8f;
	int				nuke_damage_multiplier;
	player_muzzle_t muzzleflash;

	nuke_damage_multiplier = ent->dmg / NUKE_DAMAGE;
	switch (nuke_damage_multiplier) {
	case 1:
		attenuation = default_atten / 1.4f;
		muzzleflash = MZ_NUKE1;
		break;
	case 2:
		attenuation = default_atten / 2.0f;
		muzzleflash = MZ_NUKE2;
		break;
	case 4:
		attenuation = default_atten / 3.0f;
		muzzleflash = MZ_NUKE4;
		break;
	case 8:
		attenuation = default_atten / 5.0f;
		muzzleflash = MZ_NUKE8;
		break;
	default:
		attenuation = default_atten;
		muzzleflash = MZ_NUKE1;
		break;
	}

	if (ent->wait < level.time.seconds())
		Nuke_Explode(ent);
	else if (level.time >= (gtime_t::from_sec(ent->wait) - NUKE_TIME_TO_LIVE)) {
		ent->s.frame++;

		if (ent->s.frame > 11)
			ent->s.frame = 6;

		if (gi.pointcontents(ent->s.origin) & (CONTENTS_SLIME | CONTENTS_LAVA)) {
			Nuke_Explode(ent);
			return;
		}

		ent->think = Nuke_Think;
		ent->nextthink = level.time + 10_hz;
		ent->health = 1;
		ent->owner = nullptr;

		gi.WriteByte(svc_muzzleflash);
		gi.WriteEntity(ent);
		gi.WriteByte(muzzleflash);
		gi.multicast(ent->s.origin, MULTICAST_PHS, false);

		if (ent->timestamp <= level.time) {
			if ((gtime_t::from_sec(ent->wait) - level.time) <= (NUKE_TIME_TO_LIVE / 2.0f)) {
				gi.sound(ent, CHAN_NO_PHS_ADD | CHAN_VOICE, gi.soundindex("weapons/nukewarn2.wav"), 1, attenuation, 0);
				ent->timestamp = level.time + 300_ms;
			} else {
				gi.sound(ent, CHAN_NO_PHS_ADD | CHAN_VOICE, gi.soundindex("weapons/nukewarn2.wav"), 1, attenuation, 0);
				ent->timestamp = level.time + 500_ms;
			}
		}
	} else {
		if (ent->timestamp <= level.time) {
			gi.sound(ent, CHAN_NO_PHS_ADD | CHAN_VOICE, gi.soundindex("weapons/nukewarn2.wav"), 1, attenuation, 0);
			ent->timestamp = level.time + 1_sec;
		}
		ent->nextthink = level.time + FRAME_TIME_S;
	}
}

static TOUCH(nuke_bounce) (gentity_t *ent, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	if (tr.surface && tr.surface->id) {
		if (frandom() > 0.5f)
			gi.sound(ent, CHAN_BODY, gi.soundindex("weapons/hgrenb1a.wav"), 1, ATTN_NORM, 0);
		else
			gi.sound(ent, CHAN_BODY, gi.soundindex("weapons/hgrenb2a.wav"), 1, ATTN_NORM, 0);
	}
}

void fire_nuke(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int speed) {
	gentity_t *nuke;
	vec3_t	 dir;
	vec3_t	 forward, right, up;
	int		 damage_modifier = P_DamageModifier(self);

	dir = vectoangles(aimdir);
	AngleVectors(dir, forward, right, up);

	nuke = G_Spawn();
	nuke->s.origin = start;
	nuke->velocity = aimdir * speed;
	nuke->velocity += up * (200 + crandom() * 10.0f);
	nuke->velocity += right * (crandom() * 10.0f);
	nuke->movetype = MOVETYPE_BOUNCE;
	nuke->clipmask = MASK_PROJECTILE;
	nuke->solid = SOLID_BBOX;
	nuke->s.effects |= EF_GRENADE;
	nuke->s.renderfx |= RF_IR_VISIBLE;
	nuke->mins = { -8, -8, 0 };
	nuke->maxs = { 8, 8, 16 };
	nuke->s.modelindex = gi.modelindex("models/weapons/g_nuke/tris.md2");
	nuke->owner = self;
	nuke->teammaster = self;
	nuke->nextthink = level.time + FRAME_TIME_S;
	nuke->wait = (level.time + NUKE_DELAY + NUKE_TIME_TO_LIVE).seconds();
	nuke->think = Nuke_Think;
	nuke->touch = nuke_bounce;

	nuke->health = 10000;
	nuke->takedamage = true;
	nuke->flags |= FL_DAMAGEABLE;
	nuke->dmg = NUKE_DAMAGE * damage_modifier;
	if (damage_modifier == 1)
		nuke->splash_radius = NUKE_RADIUS;
	else
		nuke->splash_radius = NUKE_RADIUS + NUKE_RADIUS * (0.25f * (float)damage_modifier);
	// this yields 1.0, 1.5, 2.0, 3.0 times radius

	nuke->classname = "nuke";
	nuke->die = nuke_die;

	gi.linkentity(nuke);
}

// *************************
// TESLA
// *************************

constexpr gtime_t TESLA_TIME_TO_LIVE = 30_sec;
constexpr float	  TESLA_DAMAGE_RADIUS = 128;
constexpr int32_t TESLA_DAMAGE = 3;
constexpr int32_t TESLA_KNOCKBACK = 8;

constexpr gtime_t TESLA_ACTIVATE_TIME = 3_sec;

constexpr int32_t TESLA_EXPLOSION_DAMAGE_MULT = 50; // this is the amount the damage is multiplied by for underwater explosions
constexpr float	  TESLA_EXPLOSION_RADIUS = 200;

static void tesla_remove(gentity_t *self) {
	gentity_t *cur, *next;

	self->takedamage = false;
	if (self->teamchain) {
		cur = self->teamchain;
		while (cur) {
			next = cur->teamchain;
			G_FreeEntity(cur);
			cur = next;
		}
	} else if (self->air_finished)
		gi.Com_Print("tesla_mine without a field!\n");

	self->owner = self->teammaster; // Going away, set the owner correctly.
	// grenade explode does damage to self->enemy
	self->enemy = nullptr;

	// play quad sound if quadded and an underwater explosion
	if ((self->splash_radius) && (self->dmg > (TESLA_DAMAGE * TESLA_EXPLOSION_DAMAGE_MULT)))
		gi.sound(self, CHAN_ITEM, gi.soundindex("items/damage3.wav"), 1, ATTN_NORM, 0);

	Grenade_Explode(self);
}

static DIE(tesla_die) (gentity_t *self, gentity_t *inflictor, gentity_t *attacker, int damage, const vec3_t &point, const mod_t &mod) -> void {
	tesla_remove(self);
}

static void tesla_blow(gentity_t *self) {
	self->dmg *= TESLA_EXPLOSION_DAMAGE_MULT;
	self->splash_radius = TESLA_EXPLOSION_RADIUS;
	tesla_remove(self);
}

static TOUCH(tesla_zap) (gentity_t *self, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {}

static BoxEntitiesResult_t tesla_think_active_BoxFilter(gentity_t *check, void *data) {
	gentity_t *self = (gentity_t *)data;

	if (!check->inuse)
		return BoxEntitiesResult_t::Skip;
	if (check == self)
		return BoxEntitiesResult_t::Skip;
	if (check->health < 1)
		return BoxEntitiesResult_t::Skip;
	// don't hit teammates
	if (check->client) {
		if (!deathmatch->integer)
			return BoxEntitiesResult_t::Skip;
		else if (CheckTeamDamage(check, self->teammaster))
			return BoxEntitiesResult_t::Skip;
	}
	if (!(check->svflags & SVF_MONSTER) && !(check->flags & FL_DAMAGEABLE) && !check->client)
		return BoxEntitiesResult_t::Skip;

	// don't hit other teslas in SP/coop
	if (!deathmatch->integer && check->classname && (check->flags & FL_TRAP))
		return BoxEntitiesResult_t::Skip;

	return BoxEntitiesResult_t::Keep;
}

static THINK(tesla_think_active) (gentity_t *self) -> void {
	size_t	 num;
	static gentity_t *touch[MAX_ENTITIES];
	gentity_t *hit;
	vec3_t	 dir, start;
	trace_t	 tr;

	if (level.time > self->air_finished) {
		tesla_remove(self);
		return;
	}

	if (deathmatch->integer && IsCombatDisabled())
		return;

	start = self->s.origin;
	start[2] += 16;

	num = gi.BoxEntities(self->teamchain->absmin, self->teamchain->absmax, touch, MAX_ENTITIES, AREA_SOLID, tesla_think_active_BoxFilter, self);
	for (size_t i = 0; i < num; i++) {
		// if the tesla died while zapping things, stop zapping.
		if (!(self->inuse))
			break;

		hit = touch[i];
		if (!hit->inuse)
			continue;
		if (hit == self)
			continue;
		if (hit->health < 1)
			continue;
		// don't hit teammates
		if (hit->client) {
			if (!deathmatch->integer)
				continue;
			else if (CheckTeamDamage(hit, self->teamchain->owner))
				continue;
		}
		if (!(hit->svflags & SVF_MONSTER) && !(hit->flags & FL_DAMAGEABLE) && !hit->client)
			continue;

		tr = gi.traceline(start, hit->s.origin, self, MASK_PROJECTILE);
		if (tr.fraction == 1 || tr.ent == hit) {
			dir = hit->s.origin - start;

			// PMM - play quad sound if it's above the "normal" damage
			if (self->dmg > TESLA_DAMAGE)
				gi.sound(self, CHAN_ITEM, gi.soundindex("items/damage3.wav"), 1, ATTN_NORM, 0);

			// PGM - don't do knockback to walking monsters
			if ((hit->svflags & SVF_MONSTER) && !(hit->flags & (FL_FLY | FL_SWIM)))
				T_Damage(hit, self, self->teammaster, dir, tr.endpos, tr.plane.normal,
					self->dmg, 0, DAMAGE_NONE | DAMAGE_STAT_ONCE, MOD_TESLA);
			else
				T_Damage(hit, self, self->teammaster, dir, tr.endpos, tr.plane.normal,
					self->dmg, TESLA_KNOCKBACK, DAMAGE_NONE | DAMAGE_STAT_ONCE, MOD_TESLA);

			gi.WriteByte(svc_temp_entity);
			gi.WriteByte(TE_LIGHTNING);
			gi.WriteEntity(self);	// source entity
			gi.WriteEntity(hit); // destination entity
			gi.WritePosition(start);
			gi.WritePosition(tr.endpos);
			gi.multicast(start, MULTICAST_PVS, false);
		}
	}

	if (self->inuse) {
		self->think = tesla_think_active;
		self->nextthink = level.time + 10_hz;
	}
}

static THINK(tesla_activate) (gentity_t *self) -> void {
	gentity_t *trigger, *search;

	if (gi.pointcontents(self->s.origin) & (CONTENTS_SLIME | CONTENTS_LAVA | CONTENTS_WATER)) {
		tesla_blow(self);
		return;
	}

	// only check for spawn points in deathmatch
	if (deathmatch->integer) {
		search = nullptr;
		while ((search = findradius(search, self->s.origin, 1.5f * TESLA_DAMAGE_RADIUS)) != nullptr) {
			// [Paril-KEX] don't allow traps to be placed near flags or teleporters
			// if it's a monster or player with health > 0
			// or it's a player start point
			// and we can see it
			// blow up
			if (search->classname && ((deathmatch->integer &&
				((!strncmp(search->classname, "info_player_", 12)) ||
					(!strcmp(search->classname, "misc_teleporter_dest")) ||
					(!strncmp(search->classname, "item_flag_", 10))))) &&
				(visible(search, self))) {
				BecomeExplosion1(self);
				return;
			}
		}
	}

	trigger = G_Spawn();
	trigger->s.origin = self->s.origin;
	trigger->mins = { -TESLA_DAMAGE_RADIUS, -TESLA_DAMAGE_RADIUS, self->mins[2] };
	trigger->maxs = { TESLA_DAMAGE_RADIUS, TESLA_DAMAGE_RADIUS, TESLA_DAMAGE_RADIUS };
	trigger->movetype = MOVETYPE_NONE;
	trigger->solid = SOLID_TRIGGER;
	trigger->owner = self;
	trigger->touch = tesla_zap;
	trigger->classname = "tesla trigger";
	// doesn't need to be marked as a teamslave since the move code for bounce looks for teamchains
	gi.linkentity(trigger);

	self->s.angles = {};
	// clear the owner if in deathmatch
	if (deathmatch->integer)
		self->owner = nullptr;
	self->teamchain = trigger;
	self->think = tesla_think_active;
	self->nextthink = level.time + FRAME_TIME_S;
	self->air_finished = level.time + TESLA_TIME_TO_LIVE;
}

static THINK(tesla_think) (gentity_t *ent) -> void {
	if (gi.pointcontents(ent->s.origin) & (CONTENTS_SLIME | CONTENTS_LAVA)) {
		tesla_remove(ent);
		return;
	}

	ent->s.angles = {};

	if (!(ent->s.frame))
		gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/teslaopen.wav"), 1, ATTN_NORM, 0);

	ent->s.frame++;
	if (ent->s.frame > 14) {
		ent->s.frame = 14;
		ent->think = tesla_activate;
		ent->nextthink = level.time + 10_hz;
	} else {
		if (ent->s.frame > 9) {
			if (ent->s.frame == 10) {
				if (ent->owner && ent->owner->client) {
					PlayerNoise(ent->owner, ent->s.origin, PNOISE_WEAPON);
				}
				ent->s.skinnum = 1;
			} else if (ent->s.frame == 12)
				ent->s.skinnum = 2;
			else if (ent->s.frame == 14)
				ent->s.skinnum = 3;
		}
		ent->think = tesla_think;
		ent->nextthink = level.time + 10_hz;
	}
}

static TOUCH(tesla_lava) (gentity_t *ent, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	if (tr.contents & (CONTENTS_SLIME | CONTENTS_LAVA)) {
		tesla_blow(ent);
		return;
	}

	if (ent->velocity) {
		if (frandom() > 0.5f)
			gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/hgrenb1a.wav"), 1, ATTN_NORM, 0);
		else
			gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/hgrenb2a.wav"), 1, ATTN_NORM, 0);
	}
}

void fire_tesla(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int tesla_damage_multiplier, int speed) {
	gentity_t *tesla;
	vec3_t	 dir;
	vec3_t	 forward, right, up;

	dir = vectoangles(aimdir);
	AngleVectors(dir, forward, right, up);

	tesla = G_Spawn();
	tesla->s.origin = start;
	tesla->velocity = aimdir * speed;

	float gravityAdjustment = level.gravity / 800.f;

	tesla->velocity += up * (200 + crandom() * 10.0f) * gravityAdjustment;
	tesla->velocity += right * (crandom() * 10.0f);

	tesla->s.angles = {};
	tesla->movetype = MOVETYPE_BOUNCE;
	tesla->solid = SOLID_BBOX;
	tesla->s.effects |= EF_GRENADE;
	tesla->s.renderfx |= RF_IR_VISIBLE;
	tesla->mins = { -12, -12, 0 };
	tesla->maxs = { 12, 12, 20 };
	tesla->s.modelindex = gi.modelindex("models/weapons/g_tesla/tris.md2");

	tesla->owner = self; // PGM - we don't want it owned by self YET.
	tesla->teammaster = self;

	tesla->wait = (level.time + TESLA_TIME_TO_LIVE).seconds();
	tesla->think = tesla_think;
	tesla->nextthink = level.time + TESLA_ACTIVATE_TIME;

	// blow up on contact with lava & slime code
	tesla->touch = tesla_lava;

	tesla->health = deathmatch->integer ? 20 : 50;

	tesla->takedamage = true;
	tesla->die = tesla_die;
	tesla->dmg = TESLA_DAMAGE * tesla_damage_multiplier;
	tesla->classname = "tesla_mine";
	tesla->flags |= (FL_DAMAGEABLE | FL_TRAP);
	tesla->clipmask = (MASK_PROJECTILE | CONTENTS_SLIME | CONTENTS_LAVA) & ~CONTENTS_DEADMONSTER;

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		tesla->clipmask &= ~CONTENTS_PLAYER;

	tesla->flags |= FL_MECHANICAL;

	gi.linkentity(tesla);
}


/*
=================
fire_ionripper
=================
*/
static THINK(ionripper_sparks) (gentity_t *self) -> void {
	gi.WriteByte(svc_temp_entity);
	gi.WriteByte(TE_WELDING_SPARKS);
	gi.WriteByte(0);
	gi.WritePosition(self->s.origin);
	gi.WriteDir(vec3_origin);
	gi.WriteByte(irandom(0xe4, 0xe8));
	gi.multicast(self->s.origin, MULTICAST_PVS, false);

	G_FreeEntity(self);
}

static TOUCH(ionripper_touch) (gentity_t *self, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	if (other == self->owner)
		return;

	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(self);
		return;
	}

	if (self->owner->client)
		PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

	if (other->takedamage) {
		T_Damage(other, self, self->owner, self->velocity, self->s.origin, tr.plane.normal, self->dmg, 1, DAMAGE_ENERGY | DAMAGE_STAT_ONCE, MOD_RIPPER);
	} else {
		return;
	}

	G_FreeEntity(self);
}

void fire_ionripper(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, effects_t effect) {
	gentity_t *ion;
	trace_t	 tr;

	ion = G_Spawn();
	ion->s.origin = start;
	ion->s.old_origin = start;
	ion->s.angles = vectoangles(dir);
	ion->velocity = dir * speed;
	ion->movetype = MOVETYPE_WALLBOUNCE;
	ion->clipmask = MASK_PROJECTILE;

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		ion->clipmask &= ~CONTENTS_PLAYER;

	ion->solid = SOLID_BBOX;
	ion->s.effects |= effect;
	ion->svflags |= SVF_PROJECTILE;
	ion->flags |= FL_DODGE;
	ion->s.renderfx |= RF_FULLBRIGHT;
	ion->s.modelindex = gi.modelindex("models/objects/boomrang/tris.md2");
	ion->s.sound = gi.soundindex("misc/lasfly.wav");
	ion->owner = self;
	ion->touch = ionripper_touch;
	ion->nextthink = level.time + 3_sec;
	ion->think = ionripper_sparks;
	ion->dmg = damage;
	ion->splash_radius = 100;
	gi.linkentity(ion);

	tr = gi.traceline(self->s.origin, ion->s.origin, ion, ion->clipmask);
	if (tr.fraction < 1.0f) {
		ion->s.origin = tr.endpos + (tr.plane.normal * 1.f);
		ion->touch(ion, tr.ent, tr, false);
	}
}


/*
=================
fire_heat
=================
*/
static THINK(heat_think) (gentity_t *self) -> void {
	gentity_t *target = nullptr;
	gentity_t *acquire = nullptr;
	vec3_t	 vec;
	vec3_t	 oldang;
	float	 len;
	float	 oldlen = 0;
	float	 dot, olddot = 1;

	vec3_t fwd = AngleVectors(self->s.angles).forward;

	// acquire new target
	while ((target = findradius(target, self->s.origin, 1024)) != nullptr) {
		if (self->owner == target)
			continue;
		if (!target->client)
			continue;
		if (target->health <= 0)
			continue;
		if (target->client && target->client->eliminated)
			continue;
		if (!visible(self, target))
			continue;

		vec = self->s.origin - target->s.origin;
		len = vec.length();

		dot = vec.normalized().dot(fwd);

		// targets that require us to turn less are preferred
		if (dot >= olddot)
			continue;

		if (acquire == nullptr || dot < olddot || len < oldlen) {
			acquire = target;
			oldlen = len;
			olddot = dot;
		}
	}

	if (acquire != nullptr) {
		oldang = self->s.angles;
		vec = (acquire->s.origin - self->s.origin).normalized();
		float t = self->accel;

		float d = self->movedir.dot(vec);

		if (d < 0.45f && d > -0.45f)
			vec = -vec;

		self->movedir = slerp(self->movedir, vec, t).normalized();
		self->s.angles = vectoangles(self->movedir);

		if (!self->enemy) {
			gi.sound(self, CHAN_WEAPON, gi.soundindex("weapons/railgr1a.wav"), 1.f, 0.25f, 0);
			self->enemy = acquire;
		}
	} else
		self->enemy = nullptr;

	self->velocity = self->movedir * self->speed;
	self->nextthink = level.time + FRAME_TIME_MS;
}

void fire_heat(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, float damage_radius, int radius_damage, float turn_fraction) {
	gentity_t *heat;

	heat = G_Spawn();
	heat->s.origin = start;
	heat->movedir = dir;
	heat->s.angles = vectoangles(dir);
	heat->velocity = dir * speed;
	heat->flags |= FL_DODGE;
	heat->movetype = MOVETYPE_FLYMISSILE;
	heat->svflags |= SVF_PROJECTILE;
	heat->clipmask = MASK_PROJECTILE;
	heat->solid = SOLID_BBOX;
	heat->s.effects |= EF_ROCKET;
	heat->s.modelindex = gi.modelindex("models/objects/rocket/tris.md2");
	heat->owner = self;
	heat->touch = rocket_touch;
	heat->speed = speed;
	heat->accel = turn_fraction;

	heat->nextthink = level.time + FRAME_TIME_MS;
	heat->think = heat_think;

	heat->dmg = damage;
	heat->splash_damage = radius_damage;
	heat->splash_radius = damage_radius;
	heat->s.sound = gi.soundindex("weapons/rockfly.wav");

	gi.linkentity(heat);
}


/*
=================
fire_phalanx
=================
*/
static TOUCH(phalanx_touch) (gentity_t *ent, gentity_t *other, const trace_t &tr, bool other_touching_self) -> void {
	vec3_t origin;

	if (other == ent->owner)
		return;

	if (tr.surface && (tr.surface->flags & SURF_SKY)) {
		G_FreeEntity(ent);
		return;
	}

	if (ent->owner->client)
		PlayerNoise(ent->owner, ent->s.origin, PNOISE_IMPACT);

	// calculate position for the explosion entity
	origin = ent->s.origin + (ent->velocity * -0.02f);

	if (other->takedamage)
		T_Damage(other, ent, ent->owner, ent->velocity, ent->s.origin, tr.plane.normal, ent->dmg, 0, DAMAGE_ENERGY, MOD_PHALANX);

	T_RadiusDamage(ent, ent->owner, (float)ent->splash_damage, other, ent->splash_radius, DAMAGE_ENERGY, MOD_PHALANX);

	gi.WriteByte(svc_temp_entity);
	gi.WriteByte(TE_PLASMA_EXPLOSION);
	gi.WritePosition(origin);
	gi.multicast(ent->s.origin, MULTICAST_PHS, false);

	G_FreeEntity(ent);
}

void fire_phalanx(gentity_t *self, const vec3_t &start, const vec3_t &dir, int damage, int speed, float damage_radius, int radius_damage) {
	gentity_t *phalanx;

	phalanx = G_Spawn();
	phalanx->s.origin = start;
	phalanx->movedir = dir;
	phalanx->s.angles = vectoangles(dir);
	phalanx->velocity = dir * speed;
	phalanx->movetype = MOVETYPE_FLYMISSILE;
	phalanx->clipmask = MASK_PROJECTILE;

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		phalanx->clipmask &= ~CONTENTS_PLAYER;

	phalanx->solid = SOLID_BBOX;
	phalanx->svflags |= SVF_PROJECTILE;
	phalanx->flags |= FL_DODGE;
	phalanx->owner = self;
	phalanx->touch = phalanx_touch;
	phalanx->nextthink = level.time + gtime_t::from_sec(8000.f / speed);
	phalanx->think = G_FreeEntity;
	phalanx->dmg = damage;
	phalanx->splash_damage = radius_damage;
	phalanx->splash_radius = damage_radius;
	phalanx->s.sound = gi.soundindex("weapons/rockfly.wav");

	phalanx->s.modelindex = gi.modelindex("sprites/s_photon.sp2");
	phalanx->s.effects |= EF_PLASMA | EF_ANIM_ALLFAST;

	gi.linkentity(phalanx);
}



/*
=================
fire_trap
=================
*/
static THINK(Trap_Gib_Think) (gentity_t *ent) -> void {
	if (ent->owner->s.frame != 5) {
		G_FreeEntity(ent);
		return;
	}

	vec3_t forward, right, up;
	vec3_t vec;

	AngleVectors(ent->owner->s.angles, forward, right, up);

	// rotate us around the center
	float degrees = (150.f * gi.frame_time_s) + ent->owner->delay;
	vec3_t diff = ent->owner->s.origin - ent->s.origin;
	vec = RotatePointAroundVector(up, diff, degrees);
	ent->s.angles[YAW] += degrees;
	vec3_t new_origin = ent->owner->s.origin - vec;

	trace_t tr = gi.traceline(ent->s.origin, new_origin, ent, MASK_SOLID);
	ent->s.origin = tr.endpos;

	// pull us towards the trap's center
	diff.normalize();
	ent->s.origin += diff * (15.0f * gi.frame_time_s);

	ent->watertype = gi.pointcontents(ent->s.origin);
	if (ent->watertype & MASK_WATER)
		ent->waterlevel = WATER_FEET;

	ent->nextthink = level.time + FRAME_TIME_S;
	gi.linkentity(ent);
}

static DIE(trap_die) (gentity_t *self, gentity_t *inflictor, gentity_t *attacker, int damage, const vec3_t &point, const mod_t &mod) -> void {
	BecomeExplosion1(self);
}

static void SP_item_foodcube(gentity_t *self) {
	if (deathmatch->integer && g_no_health->integer) {
		G_FreeEntity(self);
		return;
	}

	SpawnItem(self, GetItemByIndex(IT_FOODCUBE));
	self->spawnflags |= SPAWNFLAG_ITEM_DROPPED;
}

void SpawnDamage(int type, const vec3_t &origin, const vec3_t &normal, int damage);

static THINK(Trap_Think) (gentity_t *ent) -> void {
	gentity_t *target = nullptr;
	gentity_t *best = nullptr;
	vec3_t	 vec;
	float	 len;
	float	 oldlen = 8000;

	if (ent->timestamp < level.time) {
		BecomeExplosion1(ent);
		// note to self
		// cause explosion damage???
		return;
	}

	ent->nextthink = level.time + 10_hz;

	if (!ent->groundentity)
		return;

	// ok lets do the blood effect
	if (ent->s.frame > 4) {
		if (ent->s.frame == 5) {
			bool spawn = ent->wait == 64;

			ent->wait -= 2;

			if (spawn)
				gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/trapdown.wav"), 1, ATTN_IDLE, 0);

			ent->delay += 2.f;

			if (ent->wait < 19)
				ent->s.frame++;

			return;
		}
		ent->s.frame++;
		if (ent->s.frame == 8) {
			ent->nextthink = level.time + 1_sec;
			ent->think = G_FreeEntity;
			ent->s.effects &= ~EF_TRAP;

			best = G_Spawn();
			best->count = ent->mass;
			best->s.scale = 1.f + ((ent->accel - 100.f) / 300.f) * 1.0f;
			SP_item_foodcube(best);
			best->s.origin = ent->s.origin;
			best->s.origin[2] += 24 * best->s.scale;
			best->s.angles[YAW] = frandom() * 360;
			best->velocity[2] = 400;
			best->think(best);
			best->nextthink = 0_ms;
			best->s.old_origin = best->s.origin;
			gi.linkentity(best);

			gi.sound(best, CHAN_AUTO, gi.soundindex("misc/fhit3.wav"), 1.f, ATTN_NORM, 0.f);

			return;
		}
		return;
	}

	ent->s.effects &= ~EF_TRAP;
	if (ent->s.frame >= 4) {
		ent->s.effects |= EF_TRAP;
		// clear the owner if in deathmatch
		if (deathmatch->integer)
			ent->owner = nullptr;
	}

	if (ent->s.frame < 4) {
		ent->s.frame++;
		return;
	}

	if (deathmatch->integer && IsCombatDisabled())
		return;

	while ((target = findradius(target, ent->s.origin, 256)) != nullptr) {
		if (target == ent)
			continue;

		// [Paril-KEX] don't allow traps to be placed near flags or teleporters
		// if it's a monster or player with health > 0
		// or it's a player start point
		// and we can see it
		// blow up
		if (target->classname && ((deathmatch->integer &&
			((!strncmp(target->classname, "info_player_", 12)) ||
				(!strcmp(target->classname, "misc_teleporter_dest")) ||
				(!strncmp(target->classname, "item_flag_", 10))))) &&
			(visible(target, ent))) {
			BecomeExplosion1(ent);
			return;
		}

		if (!(target->svflags & SVF_MONSTER) && !target->client)
			continue;
		if (target != ent->teammaster && CheckTeamDamage(target, ent->teammaster))
			continue;
		// [Paril-KEX]
		if (!deathmatch->integer && target->client)
			continue;
		if (target->health <= 0)
			continue;
		if (target->client && target->client->eliminated)
			continue;
		if (!visible(ent, target))
			continue;
		vec = ent->s.origin - target->s.origin;
		len = vec.length();
		if (!best) {
			best = target;
			oldlen = len;
			continue;
		}
		if (len < oldlen) {
			oldlen = len;
			best = target;
		}
	}

	// pull the enemy in
	if (best) {
		if (best->groundentity) {
			best->s.origin[2] += 1;
			best->groundentity = nullptr;
		}
		vec = ent->s.origin - best->s.origin;
		len = vec.normalize();

		float max_speed = best->client ? 290.f : 150.f;

		best->velocity += (vec * clamp(max_speed - len, 64.f, max_speed));

		ent->s.sound = gi.soundindex("weapons/trapsuck.wav");

		if (len < 48) {
			if (best->mass < 400) {
				ent->takedamage = false;
				ent->solid = SOLID_NOT;
				ent->die = nullptr;

				T_Damage(best, ent, ent->teammaster, vec3_origin, best->s.origin, vec3_origin, 100000, 1, DAMAGE_NONE | DAMAGE_STAT_ONCE, MOD_TRAP);

				if (best->svflags & SVF_MONSTER)
					M_ProcessPain(best);

				ent->enemy = best;
				ent->wait = 64;
				ent->s.old_origin = ent->s.origin;
				ent->timestamp = level.time + 30_sec;
				ent->accel = best->mass;
				ent->mass = best->mass / (deathmatch->integer ? 4 : 10);

				// ok spawn the food cube
				ent->s.frame = 5;

				// link up any gibs that this monster may have spawned
				for (size_t i = 0; i < globals.num_entities; i++) {
					gentity_t *e = &g_entities[i];

					if (!e->inuse)
						continue;
					else if (strcmp(e->classname, "gib"))
						continue;
					else if ((e->s.origin - ent->s.origin).length() > 128.f)
						continue;

					e->movetype = MOVETYPE_NONE;
					e->nextthink = level.time + FRAME_TIME_S;
					e->think = Trap_Gib_Think;
					e->owner = ent;
					Trap_Gib_Think(e);
				}
			} else {
				BecomeExplosion1(ent);
				// note to self
				// cause explosion damage???
				return;
			}
		}
	}
}

void fire_trap(gentity_t *self, const vec3_t &start, const vec3_t &aimdir, int speed) {
	gentity_t *trap;
	vec3_t	 dir;
	vec3_t	 forward, right, up;

	dir = vectoangles(aimdir);
	AngleVectors(dir, forward, right, up);

	trap = G_Spawn();
	trap->s.origin = start;
	trap->velocity = aimdir * speed;

	float gravityAdjustment = level.gravity / 800.f;

	trap->velocity += up * (200 + crandom() * 10.0f) * gravityAdjustment;
	trap->velocity += right * (crandom() * 10.0f);

	trap->avelocity = { 0, 300, 0 };
	trap->movetype = MOVETYPE_BOUNCE;

	trap->solid = SOLID_BBOX;
	trap->takedamage = true;
	trap->mins = { -4, -4, 0 };
	trap->maxs = { 4, 4, 8 };
	trap->die = trap_die;
	trap->health = 20;
	trap->s.modelindex = gi.modelindex("models/weapons/z_trap/tris.md2");
	trap->owner = trap->teammaster = self;
	trap->nextthink = level.time + 1_sec;
	trap->think = Trap_Think;
	trap->classname = "food_cube_trap";
	trap->s.sound = gi.soundindex("weapons/traploop.wav");

	trap->flags |= (FL_DAMAGEABLE | FL_MECHANICAL | FL_TRAP);
	trap->clipmask = MASK_PROJECTILE & ~CONTENTS_DEADMONSTER;

	// [Paril-KEX]
	if (self->client && !G_ShouldPlayersCollide(true))
		trap->clipmask &= ~CONTENTS_PLAYER;

	gi.linkentity(trap);

	trap->timestamp = level.time + 30_sec;
}
