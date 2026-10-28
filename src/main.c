/*
 * twinslot: two Android ROMs on one A/B phone, each with its own data.
 *
 * Every command checks before it writes, and every write is read back.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bctl.h"
#include "install.h"
#include "payload.h"
#include "rominfo.h"
#include "twin.h"

#define MiB (1ull << 20)

static bool force;

static int usage(void)
{
	fputs("usage: twinslot [global options] <command> [arguments]\n"
	      "\n"
	      "commands:\n"
	      "  status                      slots, installed ROMs and dual data\n"
	      "  switch a|b|other            boot that slot next\n"
	      "  install ZIP [options]       flash a full ROM zip into a slot\n"
	      "      --slot a|b              target slot (default: the one not running)\n"
	      "      --no-switch             keep booting the current slot\n"
	      "      --wipe                  start the installed ROM with empty data\n"
	      "      --bootloader            also write the zip's lk/preloader\n"
	      "      --dry-run               show the plan, write nothing\n"
	      "  verify ZIP                  check a ROM zip's data before installing it\n"
	      "  setup [options]             split userdata so each slot has its own data\n"
	      "      --size N[M|G]           space for the second ROM (default: half)\n"
	      "      --owner a|b             slot that gets the new data (default: the other)\n"
	      "      --meta-from NAME|none   spare partition to become twinmeta (default: a\n"
	      "                              known spare one such as tranfs, else carve it)\n"
	      "      --apply                 do it (without: show the plan)\n"
	      "  remove [--apply]            undo setup, giving the space back to userdata\n"
	      "  rebind a|b [primary|twin]   point a slot's ROM at its data again\n"
	      "  wipe a|b                    blank the data of a slot's ROM\n"
	      "  table-save [DIR]            save a copy of the partition table (setup and\n"
	      "                              remove also save one before they write)\n"
	      "  table-restore FILE          write a saved copy back (after a cut-off write;\n"
	      "                              the disk comes from the copy's name or size)\n"
	      "  version\n"
	      "\n"
	      "global options:\n"
	      "  --dev-dir DIR   partitions are image files in DIR (default /dev/block/by-name)\n"
	      "  --disk PATH     the disk holding userdata's partition table\n"
	      "  --current a|b   the running slot, when it cannot be detected\n"
	      "  --table-dir DIR also save the partition table copy there (an SD card)\n"
	      "  --kept-copy FILE  a copy of the table that is kept off the phone (the zip's\n"
	      "                  table.bin); counts only if it matches the table byte for byte\n"
	      "  --kept-copy-earlier  an earlier step of this zip changed the table: the kept\n"
	      "                  copy is from before it and counts if it fits this disk\n"
	      "  --force         skip safety refusals (read the message first)\n",
	      stderr);
	return EXIT_USAGE;
}

static int parse_slot(const char *s)
{
	if (s && (s[0] == 'a' || s[0] == 'b') && !s[1])
		return s[0] - 'a';
	if (s && !strcmp(s, "other")) {
		int c = detect_current_slot();
		if (c < 0)
			msg("cannot tell which slot is running; say a or b");
		return c < 0 ? -1 : !c;
	}
	return -1;
}

static bool parse_size(const char *s, u64 *bytes)
{
	char *end;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (errno || end == s)
		return false;
	switch (*end) {
	case 'G': case 'g':
		if (v > UINT64_MAX >> 30)
			return false;
		v <<= 30;
		end++;
		break;
	case 'M': case 'm':
		if (v > UINT64_MAX >> 20)
			return false;
		v <<= 20;
		end++;
		break;
	}
	*bytes = v;
	return !*end && v;
}

/* ---- status ---- */

