#define _GNU_SOURCE // fopencookie()

#include "fatDirect.h"

#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iosupport.h>
#include <libfatversion.h>

#include "directory.h"
#include "partition.h"
#include "cache.h"
#include "lock.h"

_Static_assert(_LIBFAT_MAJOR_ == 1 && _LIBFAT_MINOR_ == 1 && _LIBFAT_PATCH_ == 5,
	"libfat updated! Check that fatDirect.c still matches its internal structs");

// libfat's DIR_STATE_STRUCT (source/fatdir.h), what DIR_ITER::dirStruct points to
typedef struct {
	PARTITION *partition;
	DIR_ENTRY  currentEntry;
	uint32_t   startCluster;
	bool       inUse;
	bool       validEntry;
} FD_DIR_STATE;

_Static_assert(offsetof(FD_DIR_STATE, currentEntry) == 4, "unexpected DIR_STATE_STRUCT layout");

// From libfat's source/file_allocation_table.h
uint32_t _FAT_fat_nextCluster(PARTITION *partition, uint32_t cluster);

#define FD_CLUSTER_FIRST 2

// libfat's _FAT_fat_clusterToSector (cluster 0 is the FAT12/16 root)
static inline sec_t clusterToSector(const PARTITION *p, uint32_t cluster) {
	return (cluster >= FD_CLUSTER_FIRST)
		? ((cluster - FD_CLUSTER_FIRST) * (sec_t)p->sectorsPerCluster) + p->dataStart
		: p->rootDirStart;
}

static inline bool validCluster(const PARTITION *p, uint32_t cluster) {
	return cluster >= FD_CLUSTER_FIRST && cluster <= p->fat.lastCluster;
}

