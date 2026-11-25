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
 * Integration tests for the EtherNet/IP south plugin.
 *
 * These tests connect to a real (virtual) PLC over the network via libplctag.
 * They are NOT run with the mock - they link against the real libplctag.so.
 *
 * Prerequisites:
 *   - A virtual PLC running and reachable at VPLC_ADDRESS
 *   - Tags listed in vplc_config.h pre-created on the vPLC
 *   - libplctag.so installed on the test machine
 *
 * Environment variables (see vplc_config.h):
 *   VPLC_ADDRESS, VPLC_PATH, VPLC_PLC_TYPE, VPLC_TIMEOUT,
 *   VPLC_READMAP, VPLC_WRITEMAP
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include <config_category.h>
#include <reading.h>

#include "plctag.h"
#include "vplc_config.h"

using namespace std;

// ===========================================================================
// Test fixture
// ===========================================================================

class IntegrationTest : public ::testing::Test {
protected:
	PLCTag plctag;

	void SetUp() override
	{
		string json = vplc::buildConfigJson();
		ConfigCategory cfg("inttest", json);
		plctag.configure(&cfg);

		// Wait for background tag creation to complete
		waitForReady();
	}

	void TearDown() override
	{
		plctag.stop();
	}

	// Poll takeReading until tags are created (up to timeout)
	void waitForReady(int maxMs = 10000)
	{
		auto deadline = chrono::steady_clock::now() + chrono::milliseconds(maxMs);
		while (chrono::steady_clock::now() < deadline)
		{
			auto readings = plctag.takeReading();
			if (!readings.empty())
			{
				for (auto *r : readings) delete r;
				return;
			}
			this_thread::sleep_for(chrono::milliseconds(100));
		}
		FAIL() << "Timed out waiting for vPLC tag creation at "
		       << vplc::address() << " (waited " << maxMs << "ms)";
	}

	// Helper: find a datapoint by name in a readings vector
	const Datapoint *findDatapoint(const vector<Reading *>& readings,
	                               const string& name)
	{
		for (auto *r : readings)
			for (auto *dp : r->getReadingData())
				if (dp->getName() == name)
					return dp;
		return nullptr;
	}
};

// ===========================================================================
// Connection tests
// ===========================================================================

TEST_F(IntegrationTest, ConnectsToVPLC)
{
	// SetUp already connected - if we get here, connection succeeded
	auto readings = plctag.takeReading();
	EXPECT_FALSE(readings.empty())
		<< "Expected readings from vPLC at " << vplc::address();
	for (auto *r : readings) delete r;
}

TEST_F(IntegrationTest, AllReadTagsProduceData)
{
	auto readings = plctag.takeReading();
	ASSERT_FALSE(readings.empty());

	// In "Reading per PLC tag" mode, each tag produces one Reading
	// Verify we got at least one datapoint with each expected tag name
	const char *expectedTags[] = {
		vplc::tags::DINT, vplc::tags::REAL, vplc::tags::BOOL_,
		vplc::tags::STRING, vplc::tags::INT, vplc::tags::LREAL,
		vplc::tags::SINT, vplc::tags::LINT
	};

	for (const char *tag : expectedTags)
	{
		const Datapoint *dp = findDatapoint(readings, tag);
		EXPECT_NE(dp, nullptr) << "Missing datapoint for tag: " << tag;
	}

	for (auto *r : readings) delete r;
}

// ===========================================================================
// Read type tests - verify each PLC type can be read without error
// ===========================================================================

TEST_F(IntegrationTest, ReadDINT)
{
	auto readings = plctag.takeReading();
	auto *dp = findDatapoint(readings, vplc::tags::DINT);
	ASSERT_NE(dp, nullptr);

	// DINT should produce an integer value (we can't predict the exact value
	// from the vPLC, but it should be a valid int)
	long val = dp->getData().toInt();
	(void)val;  // just verify no exception/crash

	for (auto *r : readings) delete r;
}

