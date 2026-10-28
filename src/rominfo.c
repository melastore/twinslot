#include "rominfo.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "erofs.h"
#include "lp.h"
#include "part.h"

/*
 * ROM version properties, most specific first; the build id is the fallback. Some ROMs keep theirs in
 * product or system_ext rather than system, and some values don't say which ROM they are, so the label
 * is put in front unless the value already contains its word.
 */
static const struct {
	const char *key, *label, *word;
} name_props[] = {
	{ "ro.modversion", NULL, NULL },
	{ "ro.voltage.version", "VoltageOS", "voltage" },
	{ "ro.axion.build.version", "AxionOS", "axion" },
	{ "ro.axion.version", "AxionOS", "axion" },
	{ "ro.evolution.display.version", "Evolution X", "evolution" },
	{ "ro.evolution.version", "Evolution X", "evolution" },
	{ "ro.yaap.version", "YAAP", "yaap" },
	{ "ro.infinity.version", "Infinity-X", "infinity" },
	{ "ro.lineage.display.version", "LineageOS", "lineage" },
	{ "ro.build.display.id", NULL, NULL },
};

/* Where each image keeps its build.prop. */
static const struct {
	const char *part, *paths[2];
} prop_files[] = {
	{ "system", { "/system/build.prop", "/build.prop" } },
	{ "system_ext", { "/etc/build.prop", "/system_ext/etc/build.prop" } },
	{ "product", { "/etc/build.prop", "/product/etc/build.prop" } },
};

static bool prop(const char *props, const char *key, char *val, size_t cap)
{
	size_t klen = strlen(key);
	for (const char *line = props; line && *line;) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		if (len && line[len - 1] == '\r')
			len--;
		if (len > klen && !memcmp(line, key, klen) && line[klen] == '=') {
			size_t n = MIN(len - klen - 1, cap - 1);
			memcpy(val, line + klen + 1, n);
			val[n] = 0;
			/* Shell-safe and one line: the app shows it and passes it around. */
			for (char *c = val; *c; c++)
				if ((unsigned char)*c < 0x20 || *c == '\'' || *c == '"' || *c == '`' || *c == '\\')
					*c = ' ';
			return n > 0;
		}
		line = end ? end + 1 : NULL;
	}
	return false;
}

/* "12.3" names nothing: without a label a value needs a letter to say which ROM it is. */
static bool has_letter(const char *s)
{
	for (; *s; s++)
		if ((*s | 0x20) >= 'a' && (*s | 0x20) <= 'z')
			return true;
	return false;
}

static bool has_word(const char *s, const char *word)
{
	size_t n = strlen(word);
	for (; *s; s++) {
		size_t k = 0;
		while (k < n && s[k] && (s[k] | 0x20) == word[k])
			k++;
		if (k == n)
			return true;
	}
	return false;
}

/* The build.prop of <part>_<slot> as a string, or NULL. Quiet: an ext4 image or a missing file is normal. */
static char *read_props(int super_fd, struct lp_metadata *m, int slot, size_t which)
{
	char name[32];
	snprintf(name, sizeof(name), "%s_%c", prop_files[which].part, slot_letter(slot));
	int i = lp_find_partition(m, name);
	if (i < 0 || !m->parts[i].nextents)
		return NULL;
	struct part p;
	part_logical(&p, super_fd, &m->parts[i]);
	struct erofs fs;
	struct erofs_inode ino;
	u8 *data = NULL;
	char *props = NULL;
	int saved = dup(2), devnull = open("/dev/null", 1);
	if (saved >= 0 && devnull >= 0)
		dup2(devnull, 2);
	if (!erofs_open(&fs, &p) &&
	    (!erofs_lookup(&fs, prop_files[which].paths[0], &ino) || !erofs_lookup(&fs, prop_files[which].paths[1], &ino)) &&
	    erofs_is_reg(&ino) && ino.size < (1 << 20) && !erofs_read(&fs, &ino, &data)) {
		props = xmalloc((size_t)ino.size + 1);
		memcpy(props, data, (size_t)ino.size);
		props[ino.size] = 0;
		free(data);
	}
	if (saved >= 0) {
		dup2(saved, 2);
		close(saved);
	}
	if (devnull >= 0)
		close(devnull);
	return props;
}

int rom_info(int slot, struct rom_info *ri)
{
	memset(ri, 0, sizeof(*ri));
	struct part super;
	struct lp_metadata m;
	int r = super_read_slot(&super, false, slot, &m);
	if (r)
		return r;
	char name[32];
	snprintf(name, sizeof(name), "system_%c", slot_letter(slot));
	int i = lp_find_partition(&m, name);
	ri->installed = i >= 0 && m.parts[i].nextents > 0;
	if (ri->installed)
		ri->system_bytes = lp_partition_size(&m.parts[i]);
	if (ri->installed) {
		char *props[ARRAY_SIZE(prop_files)];
		for (size_t f = 0; f < ARRAY_SIZE(prop_files); f++)
			props[f] = read_props(super.fd, &m, slot, f);
		if (props[0]) {
			char val[sizeof(ri->name) - 16]; /* room for the longest label */
			for (size_t k = 0; k < ARRAY_SIZE(name_props) && !ri->name[0]; k++)
				for (size_t f = 0; f < ARRAY_SIZE(prop_files) && !ri->name[0]; f++) {
					if (!props[f] || !prop(props[f], name_props[k].key, val, sizeof(val)))
						continue;
					if (!name_props[k].label && !has_letter(val))
						continue;
					if (name_props[k].label && !has_word(val, name_props[k].word))
						snprintf(ri->name, sizeof(ri->name), "%s %s", name_props[k].label, val);
					else
						memcpy(ri->name, val, sizeof(val));
				}
			prop(props[0], "ro.build.version.release", ri->android, sizeof(ri->android));
			prop(props[0], "ro.build.date.utc", ri->build, sizeof(ri->build));
			ri->readable = true;
		}
		for (size_t f = 0; f < ARRAY_SIZE(prop_files); f++)
			free(props[f]);
	}
	lp_free(&m);
	part_close(&super);
	return EXIT_OK;
}
