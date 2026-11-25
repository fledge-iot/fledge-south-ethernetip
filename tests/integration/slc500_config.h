/*
 * Fledge EtherNet/IP south plugin tests.
 *
 * Copyright (c) 2026 Industrial Intelligence Group, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Industrial Intelligence Group
 */
#ifndef _SLC500_CONFIG_H
#define _SLC500_CONFIG_H

/**
 * Configuration for the virtual SLC 500 integration tests.
 *
 * The simulator is ab_server from the libplctag source tree, driven by
 * tests/vplc/vplc.sh. Start it with:
 *
 *     ./tests/vplc/vplc.sh start
 *
 * Unlike the ControlLogix vPLC in vplc_config.h, this one starts with every
 * data file zeroed, so the tests write a known value and read it back rather
 * than asserting against pre-loaded contents.
 *
 * Environment variables:
 *   VPLC_SLC_ADDRESS  Simulator address (default: "127.0.0.1:44819")
 *   VPLC_SLC_TIMEOUT  Timeout in ms (default: "5000")
 *
 * The CIP path is deliberately absent and not configurable: an SLC 500 has no
 * backplane to route across, and libplctag rejects a path on a PCCC PLC unless
 * it is a DH+ bridge route. Leaving it blank here is part of what these tests
 * cover.
 */

#include <cstdlib>
#include <string>

namespace slc500 {

inline std::string env(const char *name, const char *fallback)
{
	const char *v = std::getenv(name);
	return (v && v[0]) ? std::string(v) : std::string(fallback);
}

inline std::string address() { return env("VPLC_SLC_ADDRESS", "127.0.0.1:44819"); }
inline std::string timeout() { return env("VPLC_SLC_TIMEOUT", "5000"); }

// ---------------------------------------------------------------------------
// Tag layout
//
// Each test owns its own data file element so cases cannot disturb one
// another. B3:0/1 and B3:0/2 are the exception - they share a word on purpose,
// to prove the PCCC masked write leaves the neighbouring bit alone.
// ---------------------------------------------------------------------------

namespace tags {
	constexpr const char *INT_    = "N7:0";     // 16-bit signed integer
	constexpr const char *REAL_   = "F8:0";     // 32-bit float
	constexpr const char *DINT_   = "L19:0";    // 32-bit signed integer (5/05 long file)
	constexpr const char *STRING_ = "ST18:0";   // 82-char string
	constexpr const char *BIT_A   = "B3:0/1";   // bit 1 of word 0
	constexpr const char *BIT_B   = "B3:0/2";   // bit 2 of the same word
}

// Every tag appears in both maps: the plugin keeps separate handles for reads
// and writes, so one PLCTag instance can write a value and poll it back.
// Kept on one line on purpose: ConfigCategory does not survive embedded
// newlines or tabs in a value, and the plugin then rejects the map with
// [CONFIG_ERR] failed to parse readmap JSON.
static const char *TAG_MAP =
	R"({"tags":[)"
	R"({"name":"N7:0","type":"INT"},)"
	R"({"name":"F8:0","type":"REAL"},)"
	R"({"name":"L19:0","type":"DINT"},)"
	R"({"name":"ST18:0","type":"STRING"},)"
	R"({"name":"B3:0/1","type":"BOOL"},)"
	R"({"name":"B3:0/2","type":"BOOL"}]})";

// ---------------------------------------------------------------------------
// Helper: build a ConfigCategory JSON string for PLCTag::configure()
// ---------------------------------------------------------------------------

inline std::string escapeJson(const std::string& s)
{
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
}

inline std::string buildConfigJson(
	const std::string& readingStructure = "Reading per PLC tag",
	const std::string& readMode = "scatter-gather")
{
	return std::string(R"({
		"address":  {"type":"string", "value":")")  + address() + R"("},
		"plc":      {"type":"string", "value":"slc500"},
		"path":     {"type":"string", "value":""},
		"timeout":  {"type":"string", "value":")"   + timeout() + R"("},
		"readmap":  {"type":"string", "value":")"   + escapeJson(TAG_MAP) + R"("},
		"writemap": {"type":"string", "value":")"   + escapeJson(TAG_MAP) + R"("},
		"asset":    {"type":"string", "value":"SLC500Test"},
		"readingStructure": {"type":"string", "value":")" + readingStructure + R"("},
		"readMode": {"type":"string", "value":")"   + readMode + R"("},
		"debugLevel":    {"type":"string", "value":"2"},
		"statsInterval": {"type":"string", "value":"10"},
		"pollInterval":  {"type":"string", "value":"500"}
	})";
}

} // namespace slc500

#endif // _SLC500_CONFIG_H
