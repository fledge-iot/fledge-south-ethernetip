/*
 * Fledge EtherNet/IP south plugin: libplctag tag access.
 *
 * Copyright (c) 2026 Industrial Intelligence Group, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Industrial Intelligence Group
 */
#ifndef _PLCTAG_H
#define _PLCTAG_H

#include <string>
#include <vector>
#include <utility>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

#include <plugin_api.h>
#include <config_category.h>
#include <reading.h>
#include <reading_set.h>
#include <logger.h>

// Ingest callback types (from south_plugin.h - redefined here to avoid
// pulling in the full south service dependency). The V2 callback hands the
// south service a heap-allocated ReadingSet that owns the Reading* objects.
typedef void (*INGEST_CB)(void *, Reading);
typedef void (*INGEST_CB2)(void *, ReadingSet *);

// Verbosity-gated debug logging
#define PLCTAG_DEBUG(lvl, ...) \
	do { if (m_debugLevel >= (lvl)) Logger::getLogger()->debug(__VA_ARGS__); } while (0)

enum class TagState {
	NEEDS_CREATION,
	CREATION_FAILED,
	PARTIAL,
	READY
};

// Retry backoff for tag creation. A cabinet that is down for a shift used to
// draw a fresh create attempt every ~7s for the whole outage; backing off to
// a 30s ceiling cuts that churn while still noticing power-up within half a
// minute. The retry is never abandoned - only slowed.
constexpr uint32_t RETRY_BACKOFF_MIN_MS = 1000;
constexpr uint32_t RETRY_BACKOFF_MAX_MS = 30000;

// Tracks a sustained failure so a hardware outage costs a bounded number of
// log lines: one when it starts, one per m_outageLogInterval while it lasts,
// and one when it clears. Without this a powered-down remote I/O cabinet
// produced one line per tag per retry cycle - thousands per hour, none of
// which said anything the first line had not.
struct OutageTracker {
	bool     active{false};
	uint64_t suppressed{0};   // occurrences folded into the next report
	std::chrono::steady_clock::time_point start;     // for duration
	std::chrono::steady_clock::time_point lastLog;
	std::chrono::system_clock::time_point startWall; // for display
};

enum class ReadMode {
	SCATTER_GATHER,
	SEQUENTIAL
};

struct ReadTag {
	std::string name;
	std::string typeName;
	std::string assetOverride;      // empty = use global asset name
	std::string datapointOverride;  // empty = use PLC tag name
};

struct Tag {
	std::string name;
	std::string typeName;
};

struct TagCompare {
	bool operator()(const Tag& a, const Tag& b) const {
		return a.name < b.name;
	}
};

struct PollStats {
	uint64_t count         = 0;
	double   totalMs       = 0;
	double   scatterMs     = 0;
	double   gatherMs      = 0;
	double   collectMs     = 0;
	double   minGatherMs   = std::numeric_limits<double>::max();
	double   maxGatherMs   = 0;
	uint64_t tagsAttempted = 0;
	uint64_t tagsSucceeded = 0;
	uint64_t pollTimeouts  = 0;

	void record(double total, double scatter, double gather, double collect,
	            int attempted, int succeeded, bool anyTimeout) {
		count++;
		totalMs   += total;
		scatterMs += scatter;
		gatherMs  += gather;
		collectMs += collect;
		minGatherMs = std::min(minGatherMs, gather);
		maxGatherMs = std::max(maxGatherMs, gather);
		tagsAttempted += attempted;
		tagsSucceeded += succeeded;
		if (anyTimeout) pollTimeouts++;
	}

	void logSummary(const std::string& ctx) const;
	void reset() { *this = PollStats{}; }
};

class PLCTag {
public:
	PLCTag();
	~PLCTag();

	// Public only so the unit tests can cover the formatting directly; it has
	// no dependency on instance state.
	static std::string     humanizeDuration(long seconds);

	void                   configure(ConfigCategory* config);
	std::vector<Reading *> takeReading();
	bool                   write(const std::string& name, const std::string& value);

	// Async plugin interface
	void registerIngest(INGEST_CB2 cb, void *data);
	void start();
	void stop();

	std::mutex m_mutex;  // per-instance

