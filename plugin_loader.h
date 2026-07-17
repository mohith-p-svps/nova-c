#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

// Looks for a "plugins" folder next to the running nova executable
// (NOT the current working directory — this is what lets someone hand
// a folder containing nova.exe + plugins/*.nova to another machine and
// have it work regardless of where it's launched from). For every
// *.nova file found there:
//
//   - if a package with that name is already installed (see
//     packageIsInstalled), it's already "in use" — silently do nothing
//     and let the program continue starting up.
//   - otherwise, prompt the user on the console asking whether to start
//     using it. Answering yes installs it (via packageInstall, so it
//     immediately becomes available to any script via `use <name>`) and
//     it will be silently recognized as already-in-use on every future
//     launch. Answering no leaves it uninstalled, so it will be offered
//     again the next time nova starts.
//
// `argv0` should be argv[0] from main — used to locate the executable's
// own directory. Safe to call unconditionally: if there's no plugins
// folder next to the executable (the common case), this does nothing.
// Answers "does a plugin named `name` exist in the plugins folder next
// to the running executable?" — `name` may be given with or without the
// trailing .nova. If found, fills `outPath` with the full path and
// returns 1; otherwise returns 0 and leaves `outPath` untouched.
//
// This is the only way a package name gets resolved to a file for
// `nova install` (see main.c) — deliberately narrower than a general
// "install from any path" command, so installing is always sourced from
// the one folder meant for it. It also gives you a manual escape hatch
// around the once-per-session automatic check in checkAndOfferPlugins:
// if that check happens to get skipped for a session (e.g. a reused
// process ID — see the comment on getSessionMarkerPath), you can still
// run `nova install <name>` yourself instead of waiting for a new
// terminal.
int findPluginInFolder(const char* argv0, const char* name, char* outPath, size_t outSize);

void checkAndOfferPlugins(const char* argv0);

#endif
