#include "install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bctl.h"
#include "lp.h"
#include "part.h"
#include "payload.h"
#include "twin.h"

/*
 * Partitions a ROM zip may carry that belong to the device rather than the
 * ROM: a ROM built for a sibling model can carry a bootloader that does not
 * boot this one.
 */
static const char *const bootloader_parts[] = {"lk", "preloader", "preloader_raw"};

static bool is_bootloader(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(bootloader_parts); i++)
		if (!strcmp(name, bootloader_parts[i]))
			return true;
	return false;
}

/* "name_<slot>" */
static void slotted(char *buf, size_t cap, const char *name, int slot)
{
	snprintf(buf, cap, "%s_%c", name, slot_letter(slot));
}

static bool has_suffix(const char *s, const char *suffix)
{
	size_t n = strlen(s), k = strlen(suffix);
	return n >= k && !strcmp(s + n - k, suffix);
}

struct plan {
	int current, target;
	struct twin_state twin;
	enum binding binding; /* what the target ROM must mount */
	struct lp_metadata m;
	char group[LP_NAME_LEN + 1];
	bool logical[512];    /* per payload partition */
	bool skip[512];
	u64 bytes;            /* everything that will be written */
};

/*
 * The target slot's group: the running ROM's system partition names its group
 * (main_a -> main_b); a fresh layout takes the payload's group name.
 */
static int plan_group(struct plan *pl, const struct payload *p)
{
	char name[64];
	int from = pl->current >= 0 ? pl->current : !pl->target;
	slotted(name, sizeof(name), "system", from);
	int sys = lp_find_partition(&pl->m, name);
	if (sys >= 0) {
		const char *g = pl->m.groups[pl->m.parts[sys].group].name;
		char suffix[3] = {'_', slot_letter(from), 0};
		if (has_suffix(g, suffix)) {
			snprintf(pl->group, sizeof(pl->group), "%.*s_%c", (int)(strlen(g) - 2), g,
				 slot_letter(pl->target));
			return EXIT_OK;
		}
	}
	if (!p->ngroups) {
		msg("the ROM zip has no dynamic partition groups and the running ROM names none");
		return EXIT_BAD_INPUT;
	}
	snprintf(pl->group, sizeof(pl->group), "%.33s_%c", p->groups[0].name, slot_letter(pl->target));
	return EXIT_OK;
}

static u64 payload_group_size(const struct payload *p)
{
	u64 size = 0;
	for (u32 i = 0; i < p->ngroups; i++)
		size += p->groups[i].size;
	return size;
}

/* Drops the target slot's old logical partitions (and their Virtual A/B COW partitions). */
static void plan_clear_slot(struct plan *pl)
{
	char suffix[3] = {'_', slot_letter(pl->target), 0}, cow[8];
	snprintf(cow, sizeof(cow), "%s-cow", suffix);
	for (int i = (int)pl->m.nparts - 1; i >= 0; i--) {
		const char *n = pl->m.parts[i].name;
		if (has_suffix(n, suffix) || has_suffix(n, cow)) {
			out("remove %s (%llu MiB)", n, (unsigned long long)(lp_partition_size(&pl->m.parts[i]) >> 20));
			lp_remove_partition(&pl->m, i);
		}
	}
}

static int plan_partitions(struct plan *pl, const struct payload *p, const struct install_opts *o)
{
	if (p->nparts > ARRAY_SIZE(pl->logical)) {
		msg("the ROM zip has too many partitions (%u)", p->nparts);
		return EXIT_BAD_INPUT;
	}
	int gi = lp_find_group(&pl->m, pl->group);
	u64 need = payload_group_size(p);
	if (gi < 0) {
		gi = lp_add_group(&pl->m, pl->group, need);
		if (gi < 0)
			return EXIT_BAD_INPUT;
		out("add group %s (%llu MiB)", pl->group, (unsigned long long)(need >> 20));
	} else if (pl->m.groups[gi].max_size && need > pl->m.groups[gi].max_size) {
		out("grow group %s to %llu MiB", pl->group, (unsigned long long)(need >> 20));
		pl->m.groups[gi].max_size = need;
	}

	char name[80];
	for (u32 i = 0; i < p->nparts; i++) {
		const struct pl_partition *pp = &p->parts[i];
		slotted(name, sizeof(name), pp->name, pl->target);
		char other[80];
		slotted(other, sizeof(other), pp->name, !pl->target);
		pl->logical[i] = pp->dynamic || lp_find_partition(&pl->m, other) >= 0;
		if (pl->logical[i]) {
			int r = lp_add_partition(&pl->m, name, (u32)gi, LP_ATTR_READONLY, pp->size);
			if (r) {
				msg("super cannot hold %s (%llu MiB); %llu MiB are free", name,
				    (unsigned long long)(pp->size >> 20), (unsigned long long)(lp_free_bytes(&pl->m) >> 20));
				return r;
			}
			out("add %s (%llu MiB) in super", name, (unsigned long long)(pp->size >> 20));
		} else if (is_bootloader(pp->name) && !o->bootloader) {
			pl->skip[i] = true;
			out("keep the device's %s (the zip's is skipped)", pp->name);
			continue;
		} else if (!dev_exists(name)) {
			if (dev_exists(pp->name)) {
				/* An unslotted partition is shared with the running ROM. */
				msg("%s is not A/B on this device; installing it would change the other ROM too", pp->name);
				return EXIT_REFUSED;
			}
			pl->skip[i] = true;
			out("skip %s (no such partition on this device)", pp->name);
			continue;
		} else {
			out("write %s (%llu MiB)", name, (unsigned long long)(pp->size >> 20));
		}
		pl->bytes += pp->size;
	}
	return EXIT_OK;
}

