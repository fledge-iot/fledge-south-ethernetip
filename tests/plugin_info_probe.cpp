/*
 * Fledge EtherNet/IP south plugin tests.
 *
 * Copyright (c) 2026 Industrial Intelligence Group, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Industrial Intelligence Group
 */
/**
 * plugin_info_probe - regression guard for plugin discovery.
 *
 * Fledge does not use its own PluginManager to enumerate available plugins for
 * the UI. It runs the standalone $FLEDGE_ROOT/extras/C/get_plugin_info utility,
 * which dlopen()s the plugin .so with RTLD_GLOBAL|RTLD_LAZY, calls plugin_info(),
 * and prints a JSON line that fledge.services.core.api.utils parses.
 *
 * That utility links *only* libdl. The plugin .so has no DT_NEEDED on
 * libcommon-lib - it gets Logger, ConfigCategory and friends from the south
 * service that hosts it at runtime. So any libcommon-lib call reachable from
 * plugin_info() kills the utility with an unresolved-symbol error the moment it
 * is called. Nothing reaches stdout, Fledge reports
 *
 *   Failed to parse JSON data returned from the plugin information of
 *   ethernetip, Expecting value line 1 column 1
 *
 * and the plugin silently vanishes from the "add south service" plugin list
 * while already-configured instances keep running.
 *
 * The gtest suites cannot catch this: they link common-lib, so the symbol
 * resolves for them. This probe deliberately links nothing but dl, reproducing
 * the utility's environment exactly, and is registered as a CTest.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <rapidjson/document.h>

#include <plugin_api.h>

typedef PLUGIN_INFORMATION *(*info_fn_t)();

int main(int argc, char *argv[])
{
	const char *soPath =
#ifdef PLUGIN_SO_PATH
		PLUGIN_SO_PATH;
#else
		"./libethernetip.so";
#endif
	if (argc > 1)
		soPath = argv[1];

	// Same flags get_plugin_info uses. RTLD_LAZY matters: it is what defers the
	// unresolved-symbol failure to the plugin_info() call below, exactly as it
	// does in production.
	void *hndl = dlopen(soPath, RTLD_GLOBAL | RTLD_LAZY);
	if (!hndl)
	{
		fprintf(stderr, "FAIL: dlopen(%s): %s\n", soPath, dlerror());
		return 1;
	}

	dlerror();
	info_fn_t infoEntry = (info_fn_t)dlsym(hndl, "plugin_info");
	const char *dlErr = dlerror();
	if (!infoEntry || dlErr)
	{
		fprintf(stderr, "FAIL: dlsym(plugin_info): %s\n", dlErr ? dlErr : "null");
		return 1;
	}

	// If plugin_info() reaches into libcommon-lib, the process dies right here
	// with "symbol lookup error" and the test fails - which is the point.
	PLUGIN_INFORMATION *info = (*infoEntry)();
	if (!info)
	{
		fprintf(stderr, "FAIL: plugin_info() returned NULL\n");
		return 1;
	}

	if (!info->name || !info->version || !info->type || !info->interface || !info->config)
	{
		fprintf(stderr, "FAIL: plugin_info() returned a struct with NULL fields\n");
		return 1;
	}

	// Byte-for-byte the line get_plugin_info emits and Fledge json.loads()es.
	char line[65536];
	int n = snprintf(line, sizeof(line),
		"{\"name\": \"%s\", \"version\": \"%s\", \"type\": \"%s\", "
		"\"interface\": \"%s\", \"flag\": %d, \"config\": %s}",
		info->name, info->version, info->type, info->interface,
		info->options, info->config);
	if (n < 0 || (size_t)n >= sizeof(line))
	{
		fprintf(stderr, "FAIL: plugin information does not fit in %zu bytes\n", sizeof(line));
		return 1;
	}

	rapidjson::Document doc;
	if (doc.Parse(line).HasParseError())
	{
		fprintf(stderr, "FAIL: emitted plugin information is not valid JSON "
			"(offset %zu) - Fledge would reject it\n%s\n",
			doc.GetErrorOffset(), line);
		return 1;
	}

	if (!doc.HasMember("config") || !doc["config"].IsObject())
	{
		fprintf(stderr, "FAIL: default configuration is not a JSON object\n");
		return 1;
	}

	if (strcmp(info->name, "ethernetip") != 0)
	{
		fprintf(stderr, "FAIL: plugin name is \"%s\", expected \"ethernetip\"\n", info->name);
		return 1;
	}

	printf("%s\n", line);
	fprintf(stderr, "OK: %s discoverable as \"%s\" version %s\n",
		soPath, info->name, info->version);

	// Deliberately no dlclose(): the plugin registers atexit-ish state through
	// libplctag, and get_plugin_info does not close either.
	return 0;
}
