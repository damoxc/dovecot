#ifndef MKDIR_PARENTS_H
#define MKDIR_PARENTS_H

/* Create path and all the directories under it if needed. Permissions for
   existing directories isn't changed. Returns 0 if ok. If directory already
   exists, returns -1 with errno=EXIST. */
int mkdir_parents(const char *path, mode_t mode);

#endif
