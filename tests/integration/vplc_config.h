/*
 * Fledge EtherNet/IP south plugin tests.
 *
 * Copyright (c) 2026 Industrial Intelligence Group, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Industrial Intelligence Group
 */
#ifndef _VPLC_CONFIG_H
#define _VPLC_CONFIG_H

/**
 * Virtual PLC configuration for integration tests.
 *
 * Environment variables:
 *   VPLC_ADDRESS   - IP/hostname of the virtual PLC (default: "127.0.0.1")
 *   VPLC_PATH      - CIP path (default: "" - no path)
 *   VPLC_PLC_TYPE  - PLC type string (default: "controllogix")
 *   VPLC_TIMEOUT   - Timeout in ms (default: "5000")
 *   VPLC_READMAP   - JSON readmap override (default: built-in tag set)
 *   VPLC_WRITEMAP  - JSON writemap override (default: built-in tag set)
 *
 * Default tag set:
 *   The tags below represent a typical ControlLogix test configuration.
 *   Override VPLC_READMAP / VPLC_WRITEMAP to match your virtual PLC's
 *   actual tag configuration.
 */

#include <cstdlib>
#include <string>

namespace vplc {

// Read an env var or return a default
inline std::string env(const char *name, const char *fallback)
{
	const char *v = std::getenv(name);
	return (v && v[0]) ? std::string(v) : std::string(fallback);
}

// ---------------------------------------------------------------------------
// Connection parameters
// ---------------------------------------------------------------------------

inline std::string address()  { return env("VPLC_ADDRESS",  "127.0.0.1"); }
inline std::string path()     { return env("VPLC_PATH",     ""); }
inline std::string plcType()  { return env("VPLC_PLC_TYPE", "controllogix"); }
inline std::string timeout()  { return env("VPLC_TIMEOUT",  "5000"); }

// ---------------------------------------------------------------------------
// Default tag maps
//
// These defaults assume a virtual PLC pre-loaded with the following tags.
// Adjust to match your vPLC configuration, or override via VPLC_READMAP /
// VPLC_WRITEMAP environment variables.
//
// Read tags:
//   TestDINT      DINT     - a 32-bit signed integer
//   TestREAL      REAL     - a 32-bit float
//   TestBOOL      BOOL     - a single boolean
//   TestSTRING    STRING   - a PLC string
//   TestINT       INT      - a 16-bit signed integer
//   TestLREAL     LREAL    - a 64-bit float
//   TestSINT      SINT     - an 8-bit signed integer
//   TestLINT      LINT     - a 64-bit signed integer
//
// Write tags:
//   WriteDINT     DINT     - writable 32-bit integer
//   WriteREAL     REAL     - writable 32-bit float
//   WriteBOOL     BOOL     - writable boolean
//   WriteSTRING   STRING   - writable string
// ---------------------------------------------------------------------------

// Kept on one line each: ConfigCategory does not survive embedded newlines or
// tabs in a value, and the plugin then rejects the map with
// [CONFIG_ERR] failed to parse readmap JSON.
static const char *DEFAULT_READMAP =
	R"({"tags":[)"
	R"({"name":"TestDINT","type":"DINT"},)"
	R"({"name":"TestREAL","type":"REAL"},)"
	R"({"name":"TestBOOL","type":"BOOL"},)"
	R"({"name":"TestSTRING","type":"STRING"},)"
	R"({"name":"TestINT","type":"INT"},)"
	R"({"name":"TestLREAL","type":"LREAL"},)"
	R"({"name":"TestSINT","type":"SINT"},)"
	R"({"name":"TestLINT","type":"LINT"}]})";

static const char *DEFAULT_WRITEMAP =
	R"({"tags":[)"
	R"({"name":"WriteDINT","type":"DINT"},)"
	R"({"name":"WriteREAL","type":"REAL"},)"
	R"({"name":"WriteBOOL","type":"BOOL"},)"
	R"({"name":"WriteSTRING","type":"STRING"}]})";

inline std::string readmap()  { return env("VPLC_READMAP",  DEFAULT_READMAP); }
inline std::string writemap() { return env("VPLC_WRITEMAP", DEFAULT_WRITEMAP); }

// ---------------------------------------------------------------------------
// Tag name constants - used in test assertions
// ---------------------------------------------------------------------------

namespace tags {
	// Read tags
	constexpr const char *DINT    = "TestDINT";
	constexpr const char *REAL    = "TestREAL";
	constexpr const char *BOOL_   = "TestBOOL";    // trailing _ avoids keyword clash
	constexpr const char *STRING  = "TestSTRING";
	constexpr const char *INT     = "TestINT";
	constexpr const char *LREAL   = "TestLREAL";
	constexpr const char *SINT    = "TestSINT";
	constexpr const char *LINT    = "TestLINT";

	// Write tags
	constexpr const char *W_DINT   = "WriteDINT";
	constexpr const char *W_REAL   = "WriteREAL";
	constexpr const char *W_BOOL   = "WriteBOOL";
	constexpr const char *W_STRING = "WriteSTRING";
}

// ---------------------------------------------------------------------------
// Helper: build a ConfigCategory JSON string for PLCTag::configure()
// ---------------------------------------------------------------------------

inline std::string buildConfigJson(
	const std::string& readingStructure = "Reading per PLC tag",
	const std::string& readMode = "scatter-gather")
{
	// Escape the readmap/writemap JSON for embedding inside the config JSON.
	// ConfigCategory expects {"key": {"type":"string", "value":"..."}} format.
	// Since the readmap/writemap values contain JSON, they need to be escaped.
	auto escapeJson = [](const std::string& s) -> std::string {
		std::string out;
		out.reserve(s.size() + 32);
		for (char c : s) {
			switch (c) {
				case '"':  out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\t': out += "\\t"; break;
				default:   out += c; break;
			}
		}
		return out;
	};

	return std::string(R"({
		"address":  {"type":"string", "value":")")  + address() + R"("},
		"plc":      {"type":"string", "value":")"   + plcType() + R"("},
		"path":     {"type":"string", "value":")"   + path() + R"("},
		"timeout":  {"type":"string", "value":")"   + timeout() + R"("},
		"readmap":  {"type":"string", "value":")"   + escapeJson(readmap()) + R"("},
		"writemap": {"type":"string", "value":")"   + escapeJson(writemap()) + R"("},
		"asset":    {"type":"string", "value":"IntegrationTest"},
		"readingStructure": {"type":"string", "value":")" + readingStructure + R"("},
		"readMode": {"type":"string", "value":")"   + readMode + R"("},
		"debugLevel":    {"type":"string", "value":"2"},
		"statsInterval": {"type":"string", "value":"10"},
		"pollInterval":  {"type":"string", "value":"500"}
	})";
}

} // namespace vplc

#endif // _VPLC_CONFIG_H