static void print_slot(const struct bctl *b, int s, const struct twin_state *ts, bool ts_known)
{
	struct rom_info ri;
	bool known = rom_info(s, &ri) == EXIT_OK;
	enum binding vb = BIND_NONE, v = BIND_NONE;
	bool bound = known && ri.installed && slot_binding(s, &vb, &v) == EXIT_OK;
	const struct bctl_slot *sl = &b->slot[s];
	out("slot_%c=priority:%u tries:%u successful:%u corrupted:%u bootable:%s installed:%s data:%s expected:%s system_mib:%llu",
	    slot_letter(s), sl->priority, sl->tries, sl->successful, sl->corrupted, bctl_bootable(sl) ? "yes" : "no",
	    !known ? "unknown" : ri.installed ? "yes" : "no",
	    !bound ? "unknown" : vb == v ? binding_name(vb) : "mixed",
	    ts_known ? binding_name(slot_expected_binding(ts, s)) : "unknown", (unsigned long long)(ri.system_bytes / MiB));
	if (ri.installed)
		out("rom_%c=%s|%s|%s", slot_letter(s), ri.android, ri.build, ri.readable ? ri.name : "");
}

static int cmd_status(void)
{
	struct bctl b;
	int r = bctl_read(&b);
	if (!r)
		r = bctl_check(&b, true);
	if (r)
		return r;
	if (!bctl_crc_ok(&b))
		msg("warning: the slot record's checksum is wrong");
	int cur = detect_current_slot();
	out("version=%s", TWINSLOT_VERSION);
	out("current=%c", cur >= 0 ? slot_letter(cur) : '?');
	int next = bctl_next_slot(&b);
	out("next=%c", next >= 0 ? slot_letter(next) : '?');
	out("merge_status=%s", bctl_merge_name(b.merge_status));
	out("recovery=%s", in_recovery() ? "yes" : "no");

	struct twin_state ts;
	bool ts_known = twin_state(&ts) == EXIT_OK;
	if (ts_known) {
		out("dual_data=%s", ts.ready ? "ready" : "off");
		out("userdata_mib=%llu", (unsigned long long)(ts.userdata_bytes / MiB));
		if (ts.ready) {
			out("dual_owner=%c", slot_letter(ts.owner));
			out("twindata_mib=%llu", (unsigned long long)(ts.data_bytes / MiB));
			out("twinmeta_mib=%llu", (unsigned long long)(ts.meta_bytes / MiB));
		}
	} else {
		out("dual_data=unknown");
	}
	if (ts.disk[0])
		out("disk=%s", ts.disk); /* the disk with userdata's partition table: what a table copy restores */
	for (int s = 0; s < 2; s++)
		print_slot(&b, s, &ts, ts_known);
	return EXIT_OK;
}

/* ---- switch ---- */

static int cmd_switch(const char *arg)
{
	int target = parse_slot(arg);
	if (target < 0)
		return usage();
	struct bctl b;
	int r = bctl_read(&b);
	if (!r)
		r = bctl_check(&b, force);
	if (r)
		return r;
	if (bctl_merge_pending(&b) && !force) {
		msg("a system update is still merging (%s); boot the current ROM until it finishes,",
		    bctl_merge_name(b.merge_status));
		msg("or the update is lost");
		return EXIT_REFUSED;
	}

	if (!force && bctl_half_written(&b.slot[target])) {
		msg("slot %c is marked unbootable: an install or rebind did not finish, or it failed to boot", slot_letter(target));
		msg("install it again, or after a rebind cut short run 'twinslot --force rebind %c' (--force overrides)",
		    slot_letter(target));
		return EXIT_REFUSED;
	}
	struct rom_info ri;
	if (!force && (rom_info(target, &ri) || !ri.installed)) {
		msg("no ROM is installed in slot %c; booting it would hang at the logo (--force overrides)",
		    slot_letter(target));
		return EXIT_REFUSED;
	}
	struct twin_state ts;
	if ((r = twin_state(&ts)) && !force)
		return r;
	enum binding want = slot_expected_binding(&ts, target), vb, v;
	if (!force) {
		if (slot_binding(target, &vb, &v)) {
			msg("cannot read which data slot %c's ROM mounts (a half-written install?); install it again",
			    slot_letter(target));
			return EXIT_REFUSED;
		}
		if (vb != want || v != want) {
			msg("slot %c's ROM mounts %s data (boot image) and %s data (vendor), but must mount %s",
			    slot_letter(target), binding_name(vb), binding_name(v), binding_name(want));
			msg("it would boot on the other ROM's data; run 'twinslot rebind %c' first", slot_letter(target));
			return EXIT_REFUSED;
		}
	}
	/*
	 * Without dual data both slots mount userdata: the other slot's ROM would start on the data of the
	 * ROM that used it last and break both (after remove, or a resize whose setup step failed).
	 */
	if (!ts.ready && !force) {
		int cur = detect_current_slot();
		if (cur != target) {
			msg("dual data is off: slot %c's ROM would start on the same /data as slot %c's ROM and break both",
			    slot_letter(target), slot_letter(!target));
			msg("run 'twinslot setup' from recovery first, or wipe the data (--force switches anyway)");
			return EXIT_REFUSED;
		}
	}

	bctl_set_active(&b, target);
	if ((r = bctl_write(&b)))
		return r;
	out("switched: next boot uses slot %c", slot_letter(target));
	return EXIT_OK;
}

