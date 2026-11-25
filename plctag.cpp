/*
 * Fledge EtherNet/IP south plugin: libplctag tag access.
 *
 * Copyright (c) 2026 Industrial Intelligence Group, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Industrial Intelligence Group
 */
#include <algorithm>
#include <chrono>
#include <cstring>
#include <strings.h>
#include <sstream>
#include <stdexcept>
#include <sys/time.h>

#include <libplctag.h>
#include <rapidjson/document.h>

#include "plctag.h"

using namespace std;

// ---------------------------------------------------------------------------
// PollStats
// ---------------------------------------------------------------------------

void PollStats::logSummary(const string& ctx) const
{
	if (count == 0) return;
	Logger::getLogger()->info(
		"%s poll stats [last %llu polls]: "
		"avgTotal=%.1fms avgScatter=%.1fms "
		"avgGather=%.1fms min/maxGather=%.1f/%.1fms "
		"avgCollect=%.1fms successRate=%.1f%% timeouts=%llu",
		ctx.c_str(), (unsigned long long)count,
		totalMs   / count,
		scatterMs / count,
		gatherMs  / count,
		minGatherMs, maxGatherMs,
		collectMs / count,
		100.0 * tagsSucceeded / max(tagsAttempted, (uint64_t)1),
		(unsigned long long)pollTimeouts);
}

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

string PLCTag::ltrim(string s)
{
	s.erase(s.begin(), find_if(s.begin(), s.end(),
		[](unsigned char c) { return !isspace(c); }));
	return s;
}

string PLCTag::rtrim(string s)
{
	s.erase(find_if(s.rbegin(), s.rend(),
		[](unsigned char c) { return !isspace(c); }).base(), s.end());
	return s;
}

string PLCTag::trim(string s)
{
	return ltrim(rtrim(s));
}

