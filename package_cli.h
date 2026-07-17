#ifndef PACKAGE_CLI_H
#define PACKAGE_CLI_H

// `nova install <name> <local-path>` — copies a .nova file into the
// packages directory (~/.nova/packages/, or ./packages/ as a fallback
// if no home directory is available) under <name>.nova, and records it
// in a small manifest so list/info/remove have something to work with.
int packageInstall(const char* name, const char* localPath);

// `nova list` — prints every installed package's name.
int packageList(void);

// `nova info <name>` — prints size, install date, and source path for
// one installed package.
int packageInfo(const char* name);

// `nova remove <name>` — deletes the package's .nova file and its
// manifest entry.
int packageRemove(const char* name);

// Returns 1 if `name` is already recorded in the package manifest (i.e.
// already installed, "in use" in the plugin-folder sense), 0 otherwise.
// Used by the plugins-folder auto-discovery in plugin_loader.c to decide
// whether a dropped-in .nova file still needs to be offered to the user.
int packageIsInstalled(const char* name);

#endif