/* ---- install ---- */

static int cmd_install(int argc, char **argv)
{
	struct install_opts o = {.target = -1, .switch_after = true, .force = force};
	for (int i = 0; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--slot") && i + 1 < argc) {
			if ((o.target = parse_slot(argv[++i])) < 0)
				return usage();
		} else if (!strcmp(a, "--no-switch")) {
			o.switch_after = false;
		} else if (!strcmp(a, "--wipe")) {
			o.wipe = true;
		} else if (!strcmp(a, "--bootloader")) {
			o.bootloader = true;
		} else if (!strcmp(a, "--dry-run")) {
			o.dry_run = true;
		} else if (a[0] != '-' && !o.zip) {
			o.zip = a;
		} else {
			return usage();
		}
	}
	if (!o.zip)
		return usage();
	return install_rom(&o);
}

static int cmd_verify(const char *zip)
{
	struct payload p;
	int r = payload_open(zip, &p);
	if (r)
		return r;
	u64 total = 0, done = 0;
	for (u32 i = 0; i < p.nparts; i++)
		total += p.parts[i].size;
	for (u32 i = 0; i < p.nparts && !r; i++) {
		r = payload_verify(&p, i, &done, total);
		if (!r)
			out("ok %s %llu MiB%s", p.parts[i].name, (unsigned long long)(p.parts[i].size >> 20),
			    p.parts[i].dynamic ? " (super)" : "");
	}
	u32 count = p.nparts;
	payload_close(&p);
	if (!r)
		out("verified: %u partitions, %llu MiB", count, (unsigned long long)(total >> 20));
	return r;
}

/* ---- setup / remove ---- */

static int cmd_setup(int argc, char **argv)
{
	u64 size = 0;
	int owner = -1;
	bool apply = false;
	const char *meta_from = "auto";
	for (int i = 0; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--size") && i + 1 < argc) {
			if (!parse_size(argv[++i], &size))
				return usage();
		} else if (!strcmp(a, "--meta-from") && i + 1 < argc) {
			meta_from = argv[++i];
			if (!strcmp(meta_from, "none"))
				meta_from = NULL;
		} else if (!strcmp(a, "--owner") && i + 1 < argc) {
			if ((owner = parse_slot(argv[++i])) < 0)
				return usage();
		} else if (!strcmp(a, "--apply")) {
			apply = true;
		} else {
			return usage();
		}
	}
	int cur = detect_current_slot();
	if (owner < 0) {
		if (cur < 0) {
			msg("cannot tell which slot is running; say --owner a|b");
			return EXIT_USAGE;
		}
		owner = !cur;
	}
	if (owner == cur && !in_recovery() && !force) {
		msg("slot %c is running; the new data goes to the other slot's ROM (--owner %c)", slot_letter(cur),
		    slot_letter(!cur));
		return EXIT_REFUSED;
	}
	if (apply && !in_recovery() && !force) {
		msg("setup repartitions the disk and wipes data: run it from recovery");
		return EXIT_REFUSED;
	}
	if (!size) {
		struct twin_state ts;
		int r = twin_state(&ts);
		if (r)
			return r;
		size = ts.userdata_bytes / 2 / MiB * MiB;
	}
	return twin_setup(size, 0, meta_from, owner, apply, force);
}

