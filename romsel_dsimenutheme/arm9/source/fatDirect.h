// Opening files without libfat's folder search.
//
// libfat's fopen() walks the folder entry by entry, comparing every long name, so a
// file near the end of a large folder takes ~20 ms to open. While a folder is listed
// anyway, fatDirectNoteEntry() records where each file starts; fatDirectFopen() then
// opens a recorded file straight from there as an ordinary read-only FILE*.
// Unrecorded or changed files fall back to fopen().
//
// Relies on libfat 1.1.5 internals (see the copies in universal/arm9/include).
#pragma once

#include <stdio.h>
#include <dirent.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts recording the folder opened as dir. dirPath is the path given to opendir(),
// or NULL for ".".
void fatDirectBeginDir(const char *dirPath, DIR *dir);

// Call right *before* each readdir(dir).
void fatDirectNoteEntry(DIR *dir);

void fatDirectEndDir(void);

// Forgets everything recorded (call if a drive is remounted).
void fatDirectReset(void);

// Same as fopen(path, "rb").
FILE *fatDirectFopen(const char *path);

#ifdef __cplusplus
}
#endif
