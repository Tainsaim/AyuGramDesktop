/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugin_host.h"

#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Plugins {

bool Start() {
#if defined(_WIN32)
	// The probe loads CPython at runtime instead of linking a Py import
	// library: a hard link would pin the exe to python3XX.dll at load time
	// and stop it from starting on hosts without that exact version.
	static constexpr const char *names[] = {
		"python313.dll",
		"python312.dll",
		"python311.dll",
		"python310.dll",
		"python39.dll",
	};

	HMODULE module = nullptr;
	for (const auto *name : names) {
		module = ::LoadLibraryA(name);
		if (module) {
			break;
		}
	}
	if (!module) {
		std::fputs(
			"PLUGIN HOST: no python dll (3.9-3.13) on the search path\n",
			stderr);
		return false;
	}

	using initializeEx = void (*)(int);
	using isInitialized = int (*)();
	using runSimpleString = int (*)(const char *);

	const auto pyInitializeEx = reinterpret_cast<initializeEx>(
		::GetProcAddress(module, "Py_InitializeEx"));
	const auto pyIsInitialized = reinterpret_cast<isInitialized>(
		::GetProcAddress(module, "Py_IsInitialized"));
	const auto pyRunSimpleString = reinterpret_cast<runSimpleString>(
		::GetProcAddress(module, "PyRun_SimpleString"));
	if (!pyInitializeEx || !pyIsInitialized || !pyRunSimpleString) {
		std::fputs("PLUGIN HOST: python entry points not found\n", stderr);
		return false;
	}

	// WHY: 0 leaves signal handlers to Qt; Python installing its own makes
	// an ordinary quit fault on the Qt thread.
	pyInitializeEx(0);
	if (!pyIsInitialized()) {
		std::fputs("PLUGIN HOST: Py_InitializeEx failed\n", stderr);
		return false;
	}
	pyRunSimpleString(
		"import sys\n"
		"sys.stderr.write('PLUGIN HOST: python %d.%d.%d ready\\n'"
		"    % sys.version_info[:3])\n"
		"sys.stderr.flush()\n");
	std::fputs("PLUGIN HOST: started\n", stderr);
	return true;
#else
	std::fputs(
		"PLUGIN HOST: python host not implemented on this platform\n",
		stderr);
	return false;
#endif
}

} // namespace Plugins