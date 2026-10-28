#pragma once
#include <stdio.h>
#include <sys/stat.h>
FILE *legacy_file(const char *path, const char *mode);
int legacy_stat(const char *path, struct stat *out);
#define fopen legacy_file
#define stat(path, out) legacy_stat(path, out)
