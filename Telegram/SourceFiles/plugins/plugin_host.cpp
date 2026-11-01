/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugin_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Plugins {

bool Start() {
#if defined(_WIN32)
	// WHY flush every line: the client is a GUI-subsystem binary, so
	// anything still buffered when the process dies would vanish.
	const auto note = [](const char *message) {
		std::fputs(message, stderr);
		std::fputc('\n', stderr);
		std::fflush(stderr);
	};

	// The probe loads CPython at runtime instead of linking a Py import
	// library: a hard link would pin the exe to python3XX.dll at load time
	// and stop it from starting on hosts without that exact version.
	struct Version {
		const char *tag;
		const char *dll;
	};
	static constexpr Version versions[] = {
		{ "313", "python313.dll" },
		{ "312", "python312.dll" },
		{ "311", "python311.dll" },
		{ "310", "python310.dll" },
		{ "39", "python39.dll" },
	};

	HMODULE module = nullptr;
	char loaded[512] = { 0 };
	for (const auto &version : versions) {
		::SetLastError(0);
		module = ::LoadLibraryA(version.dll);
		if (module) {
			std::snprintf(loaded, sizeof(loaded), "%s (by name)", version.dll);
			break;
		}
		char line[256] = { 0 };
		std::snprintf(
			line,
			sizeof(line),
			"PLUGIN HOST: LoadLibrary(\"%s\") -> err=%lu",
			version.dll,
			static_cast<unsigned long>(::GetLastError()));
		note(line);
	}

	if (!module) {
		// Standard install locations, for hosts whose Python dir is not on
		// the PATH the process sees.
		static constexpr const char *prefixes[] = {
			"%LOCALAPPDATA%\\Programs\\Python\\Python",
			"%ProgramFiles%\\Python",
		};
		for (const auto &version : versions) {
			for (const auto *prefix : prefixes) {
				char candidate[600] = { 0 };
				char expanded[600] = { 0 };
				std::snprintf(
					candidate,
					sizeof(candidate),
					"%s%s\\python%s.dll",
					prefix,
					version.tag,
					version.tag);
				::ExpandEnvironmentStringsA(
					candidate,
					expanded,
					sizeof(expanded));
				::SetLastError(0);
				module = ::LoadLibraryA(expanded);
				if (module) {
					std::snprintf(
						loaded,
						sizeof(loaded),
						"%s (full path)",
						expanded);
					break;
				}
			}
			if (module) {
				break;
			}
		}
	}

	if (!module) {
		note("PLUGIN HOST: no python dll (3.9-3.13) by name or install path");
		if (const auto *path = std::getenv("PATH")) {
			char line[256] = { 0 };
			std::snprintf(
				line,
				sizeof(line),
				"PLUGIN HOST: PATH starts with: %.200s",
				path);
			note(line);
		} else {
			note("PLUGIN HOST: PATH is not set");
		}
		return false;
	}
	{
		char line[600] = { 0 };
		std::snprintf(line, sizeof(line), "PLUGIN HOST: loaded %s", loaded);
		note(line);
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
		note("PLUGIN HOST: python entry points not found");
		return false;
	}

	// WHY: 0 leaves signal handlers to Qt; Python installing its own makes
	// an ordinary quit fault on the Qt thread.
	pyInitializeEx(0);
	if (!pyIsInitialized()) {
		note("PLUGIN HOST: Py_InitializeEx failed");
		return false;
	}
	pyRunSimpleString(
		"import sys\n"
		"sys.stderr.write('PLUGIN HOST: python %d.%d.%d ready\\n'"
		"    % sys.version_info[:3])\n"
		"sys.stderr.flush()\n");
	note("PLUGIN HOST: started");
	return true;
#else
	std::fputs(
		"PLUGIN HOST: python host not implemented on this platform\n",
		stderr);
	return false;
#endif
}

} // namespace Plugins