static int write_partitions(struct plan *pl, const struct payload *p, int super_fd)
{
	u64 done = 0;
	char name[80];
	for (u32 i = 0; i < p->nparts; i++) {
		if (pl->skip[i])
			continue;
		const struct pl_partition *pp = &p->parts[i];
		slotted(name, sizeof(name), pp->name, pl->target);
		struct part dst;
		int r;
		if (pl->logical[i]) {
			int k = lp_find_partition(&pl->m, name);
			part_logical(&dst, super_fd, &pl->m.parts[k]);
		} else if ((r = part_open(&dst, name, true))) {
			return r;
		}
		r = payload_write(p, i, &dst, &done, pl->bytes);
		part_close(&dst);
		if (r) {
			msg("writing %s failed; slot %c stays unbootable", name, slot_letter(pl->target));
			return r;
		}
	}
	return EXIT_OK;
}

static int finish_slot(const struct plan *pl, const struct install_opts *o)
{
	struct bctl b;
	int r = bctl_read(&b);
	if (!r)
		r = bctl_check(&b, false);
	if (r)
		return r;
	if (o->switch_after) {
		bctl_set_active(&b, pl->target);
	} else {
		/* Bootable as the fallback, below the running ROM. */
		struct bctl_slot *s = &b.slot[pl->target];
		s->priority = MIN(14u, b.slot[!pl->target].priority ? b.slot[!pl->target].priority - 1 : 14u);
		s->priority = MAX(s->priority, 1u);
		s->tries = 7;
		s->successful = 0;
		s->corrupted = 0;
	}
	return bctl_write(&b);
}

int install_rom(const struct install_opts *o)
{
	struct plan *pl = xcalloc(1, sizeof(*pl));
	struct payload p;
	struct part super = {.fd = -1};
	bool have_payload = false, have_lp = false;
	int r;

	pl->current = detect_current_slot();
	pl->target = o->target >= 0 ? o->target : pl->current >= 0 ? !pl->current : -1;
	if (pl->target < 0) {
		msg("cannot tell which slot is running; name the target slot (a or b)");
		r = EXIT_USAGE;
		goto out;
	}
	if (pl->target == pl->current && !in_recovery()) {
		msg("slot %c is the running ROM; install over it from recovery", slot_letter(pl->target));
		r = EXIT_REFUSED;
		goto out;
	}

	struct bctl b;
	if ((r = bctl_read(&b)) || (r = bctl_check(&b, false)))
		goto out;
	if (bctl_merge_pending(&b)) {
		msg("a system update is still merging (%s); boot the current ROM until it finishes",
		    bctl_merge_name(b.merge_status));
		r = EXIT_REFUSED;
		goto out;
	}

	if ((r = twin_state(&pl->twin)))
		goto out;
	if (!pl->twin.ready && !o->force) {
		msg("dual data is not set up: both ROMs would share one /data and break each other");
		msg("run 'twinslot setup' from recovery first (--force installs anyway)");
		r = EXIT_REFUSED;
		goto out;
	}
	pl->binding = slot_expected_binding(&pl->twin, pl->target);

	progress(0, "reading the ROM zip");
	if ((r = payload_open(o->zip, &p)))
		goto out;
	have_payload = true;

	if ((r = part_open(&super, "super", !o->dry_run)))
		goto out;
	/*
	 * The other slot's copy: it is the one that knows where that slot's ROM lies now (an OTA or fastbootd
	 * rewrites only its own slot's copy), and the target's partitions are cleared and planned anew anyway.
	 */
	if ((r = lp_read(super.fd, (u32)!pl->target, &pl->m)))
		goto out;
	have_lp = true;

	out("target=%c", slot_letter(pl->target));
	out("data=%s", binding_name(pl->binding));
	if ((r = plan_group(pl, &p)))
		goto out;
	plan_clear_slot(pl);
	if ((r = plan_partitions(pl, &p, o)))
		goto out;
	out("bytes=%llu", (unsigned long long)pl->bytes);
	if (o->dry_run) {
		out("dry run: nothing written");
		goto out;
	}

	/* From here a half-written slot must never be booted. */
	progress(0, "marking slot %c unbootable", slot_letter(pl->target));
	{
		struct bctl u;
		if ((r = bctl_read(&u)))
			goto out;
		bctl_set_unbootable(&u, pl->target);
		if ((r = bctl_write(&u)))
			goto out;
	}
	progress(0, "updating the super partition table");
	if ((r = lp_write(super.fd, &pl->m)))
		goto out;
	if ((r = write_partitions(pl, &p, super.fd)))
		goto out;
	if ((r = part_sync(&super)))
		goto out;

	if (pl->binding == BIND_TWIN) {
		progress(1000, "pointing the ROM at its own data");
		if ((r = slot_rebind(pl->target, BIND_TWIN)))
			goto out;
	}
	enum binding vb, v;
	if ((r = slot_binding(pl->target, &vb, &v)))
		goto out;
	if (vb != pl->binding || v != pl->binding) {
		msg("slot %c mounts %s/%s data, expected %s; leaving it unbootable", slot_letter(pl->target),
		    binding_name(vb), binding_name(v), binding_name(pl->binding));
		r = EXIT_IO;
		goto out;
	}
	if (o->wipe) {
		progress(1000, "wiping the ROM's data");
		if ((r = twin_wipe_slot_data(&pl->twin, pl->target)))
			goto out;
	}
	if ((r = finish_slot(pl, o)))
		goto out;
	progress(1000, "done");
	out(o->switch_after ? "installed: slot %c boots next" : "installed: slot %c is ready (not switched)",
	    slot_letter(pl->target));
out:
	if (have_lp)
		lp_free(&pl->m);
	if (super.fd >= 0)
		part_close(&super);
	if (have_payload)
		payload_close(&p);
	free(pl);
	return r;
}