static inline uint32_t le32(const uint8_t *b) {
	return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

// ---- Recorded folders

typedef struct {
	uint32_t nameOff; // into DirTable::names, lower-cased
	uint32_t startCluster;
	uint32_t size;
	DIR_ENTRY_POSITION alias; // the file's 8.3 directory entry
	uint8_t  aliasName[11];
} Entry;

typedef struct {
	char       key[PATH_MAX]; // lower-cased folder path with trailing slash
	PARTITION *partition;
	Entry     *entries;
	int        count, cap;
	char      *names;
	uint32_t   namesLen, namesCap;
	uint32_t   lastUse;
	bool       used;
} DirTable;

// Enough for the ROM folder, the box art folder and some back and forth;
// the least recently used one is replaced.
#define FD_MAX_TABLES 4
static DirTable tables[FD_MAX_TABLES];
static uint32_t useClock = 0;
static DirTable *building = NULL;

static void lowerAscii(char *s) {
	for (; *s; s++) {
		if (*s >= 'A' && *s <= 'Z') *s += 'a' - 'A';
	}
}

static void freeTable(DirTable *t) {
	free(t->entries);
	free(t->names);
	memset(t, 0, sizeof(*t));
}

// Lower-cased key for dir (dirLen bytes; relative paths resolve against the cwd)
static bool makeKey(char *out, size_t outSize, const char *dir, size_t dirLen) {
	size_t len = 0;
	if (memchr(dir, ':', dirLen) == NULL) {
		if (!getcwd(out, outSize)) return false;
		len = strlen(out);
		if (len == 0 || out[len - 1] != '/') {
			if (len + 1 >= outSize) return false;
			out[len++] = '/';
		}
		if (dirLen >= 2 && dir[0] == '.' && dir[1] == '/') {
			dir += 2;
			dirLen -= 2;
		} else if (dirLen == 1 && dir[0] == '.') {
			dirLen = 0;
		}
	}
	if (len + dirLen + 2 > outSize) return false;
	memcpy(out + len, dir, dirLen);
	len += dirLen;
	if (len == 0 || out[len - 1] != '/') out[len++] = '/';
	out[len] = '\0';
	lowerAscii(out);
	return true;
}

void fatDirectReset(void) {
	for (int i = 0; i < FD_MAX_TABLES; i++) freeTable(&tables[i]);
	building = NULL;
}

void fatDirectBeginDir(const char *dirPath, DIR *dir) {
	building = NULL;
	if (!dir || !dir->dirData || !dir->dirData->dirStruct) return;

	char key[PATH_MAX];
	if (!dirPath) dirPath = "";
	if (!makeKey(key, sizeof(key), dirPath, strlen(dirPath))) return;

	DirTable *t = NULL;
	for (int i = 0; i < FD_MAX_TABLES && !t; i++) {
		if (tables[i].used && strcmp(tables[i].key, key) == 0) t = &tables[i];
	}
	for (int i = 0; i < FD_MAX_TABLES && !t; i++) {
		if (!tables[i].used) t = &tables[i];
	}
	if (!t) {
		t = &tables[0];
		for (int i = 1; i < FD_MAX_TABLES; i++) {
			if (tables[i].lastUse < t->lastUse) t = &tables[i];
		}
	}
	freeTable(t);

	const FD_DIR_STATE *state = (const FD_DIR_STATE *)dir->dirData->dirStruct;
	strcpy(t->key, key);
	t->partition = state->partition;
	t->lastUse = ++useClock;
	t->used = true;
	building = t;
}

void fatDirectNoteEntry(DIR *dir) {
	DirTable *t = building;
	if (!t || !dir || !dir->dirData || !dir->dirData->dirStruct) return;

	// libfat has already read the entry the next readdir() returns
	const FD_DIR_STATE *state = (const FD_DIR_STATE *)dir->dirData->dirStruct;
	if (!state->validEntry || state->partition != t->partition) return;

	const DIR_ENTRY *e = &state->currentEntry;
	if (e->entryData[DIR_ENTRY_attributes] & ATTRIB_DIR) return;

	const size_t nameLen = strnlen(e->filename, NAME_MAX);
	if (nameLen == 0 || nameLen >= NAME_MAX) return;

	if (t->count == t->cap) {
		const int cap = t->cap ? t->cap * 2 : 64;
		Entry *n = (Entry *)realloc(t->entries, cap * sizeof(Entry));
		if (!n) return;
		t->entries = n;
		t->cap = cap;
	}
	if (t->namesLen + nameLen + 1 > t->namesCap) {
		uint32_t cap = t->namesCap ? t->namesCap * 2 : 4096;
		while (cap < t->namesLen + nameLen + 1) cap *= 2;
		char *n = (char *)realloc(t->names, cap);
		if (!n) return;
		t->names = n;
		t->namesCap = cap;
	}

	Entry *out = &t->entries[t->count++];
	out->nameOff = t->namesLen;
	memcpy(t->names + t->namesLen, e->filename, nameLen + 1);
	lowerAscii(t->names + t->namesLen);
	t->namesLen += nameLen + 1;

	out->startCluster = _FAT_directory_entryGetCluster(state->partition, e->entryData);
	out->size = le32(e->entryData + DIR_ENTRY_fileSize);
	out->alias = e->dataEnd;
	memcpy(out->aliasName, e->entryData, sizeof(out->aliasName));
}

static const char *sortNames; // qsort has no context argument

static int compareEntries(const void *a, const void *b) {
	return strcmp(sortNames + ((const Entry *)a)->nameOff, sortNames + ((const Entry *)b)->nameOff);
}

void fatDirectEndDir(void) {
	DirTable *t = building;
	building = NULL;
	if (!t || t->count == 0) return;
	sortNames = t->names;
	qsort(t->entries, t->count, sizeof(Entry), compareEntries);
}

static const Entry *findEntry(const DirTable *t, const char *name) {
	int lo = 0, hi = t->count - 1;
	while (lo <= hi) {
		const int mid = (lo + hi) / 2;
		const int c = strcmp(name, t->names + t->entries[mid].nameOff);
		if (c == 0) return &t->entries[mid];
		if (c < 0) hi = mid - 1;
		else lo = mid + 1;
	}
	return NULL;
}

// Re-reads the 8.3 entry (normally cached) to check the file wasn't deleted,
// replaced or resized since the folder was listed.
static bool entryStillValid(PARTITION *p, const Entry *e) {
	uint8_t data[DIR_ENTRY_DATA_SIZE];
	if (!_FAT_cache_readPartialSector(p->cache, data,
			clusterToSector(p, e->alias.cluster) + e->alias.sector,
			e->alias.offset * DIR_ENTRY_DATA_SIZE, DIR_ENTRY_DATA_SIZE))
		return false;
	return memcmp(data, e->aliasName, sizeof(e->aliasName)) == 0
		&& !(data[DIR_ENTRY_attributes] & ATTRIB_DIR)
		&& _FAT_directory_entryGetCluster(p, data) == e->startCluster
		&& le32(data + DIR_ENTRY_fileSize) == e->size;
}

// ---- FILE* side

typedef struct {
	PARTITION *partition;
	uint32_t   startCluster;
	uint32_t   size;
	int64_t    pos;
	uint32_t   chainIndex;   // last cluster reached in the chain,
	uint32_t   chainCluster; // so forward reads don't re-walk it
} FdFile;

// Whole sectors are read into this, then copied: the DSi SD driver has the ARM7
// write the buffer, so it must be plain main RAM, which callers' buffers aren't
// always (stack in DTCM, box art cache at 0x0D540000). libfat likewise only gives
// the driver its own heap cache pages.
#define FD_BOUNCE_SIZE (32 * 1024)
static uint8_t *bounce = NULL;

static uint8_t *getBounce(void) {
	if (!bounce) bounce = (uint8_t *)memalign(32, FD_BOUNCE_SIZE);
	return bounce;
}

// Cluster number of the file's idx-th cluster, or 0 if the chain is broken
static uint32_t clusterAt(FdFile *f, uint32_t idx) {
	if (idx < f->chainIndex) {
		f->chainIndex = 0;
		f->chainCluster = f->startCluster;
	}
	while (f->chainIndex < idx) {
		const uint32_t next = _FAT_fat_nextCluster(f->partition, f->chainCluster);
		if (!validCluster(f->partition, next)) return 0;
		f->chainCluster = next;
		f->chainIndex++;
	}
	return f->chainCluster;
}

static ssize_t fdRead(void *cookie, char *buf, size_t n) {
	FdFile *f = (FdFile *)cookie;
	if (f->pos >= f->size) return 0;
	if (n > f->size - f->pos) n = f->size - f->pos;

	PARTITION *p = f->partition;
	const uint32_t bps = p->bytesPerSector;
	const uint32_t bpc = p->bytesPerCluster;
	const uint32_t spc = p->sectorsPerCluster;
	uint8_t *const bb = (bps <= FD_BOUNCE_SIZE) ? getBounce() : NULL;
	const uint32_t bounceSectors = FD_BOUNCE_SIZE / bps;
	size_t done = 0;
	bool error = false;

	_FAT_lock(&p->lock);
	while (done < n) {
		const uint32_t pos = (uint32_t)f->pos;
		const uint32_t idx = pos / bpc;
		const uint32_t offInCluster = pos % bpc;
		const uint32_t cluster = clusterAt(f, idx);
		if (!cluster) { error = true; break; }

		const uint32_t secInCluster = offInCluster / bps;
		const uint32_t offInSector = offInCluster % bps;
		const sec_t sector = clusterToSector(p, cluster) + secInCluster;
		const size_t left = n - done;
		char *dst = buf + done;
		size_t got;

		if (offInSector == 0 && left >= bps && bb) {
			// Whole sectors, past libfat's cache so box art doesn't evict the folder,
			// running on into physically adjacent clusters
			uint32_t want = left / bps;
			if (want > bounceSectors) want = bounceSectors;
			uint32_t count = spc - secInCluster;
			if (count > want) count = want;
			uint32_t runIndex = idx, runCluster = cluster;
			while (count + spc <= want) {
				const uint32_t next = _FAT_fat_nextCluster(p, runCluster);
				if (next != runCluster + 1 || !validCluster(p, next)) break;
				runCluster = next;
				runIndex++;
				count += spc;
			}
			if (!p->disc->readSectors(sector, count, bb)) { error = true; break; }
			got = (size_t)count * bps;
			memcpy(dst, bb, got);
			f->chainIndex = runIndex;
			f->chainCluster = runCluster;
		} else if (offInSector == 0 && left >= bps) {
			uint32_t count = left / bps;
			if (count > spc - secInCluster) count = spc - secInCluster;
			if (!_FAT_cache_readSectors(p->cache, sector, count, dst)) { error = true; break; }
			got = (size_t)count * bps;
		} else {
			got = bps - offInSector;
			if (got > left) got = left;
			if (!_FAT_cache_readPartialSector(p->cache, dst, sector, offInSector, got)) { error = true; break; }
		}

		f->pos += got;
		done += got;
	}
	_FAT_unlock(&p->lock);

	if (error && done == 0) {
		errno = EIO;
		return -1;
	}
	return (ssize_t)done;
}

#ifdef __LARGE64_FILES
static int fdSeek(void *cookie, _off64_t *off, int whence)
#else
static int fdSeek(void *cookie, off_t *off, int whence)
#endif
{
	FdFile *f = (FdFile *)cookie;
	int64_t base;
	switch (whence) {
		case SEEK_SET: base = 0; break;
		case SEEK_CUR: base = f->pos; break;
		case SEEK_END: base = f->size; break;
		default: errno = EINVAL; return -1;
	}
	const int64_t target = base + (int64_t)*off;
	if (target < 0 || target > 0xFFFFFFFFLL) {
		errno = EINVAL;
		return -1;
	}
	f->pos = target;
	*off = target;
	return 0;
}

static int fdClose(void *cookie) {
	free(cookie);
	return 0;
}

static FILE *openRecorded(const char *path) {
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	const size_t dirLen = slash ? (size_t)(slash + 1 - path) : 0;
	if (*name == '\0') return NULL;

	char key[PATH_MAX];
	if (!makeKey(key, sizeof(key), path, dirLen)) return NULL;

	DirTable *t = NULL;
	for (int i = 0; i < FD_MAX_TABLES && !t; i++) {
		if (tables[i].used && tables[i].count > 0 && strcmp(tables[i].key, key) == 0) t = &tables[i];
	}
	if (!t) return NULL;
	t->lastUse = ++useClock;

	char lname[NAME_MAX + 1];
	const size_t nameLen = strlen(name);
	if (nameLen > NAME_MAX) return NULL;
	memcpy(lname, name, nameLen + 1);
	lowerAscii(lname);

	const Entry *e = findEntry(t, lname);
	if (!e) return NULL;

	PARTITION *p = t->partition;
	_FAT_lock(&p->lock);
	const bool valid = entryStillValid(p, e);
	_FAT_unlock(&p->lock);
	if (!valid) return NULL;
	if (e->size > 0 && !validCluster(p, e->startCluster)) return NULL;

	FdFile *f = (FdFile *)malloc(sizeof(FdFile));
	if (!f) return NULL;
	f->partition = p;
	f->startCluster = e->startCluster;
	f->size = e->size;
	f->pos = 0;
	f->chainIndex = 0;
	f->chainCluster = e->startCluster;

	cookie_io_functions_t fns = { fdRead, NULL, fdSeek, fdClose };
	FILE *fp = fopencookie(f, "rb", fns);
	if (!fp) free(f);
	return fp;
}

FILE *fatDirectFopen(const char *path) {
	if (!path) return NULL;
	FILE *fp = openRecorded(path);
	return fp ? fp : fopen(path, "rb");
}
