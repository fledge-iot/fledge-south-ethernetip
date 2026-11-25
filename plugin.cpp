/*
 * Fledge EtherNet/IP south plugin.
 *
 * Copyright (c) 2026 Industrial Intelligence Group, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Industrial Intelligence Group
 */
#include <mutex>
#include <string>
#include <vector>
#include <stdexcept>

#include <plugin_api.h>
#include <config_category.h>
#include <reading.h>
#include <logger.h>

#include <libplctag.h>
#include <version.h>

#include "plctag.h"

using namespace std;

static const char *def_cfg = QUOTE({
	"plugin" : {
		"description" : "EtherNet/IP south plugin",
		"type" : "string",
		"default" : "ethernetip",
		"readonly" : "true"
	},
	"asset" : {
		"description" : "Asset name for readings",
		"type" : "string",
		"default" : "PLCTags",
		"displayName" : "Asset Name",
		"order" : "1"
	},
	"address" : {
		"description" : "PLC IP address or hostname",
		"type" : "string",
		"default" : "",
		"displayName" : "PLC Address",
		"order" : "2"
	},
	"plc" : {
		"description" : "PLC type",
		"type" : "enumeration",
		"default" : "controllogix",
		"options" : ["controllogix", "logix5000", "micro800", "micrologix", "slc500", "omron-njnx"],
		"displayName" : "PLC Type",
		"order" : "3"
	},
	"path" : {
		"description" : "CIP routing path (e.g. 1,0). Ignored for Micro800. Leave blank for SLC 500 and MicroLogix unless routing over a DH+ bridge.",
		"type" : "string",
		"default" : "1,0",
		"displayName" : "CIP Path",
		"order" : "4"
	},
	"timeout" : {
		"description" : "Connection and operation timeout in milliseconds",
		"type" : "string",
		"default" : "5000",
		"displayName" : "Timeout (ms)",
		"order" : "5"
	},
	"pollInterval" : {
		"description" : "PLC poll loop interval in milliseconds",
		"type" : "string",
		"default" : "1000",
		"displayName" : "PLC Poll Loop Interval (ms)",
		"order" : "6"
	},
	"readmap" : {
		"description" : "JSON object with a 'tags' array of PLC tags to read. Each entry is an object {\"name\":\"PLC_tag\",\"type\":\"REAL\",\"asset\":\"Tank1\",\"datapoint\":\"Level\"}. 'name' and 'type' are required. Add 'program' (e.g. \"program\":\"MainProgram\") for a program-scoped tag; omit it for a controller-scoped tag. 'asset' and 'datapoint' are optional and, when omitted, fall back to the global asset name and the PLC tag address respectively.",
		"type" : "JSON",
		"default" : "{\"tags\":[{\"name\":\"Tank1_Level\",\"type\":\"REAL\",\"asset\":\"Tank1\",\"datapoint\":\"Level\"},{\"name\":\"Tank1_Temperature\",\"type\":\"REAL\",\"asset\":\"Tank1\",\"datapoint\":\"Temperature\"},{\"name\":\"Pump1_Running\",\"type\":\"BOOL\"},{\"name\":\"Counter1\",\"type\":\"DINT\"},{\"name\":\"Pressure1\",\"type\":\"REAL\"},{\"name\":\"FlowRate\",\"type\":\"REAL\"},{\"name\":\"MotorSpeed\",\"type\":\"INT\"},{\"name\":\"StuckSensor\",\"type\":\"REAL\"},{\"name\":\"Ramp1\",\"type\":\"REAL\"},{\"name\":\"RandomSensor\",\"type\":\"REAL\"}]}",
		"displayName" : "Read Tag Map",
		"order" : "7"
	},
	"writemap" : {
		"description" : "JSON object with a 'tags' array of PLC tags to write. Each entry is an object {\"name\":\"PLC_tag\",\"type\":\"REAL\"}. Both 'name' and 'type' are required. Add 'program' for a program-scoped tag; such a tag is written by its full address, Program:<program>.<name>.",
		"type" : "JSON",
		"default" : "{\"tags\":[]}",
		"displayName" : "Write Tag Map",
		"order" : "8"
	},
	"readingStructure" : {
		"description" : "How to structure readings",
		"type" : "enumeration",
		"default" : "Reading per asset",
		"options" : ["Reading per asset", "Reading per PLC tag"],
		"displayName" : "Reading Structure",
		"order" : "9"
	},
	"readMode" : {
		"description" : "Tag read strategy",
		"type" : "enumeration",
		"default" : "scatter-gather",
		"options" : ["scatter-gather", "sequential"],
		"displayName" : "Read Mode",
		"order" : "10"
	},
	"debugLevel" : {
		"description" : "Debug log verbosity: 0=minimal, 1=poll summaries, 2=per-tag values, 3=full trace",
		"type" : "enumeration",
		"default" : "1",
		"options" : ["0", "1", "2", "3"],
		"displayName" : "Debug Level",
		"order" : "11"
	},
	"outageLogInterval" : {
		"description" : "Seconds between repeat reports while PLC tags are unavailable. A powered-down cabinet is reported once when it drops, once per this interval while it stays down (with the elapsed duration), and once when it recovers. Set to 0 to log every occurrence.",
		"type" : "string",
		"default" : "3600",
		"displayName" : "Outage Log Interval (s)",
		"order" : "13"
	},
	"statsInterval" : {
		"description" : "Poll cycles between performance summary logs",
		"type" : "string",
		"default" : "100",
		"displayName" : "Stats Interval",
		"order" : "12"
	}
});