// Logix program-scoped tags are addressed Program:<program>.<tag>. A map entry
// names its program in the optional 'program' field instead of spelling out
// that prefix; this folds the two into the address libplctag expects. An entry
// without 'program' is controller-scoped and its name is left untouched.
// Returns false, with the reason in err, if the entry must be skipped.
static bool applyProgramScope(const rapidjson::Value& entry, string& tagName, string& err)
{
	if (!entry.HasMember("program"))
		return true;

	const rapidjson::Value& program = entry["program"];
	if (!program.IsString() || program.GetStringLength() == 0)
	{
		err = "'program' must be a non-empty string";
		return false;
	}
	if (strncasecmp(tagName.c_str(), "Program:", 8) == 0)
	{
		err = "sets 'program' but its name already starts with 'Program:'";
		return false;
	}

	tagName = string("Program:") + program.GetString() + "." + tagName;
	return true;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

PLCTag::PLCTag()
{
	m_validTypes = {
		"BOOL",
		"USINT", "UINT8",
		"UINT",  "UINT16",
		"UDINT", "UINT32", "DWORD",
		"ULINT", "UINT64",
		"SINT",  "INT8",
		"INT",   "INT16",
		"DINT",  "INT32",
		"LINT",  "INT64",
		"REAL",  "FLOAT32",
		"LREAL", "FLOAT64",
		"STRING"
	};
}

PLCTag::~PLCTag()
{
	stop();

	if (m_creationThread.joinable())
		m_creationThread.join();

	TagState state = m_tagState.load();
	if (state == TagState::CREATION_FAILED || state == TagState::NEEDS_CREATION)
	{
		Logger::getLogger()->debug("%s No valid tags have been defined - nothing to destroy",
			logCtx().c_str());
	}

	int readDestroyed = 0, writeDestroyed = 0;
	for (int h : m_tagCreatedVector)
	{
		if (h >= 0)
		{
			int rc = plc_tag_destroy(h);
			if (rc != PLCTAG_STATUS_OK)
				Logger::getLogger()->error("%s failed to destroy read tag handle %d: %s",
					logCtx().c_str(), h, plc_tag_decode_error(rc));
			else
				readDestroyed++;
		}
	}
	for (auto& [tag, handle] : m_writeTagMap)
	{
		if (handle >= 0)
		{
			int rc = plc_tag_destroy(handle);
			if (rc != PLCTAG_STATUS_OK)
				Logger::getLogger()->error("%s failed to destroy write tag handle %d: %s",
					logCtx().c_str(), handle, plc_tag_decode_error(rc));
			else
				writeDestroyed++;
		}
	}

	Logger::getLogger()->info("%s plugin_shutdown: destroyed %d read tag handles, %d write tag handles",
		logCtx().c_str(), readDestroyed, writeDestroyed);
	Logger::getLogger()->info("%s plugin_shutdown: complete", logCtx().c_str());
}

// ---------------------------------------------------------------------------
// logCtx
// ---------------------------------------------------------------------------

string PLCTag::logCtx() const
{
	return "[EtherIP|" + m_assetName + "|" + m_address + "]";
}

// ---------------------------------------------------------------------------
// applyTimeout
// ---------------------------------------------------------------------------

void PLCTag::applyTimeout(const string& raw)
{
	if (raw.empty()) return;

	char* end = nullptr;
	unsigned long v = strtoul(raw.c_str(), &end, 10);
	if (end == raw.c_str() || *end != '\0')
	{
		Logger::getLogger()->warn("%s [CONFIG_ERR] timeout value '%s' is not a valid integer - keeping %u ms",
			logCtx().c_str(), raw.c_str(), m_timeout);
		return;
	}
	if (v == 0 || v > 120000)
	{
		Logger::getLogger()->warn("%s [CONFIG_ERR] timeout %lu ms is out of range [1, 120000] - keeping %u ms",
			logCtx().c_str(), v, m_timeout);
		return;
	}
	m_timeout = static_cast<uint32_t>(v);
}

// ---------------------------------------------------------------------------
// configure
// ---------------------------------------------------------------------------

void PLCTag::configure(ConfigCategory* config)
{
	// Step 1 - Change detection
	string newAddress = config->itemExists("address") ? config->getValue("address") : "";
	string newPlc     = config->itemExists("plc")     ? config->getValue("plc")     : "";
	string newPath    = config->itemExists("path")    ? config->getValue("path")    : "";
	string newReadMap = config->itemExists("readmap") ? config->getValue("readmap") : "";
	string newWriteMap= config->itemExists("writemap")? config->getValue("writemap"): "";

	// 'path' is the one field where an empty string is a meaningful value.
	// PCCC controllers (SLC 500, MicroLogix) are addressed with no CIP path at
	// all - libplctag rejects one unless it routes over a DH+ bridge - so
	// clearing the field has to take effect. Presence in the category, not
	// emptiness, decides whether the operator supplied a value.
	bool pathPresent = config->itemExists("path");

	bool addressChanged  = (newAddress  != m_address);
	bool plcChanged      = (newPlc      != m_plc);
	bool pathChanged     = (newPath     != m_path);
	bool readMapChanged  = (newReadMap  != m_readMap);
	bool writeMapChanged = (newWriteMap != m_writeMap);

	Logger::getLogger()->info(
		"%s configure: change flags address=%d plc=%d path=%d readMap=%d writeMap=%d",
		logCtx().c_str(),
		addressChanged, plcChanged, pathChanged, readMapChanged, writeMapChanged);

	// Step 2 - Cleanup decision
	bool needsCleanup = false;
	if ((addressChanged  && !newAddress.empty())  ||
	    (plcChanged      && !newPlc.empty())       ||
	    (pathChanged     && pathPresent)            ||
	    (readMapChanged  && !newReadMap.empty())    ||
	    (writeMapChanged && !newWriteMap.empty()))
	{
		needsCleanup = true;
	}

	if (needsCleanup)
	{
		Logger::getLogger()->info("%s configure: cleanup required - tags will be recreated",
			logCtx().c_str());
		doCleanUp();
	}
	else
	{
		PLCTAG_DEBUG(1, "%s configure: no structural change - skipping cleanup",
			logCtx().c_str());
	}

	// Step 3 - Apply new values
	m_readMap  = newReadMap;
	m_writeMap = newWriteMap;

	if (!newAddress.empty()) m_address = newAddress;
	if (!newPlc.empty())     m_plc = newPlc;
	if (pathPresent)
	{
		m_path = newPath;
		m_path.erase(remove(m_path.begin(), m_path.end(), ' '), m_path.end());
	}

	// Build tag string prefix
	if (m_plc == "micro800")
	{
		m_tagString = "protocol=ab-eip&gateway=" + m_address
			+ "&plc=Micro800&str_count_word_bytes=1&str_is_counted=1&name=";
	}
	else
	{
		// An empty path is omitted rather than sent as a bare "path=".
		// ControlLogix needs one; SLC 500 and MicroLogix must not have one
		// unless it is a DH+ bridge route.
		m_tagString = "protocol=ab-eip&gateway=" + m_address;
		if (!m_path.empty())
			m_tagString += "&path=" + m_path;
		m_tagString += "&plc=" + m_plc + "&name=";
	}
	PLCTAG_DEBUG(3, "%s configure: tagString=%s", logCtx().c_str(), m_tagString.c_str());

	if (config->itemExists("asset"))
		m_assetName = config->getValue("asset");

	if (config->itemExists("timeout"))
	{
		string val = config->getValue("timeout");
		if (!val.empty())
			applyTimeout(val);
	}

	if (config->itemExists("readingStructure"))
	{
		string val = config->getValue("readingStructure");
		if (val == "Reading per PLC tag")
			m_readingStructure = 0;
		else if (val == "Reading per asset")
			m_readingStructure = 1;
	}

	if (config->itemExists("readMode"))
	{
		string val = config->getValue("readMode");
		m_readMode = (val == "sequential") ? ReadMode::SEQUENTIAL : ReadMode::SCATTER_GATHER;
	}

	if (config->itemExists("debugLevel"))
	{
		string val = config->getValue("debugLevel");
		if (!val.empty())
		{
			int lvl = atoi(val.c_str());
			if (lvl >= 0 && lvl <= 3) m_debugLevel = lvl;
		}
	}

	if (config->itemExists("statsInterval"))
	{
		string val = config->getValue("statsInterval");
		if (!val.empty())
		{
			int iv = atoi(val.c_str());
			if (iv > 0) m_statsInterval = iv;
		}
	}

	if (config->itemExists("outageLogInterval"))
	{
		string val = config->getValue("outageLogInterval");
		if (!val.empty())
		{
			long iv = atol(val.c_str());
			if (iv >= 0 && iv <= 86400)
				m_outageLogInterval = (uint32_t)iv;
			else
				Logger::getLogger()->warn(
					"%s [CONFIG_ERR] outageLogInterval '%s' is out of range [0, 86400] - keeping %u s",
					logCtx().c_str(), val.c_str(), m_outageLogInterval);
		}
	}

	if (config->itemExists("pollInterval"))
	{
		string val = config->getValue("pollInterval");
		if (!val.empty())
		{
			int iv = atoi(val.c_str());
			if (iv > 0) m_pollInterval = (uint32_t)iv;
		}
	}

	// The tag maps are only re-parsed when doCleanUp() has just emptied the
	// vectors. Without this guard a reconfigure that changes nothing
	// structural - a new asset name, a different readingStructure, a
	// debugLevel bump - re-parses the unchanged maps and appends a second copy
	// of every read tag, so the service starts emitting each datapoint twice.
	if (!needsCleanup && !(m_tagVector.empty() && m_writeTagMap.empty()))
	{
		PLCTAG_DEBUG(1, "%s configure: maps unchanged - keeping %zu read / %zu write tags",
			logCtx().c_str(), m_tagVector.size(), m_writeTagMap.size());
		return;
	}

	// Step 4 - Parse readmap JSON
	// A JSON object with a 'tags' array, each element describing one PLC tag:
	//   { "tags": [ { "name": "PLC_tag", "type": "TYPE", "program": "...",
	//                 "asset": "...", "datapoint": "..." } ] }
	// 'name' and 'type' are required; 'program' is optional and makes the tag
	// program-scoped; 'asset' and 'datapoint' are optional and fall back to
	// the global asset and the PLC tag address respectively.
	if (!m_readMap.empty())
	{
		rapidjson::Document doc;
		doc.Parse(m_readMap.c_str());
		if (!doc.HasParseError() && doc.IsObject() &&
		    doc.HasMember("tags") && doc["tags"].IsArray())
		{
			for (auto& entry : doc["tags"].GetArray())
			{
				if (!entry.IsObject())
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: readmap entry is not an object - skipping",
						logCtx().c_str());
					continue;
				}

				if (!entry.HasMember("name") || !entry["name"].IsString())
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: readmap entry is missing string 'name' - skipping",
						logCtx().c_str());
					continue;
				}
				string tagName = entry["name"].GetString();

				string scopeErr;
				if (!applyProgramScope(entry, tagName, scopeErr))
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: tag '%s' %s - skipping",
						logCtx().c_str(), tagName.c_str(), scopeErr.c_str());
					continue;
				}

				if (!entry.HasMember("type") || !entry["type"].IsString())
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: tag '%s' is missing string 'type' - skipping",
						logCtx().c_str(), tagName.c_str());
					continue;
				}
				string typeName = entry["type"].GetString();

				string assetOverride;
				string datapointOverride;
				if (entry.HasMember("asset") && entry["asset"].IsString())
					assetOverride = entry["asset"].GetString();
				if (entry.HasMember("datapoint") && entry["datapoint"].IsString())
					datapointOverride = entry["datapoint"].GetString();

				if (m_validTypes.count(typeName) == 0)
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: tag '%s' has unknown type '%s' - skipping",
						logCtx().c_str(), tagName.c_str(), typeName.c_str());
					continue;
				}
				m_tagVector.push_back({tagName, typeName, assetOverride, datapointOverride});
				m_tagCreatedVector.push_back(0);
				m_tagFailCount.push_back(0);
				m_tagOutage.emplace_back();
			}
		}
		else if (!m_readMap.empty())
		{
			Logger::getLogger()->error("%s [CONFIG_ERR] failed to parse readmap JSON (expected an object with a 'tags' array)",
				logCtx().c_str());
		}
	}

	// Step 5 - Parse writemap JSON
	// A JSON object with a 'tags' array:
	//   { "tags": [ { "name": "PLC_tag", "type": "TYPE", "program": "..." } ] }
	// Both 'name' and 'type' are required; 'program' is optional as for reads.
	// Writes are addressed by the full tag address, Program: prefix included.
	if (!m_writeMap.empty())
	{
		rapidjson::Document doc;
		doc.Parse(m_writeMap.c_str());
		if (!doc.HasParseError() && doc.IsObject() &&
		    doc.HasMember("tags") && doc["tags"].IsArray())
		{
			for (auto& entry : doc["tags"].GetArray())
			{
				if (!entry.IsObject())
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: writemap entry is not an object - skipping",
						logCtx().c_str());
					continue;
				}

				if (!entry.HasMember("name") || !entry["name"].IsString())
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: writemap entry is missing string 'name' - skipping",
						logCtx().c_str());
					continue;
				}
				string tagName = entry["name"].GetString();

				string scopeErr;
				if (!applyProgramScope(entry, tagName, scopeErr))
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: write tag '%s' %s - skipping",
						logCtx().c_str(), tagName.c_str(), scopeErr.c_str());
					continue;
				}

				if (!entry.HasMember("type") || !entry["type"].IsString())
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: write tag '%s' is missing string 'type' - skipping",
						logCtx().c_str(), tagName.c_str());
					continue;
				}
				string typeName = entry["type"].GetString();

				if (m_validTypes.count(typeName) == 0)
				{
					Logger::getLogger()->warn(
						"%s [CONFIG_ERR] configure: write tag '%s' has unknown type '%s' - skipping",
						logCtx().c_str(), tagName.c_str(), typeName.c_str());
					continue;
				}
				Tag t{tagName, typeName};
				m_writeTagMap[t] = -1;
			}
		}
		else if (!m_writeMap.empty())
		{
			Logger::getLogger()->error("%s [CONFIG_ERR] failed to parse writemap JSON (expected an object with a 'tags' array)",
				logCtx().c_str());
		}
	}

	// Step 6 - Set state
	m_tagState = TagState::NEEDS_CREATION;
	m_startupLogged = false;

	// Debug dump of parsed tags
	PLCTAG_DEBUG(1, "%s configure: readMap parsed - %zu tags:", logCtx().c_str(), m_tagVector.size());
	for (size_t i = 0; i < m_tagVector.size(); i++)
	{
		const auto& t = m_tagVector[i];
		const string& effAsset = t.assetOverride.empty() ? m_assetName : t.assetOverride;
		const string& effDp    = t.datapointOverride.empty() ? t.name : t.datapointOverride;
		PLCTAG_DEBUG(1, "%s   read[%2zu] '%s'  %s  -> %s.%s",
			logCtx().c_str(), i, t.name.c_str(), t.typeName.c_str(),
			effAsset.c_str(), effDp.c_str());
	}
	PLCTAG_DEBUG(1, "%s configure: writeMap parsed - %zu tags:", logCtx().c_str(), m_writeTagMap.size());
	{
		int idx = 0;
		for (auto& [tag, handle] : m_writeTagMap)
		{
			PLCTAG_DEBUG(1, "%s   write[%d] '%s'  %s",
				logCtx().c_str(), idx++, tag.name.c_str(), tag.typeName.c_str());
		}
	}

	Logger::getLogger()->info("%s configure: applied - readTags=%zu writeTags=%zu",
		logCtx().c_str(), m_tagVector.size(), m_writeTagMap.size());
}

