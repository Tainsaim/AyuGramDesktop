/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugin_host.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Plugins {
namespace {

#if defined(_WIN32)

// WHY flush every line: the client is a GUI-subsystem binary, so
// anything still buffered when the process dies would vanish.
void Note(const char *message) {
	std::fputs(message, stderr);
	std::fputc('\n', stderr);
	std::fflush(stderr);
}

std::string ToUtf8(const wchar_t *wide, int size = -1) {
	const auto bytes = ::WideCharToMultiByte(
		CP_UTF8,
		0,
		wide,
		size,
		nullptr,
		0,
		nullptr,
		nullptr);
	auto result = std::string(bytes > 0 ? bytes - 1 : 0, '\0');
	if (bytes > 1) {
		::WideCharToMultiByte(
			CP_UTF8,
			0,
			wide,
			size,
			result.data(),
			bytes,
			nullptr,
			nullptr);
	}
	return result;
}

struct Version {
	const wchar_t *dll;
	const char *tag;
};
constexpr Version kVersions[] = {
	{ L"python314.dll", "314" },
	{ L"python313.dll", "313" },
	{ L"python312.dll", "312" },
	{ L"python311.dll", "311" },
	{ L"python310.dll", "310" },
	{ L"python39.dll", "39" },
};

// WHY a full path plus these flags: the client links code that calls
// SetDllDirectory before main, which removes PATH and the current
// directory from loader search, so a bare name never resolves to a
// non-system DLL in this process. DLL_LOAD_DIR starts the dependency
// search of python3XX.dll in its own directory, where vcruntime and
// friends live.
constexpr auto kLoadFlags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
	| LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;

HMODULE LoadByFullPath(const wchar_t *path) {
	return ::LoadLibraryExW(path, nullptr, kLoadFlags);
}

// Returns the loaded module and fills `report` with what worked, or null.
HMODULE FindPython(wchar_t *report, size_t reportSize) {
	for (const auto &version : kVersions) {
		if (const auto module = ::LoadLibraryW(version.dll)) {
			_snwprintf_s(report, reportSize, _TRUNCATE,
				L"%ls (by name)",
				version.dll);
			return module;
		}
	}

	// SearchPathW with an explicit PATH string reads the environment
	// variable directly, ignoring the restricted loader search order.
	const auto envSize = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
	if (envSize > 0) {
		const auto env = static_cast<wchar_t *>(
			std::malloc(envSize * sizeof(wchar_t)));
		if (env) {
			if (::GetEnvironmentVariableW(L"PATH", env, envSize) < envSize) {
				for (const auto &version : kVersions) {
					wchar_t full[MAX_PATH] = { 0 };
					if (::SearchPathW(
							env,
							version.dll,
							nullptr,
							MAX_PATH,
							full,
							nullptr)) {
						if (const auto module = LoadByFullPath(full)) {
							_snwprintf_s(report, reportSize, _TRUNCATE,
								L"%ls (via PATH)",
								full);
							std::free(env);
							return module;
						}
					}
				}
			}
			std::free(env);
		}
	}

	// Standard install locations, for hosts whose Python dir is not on
	// the PATH the process sees.
	for (const auto &version : kVersions) {
		for (const auto *prefix : {
				L"%LOCALAPPDATA%\\Programs\\Python\\Python",
				L"%ProgramFiles%\\Python",
			}) {
			wchar_t candidate[MAX_PATH] = { 0 };
			wchar_t expanded[MAX_PATH] = { 0 };
			_snwprintf_s(candidate, _TRUNCATE,
				L"%ls%s\\python%s.dll",
				prefix,
				version.tag,
				version.tag);
			if (!::ExpandEnvironmentStringsW(
					candidate,
					expanded,
					MAX_PATH)) {
				continue;
			}
			if (const auto module = LoadByFullPath(expanded)) {
				_snwprintf_s(report, reportSize, _TRUNCATE,
					L"%ls (install path)",
					expanded);
				return module;
			}
		}
	}
	return nullptr;
}

// The plugins folder sits next to the exe: the client is portable, and
// that is where the user already has logs and tdata.
std::wstring PluginsDir() {
	wchar_t exePath[MAX_PATH] = { 0 };
	const auto length = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	if (!length || length >= MAX_PATH) {
		return {};
	}
	const auto slash = std::wcsrchr(exePath, L'\\');
	if (!slash) {
		return {};
	}
	*slash = 0;
	return std::wstring(exePath) + L"\\plugins";
}

// WHY a background thread: Start() runs before the Qt launcher, so any
// plugin work here would delay client startup. The interpreter owns no
// Qt state, and plugins get the GIL through PyGILState_Ensure only.
void RunPlugins(
	void (*runSimpleString)(const char *),
	void *(*saveThread)(void),
	std::string dirUtf8) {
	const auto gil = PyGILState_Ensure();

	// WHY raw exec per file: each plugin gets its own namespace dict and
	// its own traceback, so one broken file neither sees another's
	// globals nor stops the rest.
	auto script = std::string();
	script += "import os, sys, traceback\n";
	script += "sys.stderr.write('PLUGIN HOST: loading plugins from %s\\n' % _dir)\n";
	script += "sys.stderr.flush()\n";
	script += "try:\n";
	script += "    _names = sorted(n for n in os.listdir(_dir)"
		" if n.lower().endswith('.plugin'))\n";
	script += "except OSError as _e:\n";
	script += "    sys.stderr.write('PLUGIN HOST: plugins folder unreadable: %s\\n' % _e)\n";
	script += "else:\n";
	script += "    for _n in _names:\n";
	script += "        _p = os.path.join(_dir, _n)\n";
	script += "        try:\n";
	script += "            with open(_p, 'rb') as _f:\n";
	script += "                _src = _f.read()\n";
	script += "            _g = {'__name__': 'ayu_plugin_' + os.path.splitext(_n)[0],"
		" '__file__': _p, 'plugins_dir': _dir}\n";
	script += "            exec(compile(_src, _p, 'exec'), _g)\n";
	script += "            sys.stderr.write('PLUGIN HOST: %s: loaded\\n' % _n)\n";
	script += "        except BaseException:\n";
	script += "            sys.stderr.write('PLUGIN HOST: %s failed:\\n' % _n)\n";
	script += "            traceback.print_exc()\n";
	script += "        sys.stderr.flush()\n";
	script += "    if not _names:\n";
	script += "        sys.stderr.write('PLUGIN HOST: no *.plugin files yet\\n')\n";
	script += "        sys.stderr.flush()\n";

	// The dir goes in through __main__ globals: backslashes become
	// slashes so no escaping is needed inside the script.
	auto head = std::string("_dir = ") + dirUtf8 + "\n";
	// WHY release the GIL here: the main thread holds it since
	// Py_InitializeEx, and the loader thread can only take it once the
	// owner lets go. Exactly one SaveThread per interpreter.
	saveThread();
	std::thread([=] {
		const auto gil = PyGILState_Ensure();
		runSimpleString(head.c_str());
		runSimpleString(script.c_str());
		PyGILState_Release(gil);
	}).detach();
}

#endif // _WIN32

} // namespace

bool Start() {
#if defined(_WIN32)
	wchar_t loaded[MAX_PATH] = { 0 };
	const auto module = FindPython(loaded, MAX_PATH);
	if (!module) {
		Note("PLUGIN HOST: no python dll (3.9-3.14) by name, PATH or install path");
		if (const auto *path = std::getenv("PATH")) {
			char line[256] = { 0 };
			std::snprintf(
				line,
				sizeof(line),
				"PLUGIN HOST: PATH starts with: %.200s",
				path);
			Note(line);
		} else {
			Note("PLUGIN HOST: PATH is not set");
		}
		return false;
	}
	{
		char line[2 * MAX_PATH] = { 0 };
		std::snprintf(line, sizeof(line), "PLUGIN HOST: loaded %ls", loaded);
		Note(line);
	}

	using initializeEx = void (*)(int);
	using isInitialized = int (*)();
	using runSimpleString = int (*)(const char *);
	using saveThread = void *(*)(void);

	const auto pyInitializeEx = reinterpret_cast<initializeEx>(
		::GetProcAddress(module, "Py_InitializeEx"));
	const auto pyIsInitialized = reinterpret_cast<isInitialized>(
		::GetProcAddress(module, "Py_IsInitialized"));
	const auto pyRunSimpleString = reinterpret_cast<runSimpleString>(
		::GetProcAddress(module, "PyRun_SimpleString"));
	const auto pySaveThread = reinterpret_cast<saveThread>(
		::GetProcAddress(module, "PyEval_SaveThread"));
	if (!pyInitializeEx || !pyIsInitialized
		|| !pyRunSimpleString || !pySaveThread) {
		Note("PLUGIN HOST: python entry points not found");
		return false;
	}

	// WHY: 0 leaves signal handlers to Qt; Python installing its own makes
	// an ordinary quit fault on the Qt thread.
	pyInitializeEx(0);
	if (!pyIsInitialized()) {
		Note("PLUGIN HOST: Py_InitializeEx failed");
		return false;
	}
	pyRunSimpleString(
		"import sys\n"
		"sys.stderr.write('PLUGIN HOST: python %d.%d.%d ready\\n'"
		"    % sys.version_info[:3])\n"
		"sys.stderr.flush()\n");

	const auto dir = PluginsDir();
	const auto folderOk = !dir.empty()
		&& (::CreateDirectoryW(dir.c_str(), nullptr)
			|| ::GetLastError() == ERROR_ALREADY_EXISTS);
	{
		char line[2 * MAX_PATH] = { 0 };
		std::snprintf(
			line,
			sizeof(line),
			"PLUGIN HOST: plugins folder %s: %ls",
			folderOk ? "ok" : "unavailable",
			dir.c_str());
		Note(line);
	}
	if (folderOk) {
		RunPlugins(pyRunSimpleString, pySaveThread, ToUtf8(dir.c_str()));
	}
	Note("PLUGIN HOST: started");
	return true;
#else
	std::fputs(
		"PLUGIN HOST: python host not implemented on this platform\n",
		stderr);
	return false;
#endif
}

} // namespace Plugins