TEST_F(IntegrationTest, ReadREAL)
{
	auto readings = plctag.takeReading();
	auto *dp = findDatapoint(readings, vplc::tags::REAL);
	ASSERT_NE(dp, nullptr);

	double val = dp->getData().toDouble();
	// REAL should be a finite number
	EXPECT_FALSE(std::isnan(val)) << "REAL value is NaN";
	EXPECT_FALSE(std::isinf(val)) << "REAL value is Inf";

	for (auto *r : readings) delete r;
}

TEST_F(IntegrationTest, ReadBOOL)
{
	auto readings = plctag.takeReading();
	auto *dp = findDatapoint(readings, vplc::tags::BOOL_);
	ASSERT_NE(dp, nullptr);

	long val = dp->getData().toInt();
	EXPECT_TRUE(val == 0 || val == 1) << "BOOL value should be 0 or 1, got " << val;

	for (auto *r : readings) delete r;
}

TEST_F(IntegrationTest, ReadSTRING)
{
	auto readings = plctag.takeReading();
	auto *dp = findDatapoint(readings, vplc::tags::STRING);
	ASSERT_NE(dp, nullptr);

	string val = dp->getData().toStringValue();
	// Just verify we got a string without crash - value is vPLC-dependent
	(void)val;

	for (auto *r : readings) delete r;
}

TEST_F(IntegrationTest, ReadINT)
{
	auto readings = plctag.takeReading();
	auto *dp = findDatapoint(readings, vplc::tags::INT);
	ASSERT_NE(dp, nullptr);

	long val = dp->getData().toInt();
	// INT16 range: -32768 to 32767
	EXPECT_GE(val, -32768);
	EXPECT_LE(val, 32767);

	for (auto *r : readings) delete r;
}

TEST_F(IntegrationTest, ReadLREAL)
{
	auto readings = plctag.takeReading();
	auto *dp = findDatapoint(readings, vplc::tags::LREAL);
	ASSERT_NE(dp, nullptr);

	double val = dp->getData().toDouble();
	EXPECT_FALSE(std::isnan(val));
	EXPECT_FALSE(std::isinf(val));

	for (auto *r : readings) delete r;
}

TEST_F(IntegrationTest, ReadSINT)
{
	auto readings = plctag.takeReading();
	auto *dp = findDatapoint(readings, vplc::tags::SINT);
	ASSERT_NE(dp, nullptr);

	long val = dp->getData().toInt();
	// SINT range: -128 to 127
	EXPECT_GE(val, -128);
	EXPECT_LE(val, 127);

	for (auto *r : readings) delete r;
}

TEST_F(IntegrationTest, ReadLINT)
{
	auto readings = plctag.takeReading();
	auto *dp = findDatapoint(readings, vplc::tags::LINT);
	ASSERT_NE(dp, nullptr);

	// LINT is stored as double in DatapointValue
	double val = dp->getData().toDouble();
	EXPECT_FALSE(std::isnan(val));

	for (auto *r : readings) delete r;
}

// ===========================================================================
// Write tests
// ===========================================================================

TEST_F(IntegrationTest, WriteDINTAndReadBack)
{
	// Write a known value
	bool ok = plctag.write(vplc::tags::W_DINT, "12345");
	ASSERT_TRUE(ok) << "Failed to write DINT tag";

	// Small delay for PLC to process
	this_thread::sleep_for(chrono::milliseconds(100));

	// Read back - we need a separate PLCTag configured to read the write tag
	string readJson = R"({"tags": [{"name": ")" + string(vplc::tags::W_DINT) + R"(", "type": "DINT"}]})";
	auto escapeJson = [](const string& s) -> string {
		string out;
		for (char c : s) {
			if (c == '"') out += "\\\"";
			else if (c == '\\') out += "\\\\";
			else out += c;
		}
		return out;
	};

	string cfgJson = R"({
		"address":  {"type":"string", "value":")" + vplc::address() + R"("},
		"plc":      {"type":"string", "value":")" + vplc::plcType() + R"("},
		"path":     {"type":"string", "value":")" + vplc::path() + R"("},
		"timeout":  {"type":"string", "value":")" + vplc::timeout() + R"("},
		"readmap":  {"type":"string", "value":")" + escapeJson(readJson) + R"("},
		"writemap": {"type":"string", "value":"{\"tags\":[]}"},
		"asset":    {"type":"string", "value":"ReadBack"},
		"readingStructure": {"type":"string", "value":"Reading per PLC tag"},
		"readMode": {"type":"string", "value":"scatter-gather"},
		"debugLevel":    {"type":"string", "value":"0"},
		"statsInterval": {"type":"string", "value":"100"},
		"pollInterval":  {"type":"string", "value":"1000"}
	})";

	PLCTag reader;
	ConfigCategory cfg("readback", cfgJson);
	reader.configure(&cfg);

	// Wait for reader to connect
	auto deadline = chrono::steady_clock::now() + chrono::seconds(10);
	vector<Reading *> readings;
	while (chrono::steady_clock::now() < deadline)
	{
		readings = reader.takeReading();
		if (!readings.empty()) break;
		this_thread::sleep_for(chrono::milliseconds(100));
	}

	ASSERT_FALSE(readings.empty()) << "Failed to read back written DINT";
	auto *dp = findDatapoint(readings, vplc::tags::W_DINT);
	ASSERT_NE(dp, nullptr);
	EXPECT_EQ(dp->getData().toInt(), 12345);

	for (auto *r : readings) delete r;
}