// ---------------------------------------------------------------------------
// doCleanUp
// ---------------------------------------------------------------------------

void PLCTag::doCleanUp()
{
	// Signal any in-progress background creation to bail out promptly so the
	// join below cannot block for the full m_timeout (or longer if libplctag
	// is wedged on a stuck connect). doCleanUp is called with m_mutex held by
	// plugin_reconfigure, so a slow join here would also stall the poll thread.
	m_stopCreation = true;
	if (m_creationThread.joinable())
		m_creationThread.join();
	m_stopCreation = false;

	for (int h : m_tagCreatedVector)
	{
		if (h >= 0)
			plc_tag_destroy(h);
	}
	for (auto& [tag, handle] : m_writeTagMap)
	{
		if (handle >= 0)
			plc_tag_destroy(handle);
	}

	m_tagVector.clear();
	m_tagCreatedVector.clear();
	m_tagFailCount.clear();
	m_tagOutage.clear();
	m_writeTagMap.clear();
	m_tagState = TagState::NEEDS_CREATION;
	m_creationInProgress = false;
	m_debounceCounter = 0;
	m_pollCount = 0;
	m_stats.reset();
	m_startupLogged = false;
}

// ---------------------------------------------------------------------------
// openTag
// ---------------------------------------------------------------------------

int PLCTag::openTag(const string& tagName, const string& typeName, uint32_t timeout)
{
	string finalName = tagName;

	// BOOL array rewrite for ControlLogix
	if (m_plc == "controllogix" && typeName == "BOOL")
	{
		auto lb = tagName.find('[');
		auto rb = tagName.find(']');
		if (lb != string::npos && rb != string::npos && lb < rb)
		{
			string arrName = tagName.substr(0, lb);
			int idx = stoi(tagName.substr(lb + 1, rb - lb - 1));
			if (idx < 0)
			{
				Logger::getLogger()->error(
					"%s [CONFIG_ERR] BOOL array index %d is negative for tag '%s' - skipping",
					logCtx().c_str(), idx, tagName.c_str());
				return -1;
			}
			int word = idx / 32;
			int bit  = idx % 32;
			finalName = arrName + "[" + to_string(word) + "]." + to_string(bit);
			PLCTAG_DEBUG(3, "%s openTag: BOOL array rewrite '%s' -> '%s'",
				logCtx().c_str(), tagName.c_str(), finalName.c_str());
		}
	}

	string tagStr = m_tagString + finalName;

	if (tagStr.size() > 500)
	{
		Logger::getLogger()->error(
			"%s [CONFIG_ERR] Tag string too long (%zu chars) for tag '%s' - skipping",
			logCtx().c_str(), tagStr.size(), tagName.c_str());
		return -1;
	}

	PLCTAG_DEBUG(3, "%s openTag: tag='%s' type='%s' tagString='%s'",
		logCtx().c_str(), tagName.c_str(), typeName.c_str(), tagStr.c_str());

	int handle = plc_tag_create(tagStr.c_str(), (int)timeout);
	if (handle < 0)
	{
		Logger::getLogger()->error(
			"%s [CONN_ERR] plc_tag_create failed for tag '%s' (type %s): %s",
			logCtx().c_str(), tagName.c_str(), typeName.c_str(), plc_tag_decode_error(handle));
	}
	else
	{
		PLCTAG_DEBUG(1, "%s openTag: tag='%s' handle=%d status=PENDING",
			logCtx().c_str(), tagName.c_str(), handle);
	}
	return handle;
}

// ---------------------------------------------------------------------------
// Tag type verification
//
// libplctag exposes the data type the controller reports for a tag through the
// "raw_tag_type_bytes" attribute: a little-endian CIP type code, available as
// soon as the tag has been created. Comparing it against the type declared in
// the readmap catches the silent failure mode where a tag is read with the
// wrong type - libplctag reinterprets the bytes without error, so a REAL
// declared for an INT tag simply produces wrong values with nothing logged.
//
// The attribute is only implemented for Logix and Micro800 tags; MicroLogix
// (PCCC) and Omron NJ/NX return no bytes, and those tags are skipped.
// ---------------------------------------------------------------------------

// CIP elementary data types (CIP Vol 1, Appendix C-6.1)
const char* PLCTag::cipTypeName(int code)
{
	switch (code)
	{
		case 0x00C1: return "BOOL";
		case 0x00C2: return "SINT";
		case 0x00C3: return "INT";
		case 0x00C4: return "DINT";
		case 0x00C5: return "LINT";
		case 0x00C6: return "USINT";
		case 0x00C7: return "UINT";
		case 0x00C8: return "UDINT";
		case 0x00C9: return "ULINT";
		case 0x00CA: return "REAL";
		case 0x00CB: return "LREAL";
		case 0x00CE: return "STRING";
		case 0x00D1: return "BYTE";
		case 0x00D2: return "WORD";
		case 0x00D3: return "DWORD";
		case 0x00D4: return "LWORD";
		case 0x02A0: return "STRUCT/UDT";
		default:     return "unknown";
	}
}

// Is this a numeric scalar the plugin could plausibly have been pointed at?
static bool isNumericCipType(int code)
{
	switch (code)
	{
		case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
		case 0x00C6: case 0x00C7: case 0x00C8: case 0x00C9: case 0x00CA:
		case 0x00CB: case 0x00D1: case 0x00D2: case 0x00D3: case 0x00D4:
			return true;
		default:
			return false;
	}
}

int PLCTag::cipTypeCompatible(const string& declared, int code, bool bitAddressed)
{
	// A bit inside a word - "Tag.7", or a ControlLogix BOOL array element, which
	// openTag rewrites to "array[word].bit". The controller reports the type of
	// the containing word, so any integer or bit-string type is correct here.
	if (declared == "BOOL")
	{
		if (code == 0x00C1) return 1;
		if (bitAddressed)
		{
			switch (code)
			{
				case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
				case 0x00C6: case 0x00C7: case 0x00C8: case 0x00C9:
				case 0x00D1: case 0x00D2: case 0x00D3: case 0x00D4:
					return 1;
			}
		}
		return 0;
	}

	// Strings are encoded differently across PLC families - a structure on
	// Logix, a counted string on Micro800, 0x00CE elsewhere. Only flag a STRING
	// that has been pointed at a numeric scalar; anything else is unverifiable.
	if (declared == "STRING")
		return isNumericCipType(code) ? 0 : -1;

	// Each declared type accepts its own CIP code plus the same-width bit-string
	// type (BYTE/WORD/DWORD/LWORD), which carries identical data.
	struct TypeRule { const char* name; int exact; int bitString; };
	static const TypeRule rules[] = {
		{ "SINT",    0x00C2, 0x00D1 }, { "INT8",    0x00C2, 0x00D1 },
		{ "USINT",   0x00C6, 0x00D1 }, { "UINT8",   0x00C6, 0x00D1 },
		{ "INT",     0x00C3, 0x00D2 }, { "INT16",   0x00C3, 0x00D2 },
		{ "UINT",    0x00C7, 0x00D2 }, { "UINT16",  0x00C7, 0x00D2 },
		{ "DINT",    0x00C4, 0x00D3 }, { "INT32",   0x00C4, 0x00D3 },
		{ "UDINT",   0x00C8, 0x00D3 }, { "UINT32",  0x00C8, 0x00D3 },
		{ "DWORD",   0x00D3, 0x00C8 },
		{ "LINT",    0x00C5, 0x00D4 }, { "INT64",   0x00C5, 0x00D4 },
		{ "ULINT",   0x00C9, 0x00D4 }, { "UINT64",  0x00C9, 0x00D4 },
		{ "REAL",    0x00CA, 0x00CA }, { "FLOAT32", 0x00CA, 0x00CA },
		{ "LREAL",   0x00CB, 0x00CB }, { "FLOAT64", 0x00CB, 0x00CB },
	};

	for (const auto& r : rules)
	{
		if (declared == r.name)
			return (code == r.exact || code == r.bitString) ? 1 : 0;
	}

	return -1;  // type we do not have a rule for - say nothing
}