static int cmd_remove(int argc, char **argv)
{
	bool apply = argc == 1 && !strcmp(argv[0], "--apply");
	if (argc > 1 || (argc == 1 && !apply))
		return usage();
	if (apply && !in_recovery() && !force) {
		msg("remove repartitions the disk and wipes data: run it from recovery");
		return EXIT_REFUSED;
	}
	return twin_remove(apply, force);
}

/* ---- rebind / wipe ---- */

/*
 * A merging update lives in the running slot: its vendor is the base being merged into and its data holds
 * the COW. The other slot's images and data are not part of it.
 */
static int merge_refusal(const struct bctl *b, int slot, const char *what)
{
	if (!bctl_merge_pending(b) || force)
		return EXIT_OK;
	/*
	 * The update's COW is in the data of the slot that wrote it (the running one) and the update itself
	 * is in the slot that boots next. Before the reboot those are two slots, after it one.
	 */
	int cur = detect_current_slot();
	if (cur >= 0 && cur != slot && bctl_next_slot(b) != slot)
		return EXIT_OK;
	msg("a system update is still merging (%s) and slot %c is part of it; %s it now loses the update "
	    "(--force overrides)", bctl_merge_name(b->merge_status), slot_letter(slot), what);
	return EXIT_REFUSED;
}

static int cmd_rebind(int argc, char **argv)
{
	if (argc < 1 || argc > 2)
		return usage();
	int slot = parse_slot(argv[0]);
	if (slot < 0)
		return usage();
	struct twin_state ts;
	int r = twin_state(&ts);
	if (r)
		return r;
	enum binding to = slot_expected_binding(&ts, slot);
	if (argc == 2) {
		if (!strcmp(argv[1], "primary"))
			to = BIND_PRIMARY;
		else if (!strcmp(argv[1], "twin"))
			to = BIND_TWIN;
		else
			return usage();
		if (to != slot_expected_binding(&ts, slot) && !force) {
			msg("slot %c must mount %s data; binding it to %s needs --force", slot_letter(slot),
			    binding_name(slot_expected_binding(&ts, slot)), binding_name(to));
			return EXIT_REFUSED;
		}
	}
	if (slot == detect_current_slot() && !in_recovery() && !force) {
		msg("slot %c is running; rebind it from recovery", slot_letter(slot));
		return EXIT_REFUSED;
	}
	/* Unbootable while its images change: a rebind cut short leaves them half edited. */
	struct bctl b;
	if ((r = bctl_read(&b)) || (r = bctl_check(&b, false)))
		return r;
	if ((r = merge_refusal(&b, slot, "rebinding")))
		return r;
	struct bctl_slot was = b.slot[slot];
	bctl_set_unbootable(&b, slot);
	if ((r = bctl_write(&b)))
		return r;
	if ((r = slot_rebind(slot, to))) {
		msg("slot %c stays unbootable until a rebind succeeds", slot_letter(slot));
		return r;
	}
	if ((r = bctl_read(&b)))
		return r;
	/*
	 * Marked unbootable before, it stays so: an unfinished install looks the same as a rebind cut short or
	 * a ROM that failed to start. --force says it was one of the latter and parks it, so switch takes it.
	 */
	if (bctl_half_written(&was) && force) {
		bctl_set_parked(&b, slot);
	} else {
		b.slot[slot] = was;
		if (bctl_half_written(&was))
			msg("slot %c stays unbootable; if no install was cut short there, 'twinslot --force rebind %c' "
			    "turns it back on", slot_letter(slot), slot_letter(slot));
	}
	return bctl_write(&b);
}