TEST_F(IntegrationTest, WriteREAL)
{
	bool ok = plctag.write(vplc::tags::W_REAL, "3.14");
	EXPECT_TRUE(ok) << "Failed to write REAL tag";
}

TEST_F(IntegrationTest, WriteBOOL)
{
	bool ok = plctag.write(vplc::tags::W_BOOL, "1");
	EXPECT_TRUE(ok) << "Failed to write BOOL tag";

	ok = plctag.write(vplc::tags::W_BOOL, "0");
	EXPECT_TRUE(ok) << "Failed to write BOOL tag (reset to 0)";
}

TEST_F(IntegrationTest, WriteSTRING)
{
	bool ok = plctag.write(vplc::tags::W_STRING, "Hello");
	EXPECT_TRUE(ok) << "Failed to write STRING tag";
}

TEST_F(IntegrationTest, WriteNonExistentTagFails)
{
	bool ok = plctag.write("TagThatDoesNotExist", "42");
	EXPECT_FALSE(ok);
}

// ===========================================================================
// Reading structure tests
// ===========================================================================

TEST_F(IntegrationTest, ReadingPerAssetMode)
{
	// Reconfigure in per-asset mode
	string json = vplc::buildConfigJson("Reading per asset");
	ConfigCategory cfg("perasset", json);

	PLCTag assetTag;
	assetTag.configure(&cfg);

	// Wait for ready
	auto deadline = chrono::steady_clock::now() + chrono::seconds(10);
	vector<Reading *> readings;
	while (chrono::steady_clock::now() < deadline)
	{
		readings = assetTag.takeReading();
		if (!readings.empty()) break;
		this_thread::sleep_for(chrono::milliseconds(100));
	}

	ASSERT_FALSE(readings.empty());
	// Per-asset mode: should produce exactly 1 Reading with multiple datapoints
	EXPECT_EQ(readings.size(), 1u);
	EXPECT_GT(readings[0]->getReadingData().size(), 1u)
		<< "Expected multiple datapoints in per-asset mode";

	for (auto *r : readings) delete r;
}

// ===========================================================================
// Read mode tests
// ===========================================================================

TEST_F(IntegrationTest, SequentialReadModeWorks)
{
	string json = vplc::buildConfigJson("Reading per PLC tag", "sequential");
	ConfigCategory cfg("seqread", json);

	PLCTag seqTag;
	seqTag.configure(&cfg);

	auto deadline = chrono::steady_clock::now() + chrono::seconds(10);
	vector<Reading *> readings;
	while (chrono::steady_clock::now() < deadline)
	{
		readings = seqTag.takeReading();
		if (!readings.empty()) break;
		this_thread::sleep_for(chrono::milliseconds(100));
	}

	ASSERT_FALSE(readings.empty())
		<< "Sequential read mode produced no readings";

	for (auto *r : readings) delete r;
}

// ===========================================================================
// Repeated polling stability
// ===========================================================================