// A tag addresses a single bit if the name carries a ".<digits>" suffix, or if
// it is a ControlLogix BOOL array element, which openTag rewrites into that
// form before creating the tag.
bool PLCTag::isBitAddressed(const ReadTag& tag) const
{
	const string& n = tag.name;

	auto dot = n.rfind('.');
	if (dot != string::npos && dot + 1 < n.size())
	{
		bool allDigits = true;
		for (size_t i = dot + 1; i < n.size(); i++)
			if (!isdigit((unsigned char)n[i])) { allDigits = false; break; }
		if (allDigits) return true;
	}

	if (m_plc == "controllogix" && tag.typeName == "BOOL" &&
	    n.find('[') != string::npos && n.find(']') != string::npos)
		return true;

	return false;
}

void PLCTag::verifyTagTypes()
{
	int checked = 0, mismatched = 0, unverifiable = 0;

	for (size_t i = 0; i < m_tagVector.size(); i++)
	{
		int h = m_tagCreatedVector[i];
		if (h < 1) continue;

		uint8_t buf[16];
		memset(buf, 0, sizeof(buf));
		int n = plc_tag_get_byte_array_attribute(h, "raw_tag_type_bytes",
		                                         buf, (int)sizeof(buf));
		if (n < 2)
		{
			// No type information: PLC family does not support the attribute.
			unverifiable++;
			PLCTAG_DEBUG(2, "%s typeCheck: tag='%s' - PLC reported no type info (rc=%d), skipped",
				logCtx().c_str(), m_tagVector[i].name.c_str(), n);
			continue;
		}

		int code = (int)buf[0] | ((int)buf[1] << 8);
		int verdict = cipTypeCompatible(m_tagVector[i].typeName, code,
		                                isBitAddressed(m_tagVector[i]));

		if (verdict == 0)
		{
			mismatched++;
			Logger::getLogger()->warn(
				"%s [TYPE_MISMATCH] tag='%s' is declared as %s but the PLC reports %s "
				"(CIP type 0x%04X) - values for this tag will be misinterpreted",
				logCtx().c_str(), m_tagVector[i].name.c_str(),
				m_tagVector[i].typeName.c_str(), cipTypeName(code), code);
		}
		else if (verdict > 0)
		{
			checked++;
			PLCTAG_DEBUG(2, "%s typeCheck: tag='%s' declared %s, PLC reports %s (0x%04X) - OK",
				logCtx().c_str(), m_tagVector[i].name.c_str(),
				m_tagVector[i].typeName.c_str(), cipTypeName(code), code);
		}
		else
		{
			unverifiable++;
			PLCTAG_DEBUG(2, "%s typeCheck: tag='%s' declared %s, PLC reports 0x%04X (%s) - cannot verify",
				logCtx().c_str(), m_tagVector[i].name.c_str(),
				m_tagVector[i].typeName.c_str(), code, cipTypeName(code));
		}
	}

	if (mismatched > 0)
	{
		Logger::getLogger()->warn(
			"%s typeCheck: %d tag(s) have a declared type that does not match the PLC - "
			"fix the 'type' field in the readmap; readings for those tags are not trustworthy",
			logCtx().c_str(), mismatched);
	}

	PLCTAG_DEBUG(1, "%s typeCheck: %d verified, %d mismatched, %d unverifiable",
		logCtx().c_str(), checked, mismatched, unverifiable);
}

// ---------------------------------------------------------------------------
// createTagsBackground
// ---------------------------------------------------------------------------

void PLCTag::createTagsBackground()
{
	auto startTime = chrono::steady_clock::now();

	// Failures are collected rather than logged as they happen: whether a
	// given failure is worth a line depends on how many others failed with
	// it, which is not known until every tag has been tried.
	vector<size_t> failedIdx;
	string lastCreateError = "no response from PLC";

	// While an outage is running this line would repeat every retry cycle.
	if (m_createOutage.active)
	{
		PLCTAG_DEBUG(1, "%s createTags: retrying creation of %zu read tags, %zu write tags",
			logCtx().c_str(), m_tagVector.size(), m_writeTagMap.size());
	}
	else
	{
		Logger::getLogger()->info("%s createTags: starting background creation of %zu read tags, %zu write tags",
			logCtx().c_str(), m_tagVector.size(), m_writeTagMap.size());
	}

	// Phase 1: issue plc_tag_create for all read tags (async)
	m_tagFailCount.assign(m_tagVector.size(), 0);
	for (size_t i = 0; i < m_tagVector.size(); i++)
	{
		if (m_tagCreatedVector[i] < 1)
		{
			int h = openTag(m_tagVector[i].name, m_tagVector[i].typeName, 0);
			m_tagCreatedVector[i] = (h < 0) ? -1 : h;
		}
	}

	// Phase 2: poll status up to m_timeout ms with early failure detection
	auto deadline = chrono::steady_clock::now() + chrono::milliseconds(m_timeout);
	bool allReady = false;
	auto lastProgressLog = chrono::steady_clock::now();

	while (chrono::steady_clock::now() < deadline)
	{
		if (m_stopCreation.load())
		{
			Logger::getLogger()->info("%s createTags: cancelled by cleanup - aborting pending tags",
				logCtx().c_str());
			for (size_t i = 0; i < m_tagCreatedVector.size(); i++)
			{
				int h = m_tagCreatedVector[i];
				if (h >= 1 && plc_tag_status(h) == PLCTAG_STATUS_PENDING)
					plc_tag_abort(h);
			}
			m_creationInProgress = false;
			return;
		}

		allReady = true;
		int readyCount = 0;
		for (size_t i = 0; i < m_tagCreatedVector.size(); i++)
		{
			int h = m_tagCreatedVector[i];
			if (h < 1) continue;  // already failed or not created

			int s = plc_tag_status(h);
			if (s == PLCTAG_STATUS_PENDING)
			{
				allReady = false;
			}
			else if (s == PLCTAG_STATUS_OK)
			{
				readyCount++;
			}
			else
			{
				// Hard error - stop waiting for this tag and release the handle
				// so the tag object does not linger for the life of the plugin.
				PLCTAG_DEBUG(1, "%s createTags: tag='%s' handle=%d -> ERROR %d (%s)",
					logCtx().c_str(), m_tagVector[i].name.c_str(), h, s,
					plc_tag_decode_error(s));
				lastCreateError = plc_tag_decode_error(s);
				failedIdx.push_back(i);
				plc_tag_destroy(h);
				m_tagCreatedVector[i] = -1;
			}
		}
		if (allReady) break;

		// Log progress every 500ms
		auto now = chrono::steady_clock::now();
		auto elapsedMs = chrono::duration_cast<chrono::milliseconds>(now - startTime).count();
		if (chrono::duration_cast<chrono::milliseconds>(now - lastProgressLog).count() >= 500)
		{
			PLCTAG_DEBUG(1, "%s createTags: waiting - %d/%zu tags ready, elapsed=%ldms, deadline=%ums",
				logCtx().c_str(), readyCount, m_tagVector.size(), (long)elapsedMs, m_timeout);
			lastProgressLog = now;
		}

		this_thread::sleep_for(chrono::milliseconds(10));
	}

	auto createElapsed = chrono::duration_cast<chrono::milliseconds>(
		chrono::steady_clock::now() - startTime).count();

	// Handle timeout
	if (!allReady)
	{
		for (size_t i = 0; i < m_tagCreatedVector.size(); i++)
		{
			int h = m_tagCreatedVector[i];
			if (h >= 1 && plc_tag_status(h) == PLCTAG_STATUS_PENDING)
			{
				PLCTAG_DEBUG(1, "%s createTags: tag='%s' still PENDING at timeout - aborting",
					logCtx().c_str(), m_tagVector[i].name.c_str());
				lastCreateError = "still PENDING at create timeout";
				failedIdx.push_back(i);
				plc_tag_abort(h);
				plc_tag_destroy(h);
				m_tagCreatedVector[i] = -1;
			}
		}
	}

	// Phase 3: create write tags
	int writeReady = 0;
	for (auto& [tag, handle] : m_writeTagMap)
	{
		if (handle < 1)
		{
			handle = openTag(tag.name, tag.typeName, (int)m_timeout);
			if (handle >= 1) writeReady++;
		}
		else
		{
			writeReady++;
		}
	}

	// Determine overall state
	int readReady = 0;
	for (int h : m_tagCreatedVector)
		if (h > 0) readReady++;

	TagState newState;
	if (readReady == 0 && m_tagVector.size() > 0)
		newState = TagState::CREATION_FAILED;
	else if (readReady < (int)m_tagVector.size())
		newState = TagState::PARTIAL;
	else
		newState = TagState::READY;

	m_tagState = newState;

	const char* stateStr = "READY";
	if (newState == TagState::CREATION_FAILED) stateStr = "FAILED";
	else if (newState == TagState::PARTIAL) stateStr = "PARTIAL";

	// Nothing came up: one aggregate line, rate-limited. Every tag has the
	// same story in this case, so enumerating them adds no information.
	if (newState == TagState::CREATION_FAILED)
	{
		reportOutage(m_createOutage, "read tags could not be created",
			(int)m_tagVector.size(), (int)m_tagVector.size(), lastCreateError);
	}
	else
	{
		clearOutage(m_createOutage, "read tag creation", readReady, (int)m_tagVector.size());
		m_retryBackoffMs = RETRY_BACKOFF_MIN_MS;

		// A subset failing is the interesting case - a bad tag name rather
		// than a dead cabinet - so those are still named individually.
		for (size_t idx : failedIdx)
		{
			Logger::getLogger()->warn(
				"%s [CONFIG_ERR] createTags: tag='%s' could not be created (%s) - "
				"the other %d tag(s) came up, so check this tag's name and type",
				logCtx().c_str(), m_tagVector[idx].name.c_str(),
				lastCreateError.c_str(), readReady);
		}
	}

	// The completion line repeats every retry while an outage is running.
	if (m_createOutage.active)
	{
		PLCTAG_DEBUG(1,
			"%s createTags: complete in %ldms - readReady=%d/%zu writeReady=%d/%zu state=%s",
			logCtx().c_str(), (long)createElapsed,
			readReady, m_tagVector.size(),
			writeReady, m_writeTagMap.size(),
			stateStr);
	}
	else
	{
		Logger::getLogger()->info(
			"%s createTags: complete in %ldms - readReady=%d/%zu writeReady=%d/%zu state=%s",
			logCtx().c_str(), (long)createElapsed,
			readReady, m_tagVector.size(),
			writeReady, m_writeTagMap.size(),
			stateStr);
	}

	// Compare each created tag's declared type against what the PLC reports.
	// Warn only - a mismatch does not stop the tag being read, since the user
	// may have deliberately reinterpreted it.
	if (readReady > 0)
		verifyTagTypes();

	// Startup diagnostics block
	if ((newState == TagState::READY || newState == TagState::PARTIAL) && !m_startupLogged)
	{
		m_startupLogged = true;
		const char* modeStr = (m_readMode == ReadMode::SEQUENTIAL) ? "sequential" : "scatter-gather";
		Logger::getLogger()->info("%s ── PLUGIN CONNECTED ──────────────────────────────", logCtx().c_str());
		Logger::getLogger()->info("%s   PLC type     : %s", logCtx().c_str(), m_plc.c_str());
		Logger::getLogger()->info("%s   Address      : %s", logCtx().c_str(), m_address.c_str());
		Logger::getLogger()->info("%s   CIP path     : %s", logCtx().c_str(),
			(m_plc == "micro800" || m_path.empty()) ? "none" : m_path.c_str());
		Logger::getLogger()->info("%s   Asset name   : %s", logCtx().c_str(), m_assetName.c_str());
		Logger::getLogger()->info("%s   Timeout      : %u ms", logCtx().c_str(), m_timeout);
		Logger::getLogger()->info("%s   Read mode    : %s", logCtx().c_str(), modeStr);
		Logger::getLogger()->info("%s   Read tags    : %zu configured, %d created OK",
			logCtx().c_str(), m_tagVector.size(), readReady);
		Logger::getLogger()->info("%s   Write tags   : %zu configured, %d created OK",
			logCtx().c_str(), m_writeTagMap.size(), writeReady);
		Logger::getLogger()->info("%s   Create time  : %ld ms", logCtx().c_str(), (long)createElapsed);
		Logger::getLogger()->info("%s ──────────────────────────────────────────────────", logCtx().c_str());
	}

	m_creationInProgress = false;
}

