#include "fstab.h"

#include <string.h>

struct token {
	u8 *p;
	size_t len;
};

static bool is_space(u8 c) { return c == ' ' || c == '\t' || c == '\r'; }

static bool token_is(const struct token *t, const char *s)
{
	return t->len == strlen(s) && !memcmp(t->p, s, t->len);
}

static bool is_name_char(u8 c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
	       c == '.';
}

/*
 * The last "/name" in t that ends the token or is followed by something that is
 * not part of a name: a file_contexts regex such as by-name/userdata(/.*)? or
 * userdata$ counts as naming userdata. Empty if there is none.
 */
static struct token find_name(const struct token *t, const char *name)
{
	size_t n = strlen(name);
	for (size_t i = t->len; i-- > 0;) {
		if (t->p[i] != '/' || i + 1 + n > t->len || memcmp(t->p + i + 1, name, n))
			continue;
		size_t end = i + 1 + n;
		if (end == t->len || !is_name_char(t->p[end]))
			return (struct token){t->p + i + 1, n};
	}
	return (struct token){t->p + t->len, 0};
}

/* A block device path: /dev/block/by-name/x, /dev/block/platform/.../by-name/x. */
static bool is_device(const struct token *t)
{
	static const char prefix[] = "/dev/block/";
	return t->len > sizeof(prefix) - 1 && !memcmp(t->p, prefix, sizeof(prefix) - 1);
}

/*
 * For every line with a token that is exactly /data or /metadata, visits each
 * block device on that line that names one of the two partitions for that
 * mount (find_name). Mount points and options (keydirectory=/metadata/...) are never
 * touched; device and mount point may be in either order (AOSP fstab or
 * twrp.flags). SELinux file_contexts lines that give the partition its block
 * device label count too: without them the twin partitions keep the generic
 * block_device label and vold cannot open them, so /data never formats.
 */
static int walk(u8 *buf, size_t len, enum binding to, enum binding *seen)
{
	static const struct {
		const char *mount, *label, *primary, *twin;
	} map[] = {{"/data", "u:object_r:userdata_block_device:s0", "userdata", TWIN_DATA},
		   {"/metadata", "u:object_r:metadata_block_device:s0", "metadata", TWIN_META}};
	int changed = 0;

	for (size_t start = 0; start < len;) {
		size_t end = start;
		while (end < len && buf[end] != '\n')
			end++;

		struct token tok[16];
		int n = 0;
		for (size_t i = start; i < end && n < 16;) {
			while (i < end && is_space(buf[i]))
				i++;
			if (i >= end || buf[i] == '#')
				break;
			size_t s = i;
			while (i < end && !is_space(buf[i]))
				i++;
			tok[n++] = (struct token){buf + s, i - s};
		}

		for (size_t m = 0; m < ARRAY_SIZE(map); m++) {
			bool mounts = false;
			for (int i = 0; i < n; i++)
				mounts |= token_is(&tok[i], map[m].mount) || token_is(&tok[i], map[m].label);
			if (!mounts)
				continue;
			for (int i = 0; i < n; i++) {
				if (!is_device(&tok[i]))
					continue;
				struct token primary = find_name(&tok[i], map[m].primary);
				struct token twin = find_name(&tok[i], map[m].twin);
				/* Both in one path: the one nearer the end is the partition. */
				struct token base = primary;
				enum binding b = BIND_PRIMARY;
				if (twin.len && (!primary.len || twin.p > primary.p)) {
					base = twin;
					b = BIND_TWIN;
				}
				if (!base.len)
					continue;
				*seen = binding_merge(*seen, b);
				if (to != BIND_NONE && b != to) {
					memcpy(base.p, to == BIND_TWIN ? map[m].twin : map[m].primary, base.len);
					changed++;
				}
			}
		}
		start = end + 1;
	}
	return changed;
}

int fstab_rebind(u8 *buf, size_t len, enum binding to)
{
	enum binding seen = BIND_NONE;
	return walk(buf, len, to, &seen);
}

enum binding fstab_binding(const u8 *buf, size_t len)
{
	enum binding seen = BIND_NONE;
	walk((u8 *)buf, len, BIND_NONE, &seen);
	return seen;
}

enum binding binding_merge(enum binding a, enum binding b)
{
	if (a == BIND_NONE)
		return b;
	if (b == BIND_NONE)
		return a;
	return a == b ? a : BIND_MIXED;
}

const char *binding_name(enum binding b)
{
	static const char *names[] = {"none", "primary", "twin", "mixed"};
	return names[b];
}