TEST_F(IntegrationTest, RepeatedPollingIsStable)
{
	// Poll 20 times in rapid succession - no crashes, no leaks
	int successCount = 0;
	for (int i = 0; i < 20; i++)
	{
		auto readings = plctag.takeReading();
		if (!readings.empty())
		{
			successCount++;
			for (auto *r : readings) delete r;
		}
		this_thread::sleep_for(chrono::milliseconds(50));
	}

	EXPECT_GT(successCount, 15)
		<< "Expected most polls to succeed, got " << successCount << "/20";
}

// ===========================================================================
// Async ingest test
// ===========================================================================

TEST_F(IntegrationTest, AsyncIngestDeliversReadings)
{
	atomic<int> ingestCount{0};
	ReadingSet *lastSet = nullptr;
	mutex readingsMutex;

	struct CbCtx {
		atomic<int>  *count;
		ReadingSet  **lastSet;
		mutex        *mtx;
	};
	CbCtx ctx{&ingestCount, &lastSet, &readingsMutex};

	auto cb = [](void *data, ReadingSet *set) {
		auto *c = static_cast<CbCtx*>(data);
		c->count->fetch_add(1);
		lock_guard<mutex> lock(*c->mtx);
		delete *c->lastSet;  // free previous set (and its Readings)
		*c->lastSet = set;   // take ownership of the new one
	};

	plctag.registerIngest(cb, &ctx);
	plctag.start();

	// Wait for a few ingest cycles
	this_thread::sleep_for(chrono::seconds(3));
	plctag.stop();

	EXPECT_GT(ingestCount.load(), 0)
		<< "Expected at least one ingest callback";

	// Verify the last set of readings has data
	lock_guard<mutex> lock(readingsMutex);
	if (lastSet && !lastSet->getAllReadings().empty())
	{
		EXPECT_GT(lastSet->getAllReadings()[0]->getReadingData().size(), 0u);
	}

	delete lastSet;
}

// ===========================================================================
// Reconfigure test
// ===========================================================================

TEST_F(IntegrationTest, ReconfigureReconnects)
{
	// Read once to confirm connected
	auto readings1 = plctag.takeReading();
	ASSERT_FALSE(readings1.empty());
	for (auto *r : readings1) delete r;

	// Reconfigure with same address - should reconnect
	string json = vplc::buildConfigJson();
	ConfigCategory cfg("reconfig", json);
	{
		lock_guard<mutex> lock(plctag.m_mutex);
		plctag.configure(&cfg);
	}

	// Wait for reconnection
	waitForReady();

	auto readings2 = plctag.takeReading();
	EXPECT_FALSE(readings2.empty())
		<< "Expected readings after reconfigure";
	for (auto *r : readings2) delete r;
}

// ===========================================================================
// Bad address test - verify graceful failure
// ===========================================================================

TEST_F(IntegrationTest, BadAddressFailsGracefully)
{
	PLCTag badTag;
	string json = R"({
		"address":  {"type":"string", "value":"192.0.2.1"},
		"plc":      {"type":"string", "value":"controllogix"},
		"path":     {"type":"string", "value":""},
		"timeout":  {"type":"string", "value":"2000"},
		"readmap":  {"type":"string", "value":"{\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]}"},
		"writemap": {"type":"string", "value":"{\"tags\":[]}"},
		"asset":    {"type":"string", "value":"BadAddr"},
		"readingStructure": {"type":"string", "value":"Reading per PLC tag"},
		"readMode": {"type":"string", "value":"scatter-gather"},
		"debugLevel":    {"type":"string", "value":"0"},
		"statsInterval": {"type":"string", "value":"100"},
		"pollInterval":  {"type":"string", "value":"1000"}
	})";
	ConfigCategory cfg("badaddr", json);
	badTag.configure(&cfg);

	// Should not crash - just return empty readings
	badTag.takeReading();  // triggers creation
	this_thread::sleep_for(chrono::seconds(3));
	auto readings = badTag.takeReading();
	EXPECT_TRUE(readings.empty())
		<< "Expected no readings from unreachable PLC";
}