// ---------------------------------------------------------------------------
// takeReading
// ---------------------------------------------------------------------------

vector<Reading *> PLCTag::takeReading()
{
	m_pollCount++;
	TagState state = m_tagState.load();

	PLCTAG_DEBUG(1, "%s poll#%llu: start - state=%s readTags=%zu mode=%s",
		logCtx().c_str(), (unsigned long long)m_pollCount,
		state == TagState::READY ? "READY" :
		state == TagState::PARTIAL ? "PARTIAL" :
		state == TagState::NEEDS_CREATION ? "NEEDS_CREATION" : "CREATION_FAILED",
		m_tagVector.size(),
		m_readMode == ReadMode::SEQUENTIAL ? "sequential" : "scatter-gather");

	// Launch background creation if needed
	if (state == TagState::NEEDS_CREATION && !m_creationInProgress.load())
	{
		m_creationInProgress = true;
		if (m_creationThread.joinable())
			m_creationThread.join();
		m_creationThread = thread([this]() { createTagsBackground(); });
		return {};
	}

	if (m_creationInProgress.load())
		return {};

	if (state == TagState::CREATION_FAILED)
	{
		// Once an outage is declared, the [OUTAGE] line is the single source
		// of truth; this message would only restate it every 300 polls.
		if (m_debounceCounter == 0 && !m_createOutage.active)
		{
			Logger::getLogger()->error(
				"%s [CONN_ERR] Some PLC tags may be invalid or connection to '%s' PLC at '%s' path '%s' failed.",
				logCtx().c_str(), m_plc.c_str(), m_address.c_str(), m_path.c_str());
		}
		m_debounceCounter = (m_debounceCounter + 1) % 300;

		// Back off between attempts. Hammering a dead cabinet every cycle
		// costs a connect attempt plus a full m_timeout wait each time and
		// buys nothing; the ceiling keeps recovery detection prompt.
		auto now = chrono::steady_clock::now();
		if (now < m_nextCreateAttempt)
			return {};

		m_nextCreateAttempt = now + chrono::milliseconds(m_retryBackoffMs);
		if (m_retryBackoffMs < RETRY_BACKOFF_MAX_MS)
			m_retryBackoffMs = min(m_retryBackoffMs * 2, RETRY_BACKOFF_MAX_MS);

		// Retry creation
		m_tagState = TagState::NEEDS_CREATION;
		return {};
	}

	// READY or PARTIAL - proceed with reads
	auto totalStart = chrono::steady_clock::now();
	vector<Reading *> readings;

	if (m_readMode == ReadMode::SEQUENTIAL)
		readings = readTagsSequential();
	else
		readings = readTagsScatterGather();

	auto totalEnd = chrono::steady_clock::now();
	double totalMs = chrono::duration<double, milli>(totalEnd - totalStart).count();

	PLCTAG_DEBUG(1, "%s poll#%llu: complete - %zu readings emitted, totalTime=%.1fms",
		logCtx().c_str(), (unsigned long long)m_pollCount, readings.size(), totalMs);

	// Reset debounce on successful read
	m_debounceCounter = 0;

	return readings;
}

// ---------------------------------------------------------------------------
// Outage reporting
//
// A powered-down remote I/O cabinet is an expected operating condition, not an
// error to be restated every cycle. These helpers collapse a sustained failure
// into three log lines - onset, periodic reminder, recovery - so an overnight
// outage is legible instead of burying every other message in the service log.
// ---------------------------------------------------------------------------

// "10/10 " for a fleet of tags, "" for a single named one - so the per-tag
// messages do not read "1/1 tag 'X'".
static string countPrefix(int failed, int total)
{
	if (total <= 1)
		return "";
	return to_string(failed) + "/" + to_string(total) + " ";
}

string PLCTag::humanizeDuration(long seconds)
{
	if (seconds < 0) seconds = 0;
	if (seconds < 60)
		return to_string(seconds) + "s";

	long m = seconds / 60;
	if (m < 60)
		return to_string(m) + "m" + to_string(seconds % 60) + "s";

	long h = m / 60;
	m %= 60;
	if (h < 24)
		return to_string(h) + "h" + to_string(m) + "m";

	return to_string(h / 24) + "d" + to_string(h % 24) + "h" + to_string(m) + "m";
}

string PLCTag::formatWallClock(chrono::system_clock::time_point tp)
{
	time_t t = chrono::system_clock::to_time_t(tp);
	struct tm tmv;
	gmtime_r(&t, &tmv);
	char buf[32];
	strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
	return buf;
}