static int cmd_wipe(int argc, char **argv)
{
	if (argc != 1)
		return usage();
	int slot = parse_slot(argv[0]);
	if (slot < 0)
		return usage();
	struct twin_state ts;
	int r = twin_state(&ts);
	if (r)
		return r;
	if (!ts.ready && !force) {
		msg("dual data is off: both ROMs use userdata, so wiping slot %c's data wipes slot %c's too (--force)",
		    slot_letter(slot), slot_letter(!slot));
		return EXIT_REFUSED;
	}
	struct bctl b;
	if ((r = bctl_read(&b)) || (r = bctl_check(&b, false)) || (r = merge_refusal(&b, slot, "wiping its data")))
		return r;
	return twin_wipe_slot_data(&ts, slot);
}

int main(int argc, char **argv)
{
	int i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--dev-dir") && i + 1 < argc) {
			dev_dir_set(argv[++i]);
		} else if (!strcmp(a, "--table-dir") && i + 1 < argc) {
			g_table_dir = argv[++i];
		} else if (!strcmp(a, "--kept-copy") && i + 1 < argc) {
			g_kept_copy = argv[++i];
		} else if (!strcmp(a, "--kept-copy-earlier")) {
			g_kept_copy_earlier = true;
		} else if (!strcmp(a, "--disk") && i + 1 < argc) {
			g_disk = argv[++i];
		} else if (!strcmp(a, "--current") && i + 1 < argc) {
			const char *v = argv[++i];
			if ((v[0] != 'a' && v[0] != 'b') || v[1])
				return usage();
			setenv("TWINSLOT_CURRENT_SLOT", v, 1);
		} else if (!strcmp(a, "--force")) {
			force = true;
		} else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			usage();
			return EXIT_OK;
		} else {
			return usage();
		}
	}
	if (i >= argc)
		return usage();
	dev_dir_find();
	const char *cmd = argv[i++];
	int n = argc - i;
	char **rest = argv + i;
	/* --force after the command is accepted too. */
	for (int k = 0; k < n; k++)
		if (!strcmp(rest[k], "--force")) {
			force = true;
			memmove(rest + k, rest + k + 1, (size_t)(n - k - 1) * sizeof(*rest));
			n--;
			k--;
		}

	if (!strcmp(cmd, "status") && !n)
		return cmd_status();
	if (!strcmp(cmd, "switch") && n == 1)
		return cmd_switch(rest[0]);
	if (!strcmp(cmd, "install"))
		return cmd_install(n, rest);
	if (!strcmp(cmd, "verify") && n == 1)
		return cmd_verify(rest[0]);
	if (!strcmp(cmd, "setup"))
		return cmd_setup(n, rest);
	if (!strcmp(cmd, "remove"))
		return cmd_remove(n, rest);
	if (!strcmp(cmd, "rebind"))
		return cmd_rebind(n, rest);
	if (!strcmp(cmd, "wipe"))
		return cmd_wipe(n, rest);
	if (!strcmp(cmd, "table-save") && n <= 1)
		return twin_table_save(n ? rest[0] : NULL);
	if (!strcmp(cmd, "table-restore") && n == 1) {
		if (!in_recovery() && !force) {
			msg("restoring the partition table changes partitions Android is using: run it from recovery");
			return EXIT_REFUSED;
		}
		return twin_table_restore(rest[0], force);
	}
	if (!strcmp(cmd, "version") && !n) {
		out("twinslot %s", TWINSLOT_VERSION);
		return EXIT_OK;
	}
	return usage();
}