	// Compare a configured type name against the CIP data type the PLC reports
	// for the tag. Returns 1 = compatible, 0 = mismatch, -1 = cannot tell.
	// bitAddressed marks a tag that addresses a single bit inside a word, where
	// the PLC reports the containing word's type rather than BOOL.
	// Public so the type table can be unit tested directly.
	static int cipTypeCompatible(const std::string& declared, int cipCode,
	                             bool bitAddressed);
	static const char* cipTypeName(int cipCode);

private:
	// Connection
	std::string  m_address;
	std::string  m_plc;
	std::string  m_path;
	std::string  m_tagString;  // std::string, not char[256]

	// Tag state
	std::atomic<TagState> m_tagState{TagState::NEEDS_CREATION};

	// Read tags
	std::vector<ReadTag> m_tagVector;
	std::vector<int>     m_tagCreatedVector;
	std::vector<int>     m_tagFailCount;  // consecutive failed polls per tag

	// Write tags
	std::map<Tag, int, TagCompare> m_writeTagMap;

	// Background creation
	std::thread        m_creationThread;
	std::atomic<bool>  m_creationInProgress{false};
	std::atomic<bool>  m_stopCreation{false};  // cancel flag for doCleanUp

	// Async poll thread
	std::thread        m_pollThread;
	std::atomic<bool>  m_running{false};
	INGEST_CB2         m_ingestCb{nullptr};
	void              *m_ingestData{nullptr};
	uint32_t           m_pollInterval{1000};

	void pollLoop();
	void ingest(std::vector<Reading *>& readings);

	// Config
	std::string  m_assetName{"PLCTags"};
	uint32_t     m_timeout{5000};
	std::string  m_readMap;
	std::string  m_writeMap;
	int          m_readingStructure{1};  // 0=per-tag, 1=per-asset (matches the config default)
	int          m_debounceCounter{0};   // per-instance
	ReadMode     m_readMode{ReadMode::SCATTER_GATHER};
	int          m_debugLevel{1};
	int          m_statsInterval{100};
	int          m_maxTagFailures{5};    // consecutive failures before a tag is recreated
	uint32_t     m_outageLogInterval{3600};  // seconds between outage reports; 0 = every occurrence

	// Outage tracking / retry backoff
	OutageTracker              m_createOutage;  // whole-instance: no tag could be created
	OutageTracker              m_readOutage;    // whole-instance: no tag could be read
	std::vector<OutageTracker> m_tagOutage;     // per-tag, for partial failures
	uint32_t     m_retryBackoffMs{RETRY_BACKOFF_MIN_MS};
	std::chrono::steady_clock::time_point m_nextCreateAttempt{};

	// Validation
	std::unordered_set<std::string> m_validTypes;

	// Diagnostics
	uint64_t     m_pollCount{0};
	PollStats    m_stats;
	bool         m_startupLogged{false};

	// Methods
	void doCleanUp();
	int  openTag(const std::string& name, const std::string& type, uint32_t timeout);
	void createTagsBackground();
	void noteTagFailure(size_t index, const char* reason);

	// Outage reporting. reportOutage() logs on the leading edge and then at
	// most once per m_outageLogInterval; clearOutage() logs the trailing edge
	// with the total duration. Both are no-ops when there is nothing to say.
	void reportOutage(OutageTracker& o, const std::string& what,
	                  int failed, int total, const std::string& lastError);
	void clearOutage(OutageTracker& o, const std::string& what, int ok, int total);
	void reportCycleOutcome(const std::vector<bool>& ok,
	                        const std::vector<std::pair<size_t, std::string>>& failures,
	                        int attempted, int succeeded);
	void resetTagOutages();  // silently clear per-tag trackers (whole-instance recovery)
	static std::string formatWallClock(std::chrono::system_clock::time_point tp);
	void verifyTagTypes();
	bool isBitAddressed(const ReadTag& tag) const;

	std::vector<Reading *> readTagsScatterGather();
	std::vector<Reading *> readTagsSequential();
	std::vector<Reading *> collectReadings(const std::vector<bool>& ready);

	DatapointValue* getTagValue(int handle, int offset, const std::string& type,
	                            const std::string& tagName);
	void setTagValue(int handle, const std::string& type, const std::string& value);
	void applyTimeout(const std::string& raw);

	std::string logCtx() const;

	static std::string ltrim(std::string s);
	static std::string rtrim(std::string s);
	static std::string trim(std::string s);
};

#endif // _PLCTAG_H