void PLCTag::reportOutage(OutageTracker& o, const string& what,
                          int failed, int total, const string& lastError)
{
	auto now = chrono::steady_clock::now();

	if (!o.active)
	{
		o.active     = true;
		o.start      = now;
		o.lastLog    = now;
		o.startWall  = chrono::system_clock::now();
		o.suppressed = 0;

		Logger::getLogger()->warn(
			"%s [OUTAGE] %s%s - %s. Retrying continuously; "
			"further reports at most every %s until this clears.",
			logCtx().c_str(), countPrefix(failed, total).c_str(), what.c_str(),
			lastError.c_str(),
			humanizeDuration((long)m_outageLogInterval).c_str());
		return;
	}

	o.suppressed++;

	long sinceLog = (long)chrono::duration_cast<chrono::seconds>(now - o.lastLog).count();
	if (m_outageLogInterval != 0 && sinceLog < (long)m_outageLogInterval)
		return;

	long downFor = (long)chrono::duration_cast<chrono::seconds>(now - o.start).count();
	Logger::getLogger()->warn(
		"%s [OUTAGE] %s%s still down %s (since %s), "
		"last error: %s. %llu further occurrence(s) suppressed since the last report.",
		logCtx().c_str(), countPrefix(failed, total).c_str(), what.c_str(),
		humanizeDuration(downFor).c_str(), formatWallClock(o.startWall).c_str(),
		lastError.c_str(), (unsigned long long)o.suppressed);

	o.lastLog    = now;
	o.suppressed = 0;
}

void PLCTag::clearOutage(OutageTracker& o, const string& what, int ok, int total)
{
	if (!o.active)
		return;

	long downFor = (long)chrono::duration_cast<chrono::seconds>(
		chrono::steady_clock::now() - o.start).count();

	string tally = (total > 1) ? (to_string(ok) + "/" + to_string(total) + " OK")
	                           : string("now reading");
	Logger::getLogger()->warn(
		"%s [OUTAGE] %s RECOVERED after %s (down since %s) - %s.",
		logCtx().c_str(), what.c_str(), humanizeDuration(downFor).c_str(),
		formatWallClock(o.startWall).c_str(), tally.c_str());

	o.active     = false;
	o.suppressed = 0;
}

// Decides what a single poll cycle's failures are worth saying.
//
//   nothing read at all  -> one aggregate line, rate-limited (cabinet down)
//   some tags failed     -> one line per tag, each rate-limited (bad tag name)
//   a failing tag reads  -> one recovery line for that tag
//
// The two cases are kept apart deliberately: a total failure is one event
// about the connection, while a partial failure is N events about N tags, and
// flattening the second into a count would hide which tag is broken.
void PLCTag::reportCycleOutcome(const vector<bool>& ok,
                                const vector<pair<size_t, string>>& failures,
                                int attempted, int succeeded)
{
	if (attempted > 0 && succeeded == 0)
	{
		string lastError = failures.empty() ? "no response from PLC" : failures.back().second;
		reportOutage(m_readOutage, "read tags could not be read",
			(int)failures.size(), attempted, lastError);
		return;
	}

	// Something is flowing again, so any instance-wide outage is over. The
	// aggregate recovery line covers every tag; per-tag trackers are wiped
	// silently so recovery does not cost one line per tag.
	if (m_readOutage.active)
	{
		clearOutage(m_readOutage, "PLC reads", succeeded, attempted);
		resetTagOutages();
		return;
	}

	for (const auto& f : failures)
	{
		if (f.first >= m_tagOutage.size()) continue;
		reportOutage(m_tagOutage[f.first],
			"tag '" + m_tagVector[f.first].name + "' could not be read",
			1, 1, f.second);
	}

	for (size_t i = 0; i < ok.size() && i < m_tagOutage.size(); i++)
	{
		if (ok[i] && m_tagOutage[i].active)
			clearOutage(m_tagOutage[i], "tag '" + m_tagVector[i].name + "'", 1, 1);
	}
}

// Clear per-tag trackers without logging. Used when a whole-instance outage
// ends: the aggregate recovery line already covers every tag, and emitting a
// second line per tag would reintroduce the spam on the way back up.
void PLCTag::resetTagOutages()
{
	for (auto& o : m_tagOutage)
	{
		o.active     = false;
		o.suppressed = 0;
	}
}

// ---------------------------------------------------------------------------
// noteTagFailure
//
// Records a failed poll for a single tag and, once a tag has failed
// m_maxTagFailures polls in a row, destroys and recreates it. libplctag keeps
// per-tag "operation in flight" state that an async read (timeout == 0) only
// clears on completion or on an explicit plc_tag_abort(); a tag left in that
// state answers PLCTAG_ERR_BUSY to every subsequent read, forever. Callers
// abort first, and this provides the second line of defence for anything an
// abort cannot clear.
// ---------------------------------------------------------------------------

void PLCTag::noteTagFailure(size_t i, const char* reason)
{
	if (i >= m_tagFailCount.size())
		return;

	if (++m_tagFailCount[i] < m_maxTagFailures)
		return;

	int old = m_tagCreatedVector[i];
	Logger::getLogger()->warn(
		"%s [TAG_RECOVER] tag='%s' failed %d consecutive polls (%s) - recreating tag",
		logCtx().c_str(), m_tagVector[i].name.c_str(), m_tagFailCount[i], reason);

	m_tagFailCount[i] = 0;

	if (old >= 1)
	{
		plc_tag_abort(old);
		plc_tag_destroy(old);
	}

	int h = openTag(m_tagVector[i].name, m_tagVector[i].typeName, 0);
	if (h >= 1)
	{
		// Created asynchronously - the tag reports PENDING until the session is
		// up, and the scatter phase skips it until then.
		m_tagCreatedVector[i] = h;
	}
	else
	{
		// Could not even issue the create. Fall back to the full creation cycle,
		// which retries every tag that does not currently have a handle.
		m_tagCreatedVector[i] = 0;
		m_tagState = TagState::NEEDS_CREATION;
	}
}

// ---------------------------------------------------------------------------
// readTagsScatterGather
// ---------------------------------------------------------------------------

