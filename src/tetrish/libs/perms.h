#ifndef PERMS_H
#define PERMS_H
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/stat.h>

void perms_to_string(mode_t mode, char str[11]);

#endif
