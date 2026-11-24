#pragma once
/* does this disc image hold the named file? Leaves the open disc as it was. */
int disc_has_file(const char *path, const char *name);