vector<Reading *> PLCTag::readTagsScatterGather()
{
	auto scatterStart = chrono::steady_clock::now();

	// Phase 1: scatter - issue all reads
	vector<int> pending;
	vector<pair<size_t, string>> failures;  // logged in aggregate at cycle end
	int attempted = 0;
	for (size_t i = 0; i < m_tagVector.size(); i++)
	{
		int h = m_tagCreatedVector[i];
		if (h < 1) continue;
		attempted++;

		// A tag that is still PENDING is either mid-creation or still finishing
		// an operation from an earlier cycle. Reading it now would only return
		// PLCTAG_ERR_BUSY, so skip it and let the failure counter recreate it if
		// it never comes back.
		int s = plc_tag_status(h);
		if (s == PLCTAG_STATUS_PENDING)
		{
			PLCTAG_DEBUG(1, "%s poll#%llu tag='%s' still PENDING - skipping this cycle",
				logCtx().c_str(), (unsigned long long)m_pollCount,
				m_tagVector[i].name.c_str());
			failures.emplace_back(i, "stuck in PENDING");
			noteTagFailure(i, "tag stuck in PENDING");
			continue;
		}

		int rc = plc_tag_read(h, 0);
		if (rc == PLCTAG_STATUS_OK || rc == PLCTAG_STATUS_PENDING)
		{
			pending.push_back((int)i);
		}
		else
		{
			PLCTAG_DEBUG(1, "%s poll#%llu plc_tag_read failed for '%s': %s",
				logCtx().c_str(), (unsigned long long)m_pollCount,
				m_tagVector[i].name.c_str(), plc_tag_decode_error(rc));
			failures.emplace_back(i, plc_tag_decode_error(rc));
			// Clear any in-flight state left on the tag, otherwise every later
			// read of this tag returns PLCTAG_ERR_BUSY.
			plc_tag_abort(h);
			noteTagFailure(i, plc_tag_decode_error(rc));
		}
	}

	auto scatterEnd = chrono::steady_clock::now();
	double scatterUs = chrono::duration<double, micro>(scatterEnd - scatterStart).count();
	PLCTAG_DEBUG(1, "%s poll#%llu: scatter - issued %zu async reads in %.0fus",
		logCtx().c_str(), (unsigned long long)m_pollCount, pending.size(), scatterUs);

	// Phase 2: gather - wait with per-tag tracking
	auto gatherStart = chrono::steady_clock::now();
	auto deadline = gatherStart + chrono::milliseconds(m_timeout);
	// 'done' means "no longer waiting on this tag"; 'ok' means "the tag holds
	// fresh data". They differ for failed reads, whose buffers still hold the
	// previous cycle's values and must not be emitted.
	vector<bool> done(m_tagVector.size(), false);
	vector<bool> ok(m_tagVector.size(), false);

	// Mark tags with no valid handle as done
	for (size_t i = 0; i < m_tagVector.size(); i++)
		if (m_tagCreatedVector[i] < 1) done[i] = true;

	bool anyTimeout = false;
	int succeeded = 0;

	while (chrono::steady_clock::now() < deadline)
	{
		bool allDone = true;
		for (int i : pending)
		{
			if (done[i]) continue;
			int h = m_tagCreatedVector[i];
			int s = plc_tag_status(h);
			if (s == PLCTAG_STATUS_PENDING) { allDone = false; continue; }
			done[i] = true;
			if (s == PLCTAG_STATUS_OK)
			{
				ok[i] = true;
				succeeded++;
				m_tagFailCount[i] = 0;
			}
			else
			{
				PLCTAG_DEBUG(1,
					"%s poll#%llu tag='%s' read error: %s",
					logCtx().c_str(), (unsigned long long)m_pollCount,
					m_tagVector[i].name.c_str(), plc_tag_decode_error(s));
				failures.emplace_back((size_t)i, plc_tag_decode_error(s));
				// A read that ends in an error status leaves libplctag's
				// read_in_flight flag set; without this abort the tag returns
				// PLCTAG_ERR_BUSY on every subsequent poll for good.
				plc_tag_abort(h);
				noteTagFailure(i, plc_tag_decode_error(s));
			}
		}
		if (allDone) break;
		this_thread::sleep_for(chrono::milliseconds(1));
	}

	auto gatherEnd = chrono::steady_clock::now();
	double gatherMs = chrono::duration<double, milli>(gatherEnd - gatherStart).count();

	// Log tags that timed out
	for (int i : pending)
	{
		if (!done[i])
		{
			PLCTAG_DEBUG(1,
				"%s poll#%llu tag='%s' TIMEOUT after %.0fms - skipped this cycle",
				logCtx().c_str(), (unsigned long long)m_pollCount,
				m_tagVector[i].name.c_str(), gatherMs);
			failures.emplace_back((size_t)i, "read timeout");
			plc_tag_abort(m_tagCreatedVector[i]);
			noteTagFailure(i, "read timeout");
			anyTimeout = true;
		}
	}

	PLCTAG_DEBUG(1, "%s poll#%llu: gather - %d/%zu complete after %.0fms",
		logCtx().c_str(), (unsigned long long)m_pollCount, succeeded, pending.size(), gatherMs);

	reportCycleOutcome(ok, failures, attempted, succeeded);

	// Phase 3: collect values
	auto collectStart = chrono::steady_clock::now();
	vector<Reading *> readings = collectReadings(ok);
	auto collectEnd = chrono::steady_clock::now();
	double collectMs = chrono::duration<double, milli>(collectEnd - collectStart).count();

	// Record stats
	double scatterMs = chrono::duration<double, milli>(scatterEnd - scatterStart).count();
	m_stats.record(scatterMs + gatherMs + collectMs, scatterMs, gatherMs, collectMs,
		(int)pending.size(), succeeded, anyTimeout);

	if (m_pollCount % m_statsInterval == 0)
	{
		m_stats.logSummary(logCtx());
		m_stats.reset();
	}

	return readings;
}

// ---------------------------------------------------------------------------
// readTagsSequential
// ---------------------------------------------------------------------------

vector<Reading *> PLCTag::readTagsSequential()
{
	vector<bool> done(m_tagVector.size(), false);
	vector<pair<size_t, string>> failures;  // logged in aggregate at cycle end
	int succeeded = 0;
	int attempted = 0;
	bool anyTimeout = false;

	for (size_t i = 0; i < m_tagVector.size(); i++)
	{
		int h = m_tagCreatedVector[i];
		if (h < 1) continue;
		attempted++;

		auto tagStart = chrono::steady_clock::now();
		int rc = plc_tag_read(h, (int)m_timeout);
		auto tagEnd = chrono::steady_clock::now();
		double tagMs = chrono::duration<double, milli>(tagEnd - tagStart).count();

		if (rc == PLCTAG_STATUS_OK)
		{
			done[i] = true;
			succeeded++;
			m_tagFailCount[i] = 0;
			PLCTAG_DEBUG(2, "%s poll#%llu: tag[%zu]='%s' read in %.1fms",
				logCtx().c_str(), (unsigned long long)m_pollCount,
				i, m_tagVector[i].name.c_str(), tagMs);
		}
		else
		{
			PLCTAG_DEBUG(1,
				"%s poll#%llu tag[%zu]='%s' read error %s after %.1fms",
				logCtx().c_str(), (unsigned long long)m_pollCount,
				i, m_tagVector[i].name.c_str(),
				plc_tag_decode_error(rc), tagMs);
			failures.emplace_back(i, plc_tag_decode_error(rc));
			if (rc == PLCTAG_ERR_TIMEOUT) anyTimeout = true;
			plc_tag_abort(h);
			noteTagFailure(i, plc_tag_decode_error(rc));
		}
	}

	reportCycleOutcome(done, failures, attempted, succeeded);

	vector<Reading *> readings = collectReadings(done);

	// Record simple stats (no scatter phase)
	m_stats.record(0, 0, 0, 0, (int)m_tagVector.size(), succeeded, anyTimeout);
	if (m_pollCount % m_statsInterval == 0)
	{
		m_stats.logSummary(logCtx());
		m_stats.reset();
	}

	return readings;
}

// ---------------------------------------------------------------------------
// collectReadings
// ---------------------------------------------------------------------------

static string formatValue(const string& type, DatapointValue* dpv)
{
	if (!dpv) return "null";
	if (type == "BOOL")
		return dpv->toInt() ? "true" : "false";
	if (type == "STRING")
		return "'" + dpv->toStringValue() + "'";
	if (type == "REAL" || type == "FLOAT32" || type == "LREAL" || type == "FLOAT64" ||
	    type == "UDINT" || type == "UINT32" || type == "DWORD" ||
	    type == "ULINT" || type == "UINT64" ||
	    type == "LINT" || type == "INT64")
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "%.6g", dpv->toDouble());
		return buf;
	}
	return to_string((long)dpv->toInt());
}

vector<Reading *> PLCTag::collectReadings(const vector<bool>& ready)
{
	vector<Reading *> readings;

	// One timestamp for the whole poll cycle. The Reading constructor stamps
	// gettimeofday() per object, so building N Readings in a loop produced N
	// timestamps a few microseconds apart, which downstream storage/export
	// treats as N separate points in time rather than one scan.
	struct timeval scanTime;
	gettimeofday(&scanTime, NULL);

	if (m_readingStructure == 1)
	{
		// Reading per asset - group datapoints by their effective asset name.
		// Preserve tag declaration order for both the asset list and the
		// datapoints within each asset by tracking first-seen order.
		std::unordered_map<string, vector<Datapoint *>> byAsset;
		vector<string> assetOrder;
		for (size_t i = 0; i < m_tagVector.size(); i++)
		{
			if (!ready[i]) continue;
			const auto& t = m_tagVector[i];
			DatapointValue* dpv = getTagValue(m_tagCreatedVector[i], 0,
				t.typeName, t.name);
			if (!dpv) continue;

			const string& asset = t.assetOverride.empty() ? m_assetName : t.assetOverride;
			const string& dpName = t.datapointOverride.empty() ? t.name : t.datapointOverride;

			auto it = byAsset.find(asset);
			if (it == byAsset.end())
			{
				assetOrder.push_back(asset);
				it = byAsset.emplace(asset, vector<Datapoint *>{}).first;
			}
			it->second.push_back(new Datapoint(dpName, *dpv));
			delete dpv;
		}
		for (const auto& asset : assetOrder)
		{
			auto& dps = byAsset[asset];
			if (!dps.empty())
				readings.push_back(new Reading(asset, dps));
		}
	}
	else
	{
		// Reading per PLC tag - each tag becomes its own Reading, using its
		// (possibly remapped) asset and datapoint names.
		for (size_t i = 0; i < m_tagVector.size(); i++)
		{
			if (!ready[i]) continue;
			const auto& t = m_tagVector[i];
			DatapointValue* dpv = getTagValue(m_tagCreatedVector[i], 0,
				t.typeName, t.name);
			if (dpv)
			{
				const string& asset = t.assetOverride.empty() ? m_assetName : t.assetOverride;
				const string& dpName = t.datapointOverride.empty() ? t.name : t.datapointOverride;
				readings.push_back(new Reading(asset, new Datapoint(dpName, *dpv)));
				delete dpv;
			}
		}
	}

	// Stamp every Reading from this cycle with the same instant so they line
	// up as a single scan.
	for (Reading *r : readings)
	{
		r->setTimestamp(scanTime);
		r->setUserTimestamp(scanTime);
	}

	return readings;
}

// ---------------------------------------------------------------------------
// getTagValue
// ---------------------------------------------------------------------------