// PLUGIN_BUILD_ID is the plugin version from the VERSION file plus SemVer build
// metadata identifying this specific build (git commit, and a dirty marker when
// built from a modified checkout). Both it and VERSION come from the generated
// version.h - see mkversion.
static PLUGIN_INFORMATION info = {
	"ethernetip",              // name
	PLUGIN_BUILD_ID,           // version
	SP_ASYNC | SP_CONTROL,     // flags: async plugin with write support
	PLUGIN_TYPE_SOUTH,         // type
	"2.0.0",                   // interface version
	def_cfg                    // default configuration
};

// libplctag log callback (registered once via call_once)
static void log_callback_func(int32_t tag_id, int debug_level, const char *message)
{
	string msg = "LIBPLCTAG tag=" + to_string(tag_id) + ": " + string(message);
	switch (debug_level)
	{
		case 1: Logger::getLogger()->error("%s", msg.c_str()); break;
		case 2: Logger::getLogger()->warn("%s",  msg.c_str()); break;
		case 3: Logger::getLogger()->info("%s",  msg.c_str()); break;
		case 4: Logger::getLogger()->debug("%s", msg.c_str()); break;
		default: Logger::getLogger()->debug("%s", msg.c_str()); break;
	}
}

static once_flag s_loggerRegistered;
static once_flag s_buildLogged;

extern "C" {

// plugin_info() must not call into libcommon-lib (Logger, ConfigCategory, ...).
// Fledge enumerates plugins with the standalone get_plugin_info utility, which
// dlopen()s this library with RTLD_LAZY and does *not* link libcommon-lib - the
// plugin only gets those symbols from the south service that hosts it. A
// libcommon-lib call here therefore dies on an unresolved symbol at plugin
// discovery time, the utility prints nothing, and the plugin disappears from the
// UI's plugin list with "Failed to parse JSON data returned from the plugin
// information". Anything to be logged about the build belongs in plugin_init().
PLUGIN_INFORMATION *plugin_info()
{
	return &info;
}

PLUGIN_HANDLE plugin_init(ConfigCategory *config)
{
	// Log the build identity once per process so the exact binary in use can be
	// identified from the service log as well as from the Fledge UI.
	call_once(s_buildLogged, []() {
		Logger::getLogger()->info("[EtherIP] ethernetip %s (built %s)",
			PLUGIN_BUILD_ID, PLUGIN_BUILD_DATE);
	});

	call_once(s_loggerRegistered, []() {
		plc_tag_register_logger(log_callback_func);
	});

	PLCTag *handle = new PLCTag();
	handle->configure(config);

	Logger::getLogger()->info("[EtherIP] plugin_init: complete - plc=%s address=%s",
		config->itemExists("plc") ? config->getValue("plc").c_str() : "?",
		config->itemExists("address") ? config->getValue("address").c_str() : "?");

	return (PLUGIN_HANDLE)handle;
}

void plugin_start(PLUGIN_HANDLE handle)
{
	PLCTag *plctag = (PLCTag *)handle;
	if (plctag == nullptr)
		throw runtime_error("Bad plugin handle");

	plctag->start();
}

void plugin_register_ingest(PLUGIN_HANDLE handle, INGEST_CB2 cb, void *data)
{
	PLCTag *plctag = (PLCTag *)handle;
	if (plctag == nullptr)
		throw runtime_error("Bad plugin handle");

	plctag->registerIngest(cb, data);
}

// Async plugin - poll should never be called by the framework
std::vector<Reading *> *plugin_poll(PLUGIN_HANDLE handle)
{
	throw runtime_error("plugin_poll called on async plugin ethernetip - this is a framework error");
}

void plugin_reconfigure(PLUGIN_HANDLE *handle, const string &newConfig)
{
	PLCTag *plctag = (PLCTag *)*handle;
	if (plctag == nullptr)
		throw runtime_error("Bad plugin handle");

	lock_guard<mutex> lock(plctag->m_mutex);

	ConfigCategory category("reconfig", newConfig);
	plctag->configure(&category);
}

void plugin_shutdown(PLUGIN_HANDLE handle)
{
	PLCTag *plctag = (PLCTag *)handle;
	if (plctag == nullptr)
		throw runtime_error("Bad plugin handle");

	plctag->stop();
	delete plctag;
}

bool plugin_write(PLUGIN_HANDLE handle, const string &name, const string &value)
{
	PLCTag *plctag = (PLCTag *)handle;
	if (plctag == nullptr)
		return false;

	lock_guard<mutex> lock(plctag->m_mutex);
	return plctag->write(name, value);
}

}; // extern "C"