DatapointValue* PLCTag::getTagValue(int handle, int offset, const string& type,
                                    const string& tagName)
{
	DatapointValue* dpv = nullptr;

	if (type == "BOOL")
	{
		int val = plc_tag_get_bit(handle, offset);
		dpv = new DatapointValue((long)val);
	}
	else if (type == "USINT" || type == "UINT8")
	{
		uint8_t val = plc_tag_get_uint8(handle, offset);
		dpv = new DatapointValue((long)val);
	}
	else if (type == "UINT" || type == "UINT16")
	{
		uint16_t val = plc_tag_get_uint16(handle, offset);
		dpv = new DatapointValue((long)val);
	}
	else if (type == "UDINT" || type == "UINT32" || type == "DWORD")
	{
		uint32_t val = plc_tag_get_uint32(handle, offset);
		dpv = new DatapointValue((double)val);
	}
	else if (type == "ULINT" || type == "UINT64")
	{
		uint64_t val = plc_tag_get_uint64(handle, offset);
		dpv = new DatapointValue((double)val);
	}
	else if (type == "SINT" || type == "INT8")
	{
		int8_t val = plc_tag_get_int8(handle, offset);
		dpv = new DatapointValue((long)val);
	}
	else if (type == "INT" || type == "INT16")
	{
		int16_t val = plc_tag_get_int16(handle, offset);
		dpv = new DatapointValue((long)val);
	}
	else if (type == "DINT" || type == "INT32")
	{
		int32_t val = plc_tag_get_int32(handle, offset);
		dpv = new DatapointValue((long)val);
	}
	else if (type == "LINT" || type == "INT64")
	{
		int64_t val = plc_tag_get_int64(handle, offset);
		dpv = new DatapointValue((double)val);
	}
	else if (type == "REAL" || type == "FLOAT32")
	{
		float val = plc_tag_get_float32(handle, offset);
		dpv = new DatapointValue((double)val);
	}
	else if (type == "LREAL" || type == "FLOAT64")
	{
		double val = plc_tag_get_float64(handle, offset);
		dpv = new DatapointValue(val);
	}
	else if (type == "STRING")
	{
		char buf[256];
		memset(buf, 0, sizeof(buf));
		int rc = plc_tag_get_string(handle, 0, buf, (int)sizeof(buf));
		if (rc == PLCTAG_STATUS_OK)
			dpv = new DatapointValue(string(buf));
		else
		{
			Logger::getLogger()->error("%s [DATA_ERR] poll#%llu string read failed for tag '%s': %s",
				logCtx().c_str(), (unsigned long long)m_pollCount,
				tagName.c_str(), plc_tag_decode_error(rc));
			return nullptr;
		}
	}

	if (dpv)
	{
		PLCTAG_DEBUG(2, "%s poll#%llu tag='%s' type=%s offset=%d value=%s",
			logCtx().c_str(), (unsigned long long)m_pollCount,
			tagName.c_str(), type.c_str(), offset,
			formatValue(type, dpv).c_str());
	}

	return dpv;
}

// ---------------------------------------------------------------------------
// setTagValue
// ---------------------------------------------------------------------------

void PLCTag::setTagValue(int handle, const string& type, const string& value)
{
	if (type == "BOOL")
	{
		int v = (value == "true" || value == "1") ? 1 : 0;
		plc_tag_set_bit(handle, 0, v);
	}
	else if (type == "USINT" || type == "UINT8")
	{
		plc_tag_set_uint8(handle, 0, (uint8_t)stoul(value));
	}
	else if (type == "UINT" || type == "UINT16")
	{
		plc_tag_set_uint16(handle, 0, (uint16_t)stoul(value));
	}
	else if (type == "UDINT" || type == "UINT32" || type == "DWORD")
	{
		plc_tag_set_uint32(handle, 0, (uint32_t)stoul(value));
	}
	else if (type == "ULINT" || type == "UINT64")
	{
		plc_tag_set_uint64(handle, 0, (uint64_t)stoull(value));
	}
	else if (type == "SINT" || type == "INT8")
	{
		plc_tag_set_int8(handle, 0, (int8_t)stoi(value));
	}
	else if (type == "INT" || type == "INT16")
	{
		plc_tag_set_int16(handle, 0, (int16_t)stoi(value));
	}
	else if (type == "DINT" || type == "INT32")
	{
		plc_tag_set_int32(handle, 0, (int32_t)stoi(value));
	}
	else if (type == "LINT" || type == "INT64")
	{
		plc_tag_set_int64(handle, 0, (int64_t)stoll(value));
	}
	else if (type == "REAL" || type == "FLOAT32")
	{
		plc_tag_set_float32(handle, 0, stof(value));
	}
	else if (type == "LREAL" || type == "FLOAT64")
	{
		plc_tag_set_float64(handle, 0, stod(value));
	}
	else if (type == "STRING")
	{
		plc_tag_set_string(handle, 0, value.c_str());
	}
}

// ---------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------

bool PLCTag::write(const string& name, const string& value)
{
	// Find the tag in writeMap
	Tag lookupTag{name, ""};
	auto it = m_writeTagMap.find(lookupTag);
	if (it == m_writeTagMap.end())
	{
		Logger::getLogger()->warn("%s [TAG_ERR] write: tag '%s' not found in writeMap - ignoring",
			logCtx().c_str(), name.c_str());
		return false;
	}

	int handle = it->second;
	const string& typeName = it->first.typeName;

	if (handle < 1)
	{
		Logger::getLogger()->warn("%s [TAG_ERR] write: tag '%s' handle invalid (%d) - tags not yet ready",
			logCtx().c_str(), name.c_str(), handle);
		return false;
	}

	Logger::getLogger()->info("%s write: tag='%s' type='%s' value='%s'",
		logCtx().c_str(), name.c_str(), typeName.c_str(), value.c_str());

	auto writeStart = chrono::steady_clock::now();

	setTagValue(handle, typeName, value);

	PLCTAG_DEBUG(1, "%s write: tag='%s' handle=%d calling plc_tag_write with timeout=%u",
		logCtx().c_str(), name.c_str(), handle, m_timeout);

	int rc = plc_tag_write(handle, (int)m_timeout);

	auto writeEnd = chrono::steady_clock::now();
	double writeMs = chrono::duration<double, milli>(writeEnd - writeStart).count();

	if (rc == PLCTAG_STATUS_OK)
	{
		Logger::getLogger()->info("%s write: tag='%s' SUCCESS in %.1fms",
			logCtx().c_str(), name.c_str(), writeMs);
		return true;
	}
	else
	{
		Logger::getLogger()->error("%s [TAG_ERR] write: tag='%s' FAILED - %s after %.1fms",
			logCtx().c_str(), name.c_str(), plc_tag_decode_error(rc), writeMs);
		return false;
	}
}

// ---------------------------------------------------------------------------
// Async plugin interface
// ---------------------------------------------------------------------------

void PLCTag::registerIngest(INGEST_CB2 cb, void *data)
{
	m_ingestCb = cb;
	m_ingestData = data;
}

void PLCTag::start()
{
	if (m_running.load())
		return;

	m_running = true;
	m_pollThread = thread([this]() { pollLoop(); });

	Logger::getLogger()->info("%s plugin_start: poll thread launched, interval=%ums",
		logCtx().c_str(), m_pollInterval);
}

void PLCTag::stop()
{
	m_running = false;
	if (m_pollThread.joinable())
		m_pollThread.join();
}

void PLCTag::pollLoop()
{
	while (m_running.load())
	{
		{
			lock_guard<mutex> lock(m_mutex);
			vector<Reading *> readings = takeReading();
			if (!readings.empty())
				ingest(readings);
		}

		// Sleep for poll interval, but check m_running periodically
		// to allow prompt shutdown
		auto deadline = chrono::steady_clock::now()
		              + chrono::milliseconds(m_pollInterval);
		while (m_running.load() && chrono::steady_clock::now() < deadline)
			this_thread::sleep_for(chrono::milliseconds(50));
	}

	Logger::getLogger()->info("%s poll thread exiting", logCtx().c_str());
}

void PLCTag::ingest(vector<Reading *>& readings)
{
	if (!m_ingestCb)
		return;

	// Fledge's V2 ingest callback takes ownership of a heap-allocated
	// ReadingSet, which in turn takes ownership of the Reading* pointers.
	// The south service deletes the ReadingSet after dispatch, which deletes
	// the Readings. Clear our local vector so callers don't double-free.
	ReadingSet *set = new ReadingSet(&readings);
	readings.clear();
	(*m_ingestCb)(m_ingestData, set);
}
