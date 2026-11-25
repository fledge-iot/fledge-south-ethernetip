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
 * Integration tests against a virtual SLC 500.
 *
 * These exercise the PCCC path end to end - real libplctag, real EtherNet/IP
 * over a socket - against ab_server, the reference simulator from the
 * libplctag source tree. Start it first:
 *
 *     ./tests/vplc/vplc.sh start
 *
 * or run the whole thing with  make test-slc500,  which starts and stops the
 * simulator around the test binary.
 *
 * If the simulator is not listening every test is skipped rather than failed,
 * so an ordinary `make test` on a machine without it still passes.
 *
 * What is worth covering here, over and above the ControlLogix suite:
 *   - Tag creation with NO CIP path. An SLC has no backplane, and this is the
 *     configuration that was impossible to express before the path fix.
 *   - PCCC data file addressing (N7:0, F8:0, B3:0/1, L19:0, ST18:0) rather
 *     than symbolic Logix tag names.
 *   - The PCCC masked bit write, which is a read-modify-write on the whole
 *     word and could plausibly clobber neighbouring bits.
 */

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include <config_category.h>
#include <reading.h>

#include "plctag.h"
#include "slc500_config.h"

using namespace std;

namespace {

// Split "host:port" and try a TCP connect, so the suite can skip cleanly when
// nobody started the simulator.
bool simulatorReachable(const string& addr)
{
	string host = addr;
	int port = 44818;

	auto colon = addr.rfind(':');
	if (colon != string::npos)
	{
		host = addr.substr(0, colon);
		port = atoi(addr.substr(colon + 1).c_str());
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return false;

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons((uint16_t)port);
	if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1)
	{
		close(fd);
		return false;
	}

	bool ok = (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	close(fd);
	return ok;
}

} // namespace

// ===========================================================================
// Test fixture
// ===========================================================================

class SLC500Test : public ::testing::Test {
protected:
	PLCTag plctag;

	void SetUp() override
	{
		if (!simulatorReachable(slc500::address()))
			GTEST_SKIP() << "No SLC 500 simulator at " << slc500::address()
			             << " - start one with ./tests/vplc/vplc.sh start";

		configureWith();
	}

	void TearDown() override
	{
		plctag.stop();
	}

	void configureWith(const string& readingStructure = "Reading per PLC tag",
	                   const string& readMode = "scatter-gather")
	{
		string json = slc500::buildConfigJson(readingStructure, readMode);
		ConfigCategory cfg("slc500test", json);
		plctag.configure(&cfg);
		waitForReady();
	}

	// Tag creation runs on a background thread; poll until it yields data.
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
		FAIL() << "Timed out waiting for SLC 500 tag creation at "
		       << slc500::address() << " (waited " << maxMs << "ms)";
	}

	const Datapoint *findDatapoint(const vector<Reading *>& readings,
	                               const string& name)
	{
		for (auto *r : readings)
			for (auto *dp : r->getReadingData())
				if (dp->getName() == name)
					return dp;
		return nullptr;
	}

	// Write a value, then poll and hand back the datapoint for the same tag.
	// Readings are freed by the caller via the returned vector.
	void writeThenRead(const char *tag, const string& value,
	                   vector<Reading *>& readings)
	{
		ASSERT_TRUE(plctag.write(tag, value))
			<< "write failed for " << tag << " = " << value;
		this_thread::sleep_for(chrono::milliseconds(100));
		readings = plctag.takeReading();
		ASSERT_FALSE(readings.empty()) << "no readings after writing " << tag;
	}
};

// ===========================================================================
// Connection - the configuration that the path fix made expressible
// ===========================================================================

TEST_F(SLC500Test, ConnectsWithNoCipPath)
{
	auto readings = plctag.takeReading();
	EXPECT_FALSE(readings.empty())
		<< "Expected readings from the SLC 500 simulator at " << slc500::address();
	for (auto *r : readings) delete r;
}

TEST_F(SLC500Test, AllDataFilesProduceDatapoints)
{
	auto readings = plctag.takeReading();
	ASSERT_FALSE(readings.empty());

	const char *expected[] = {
		slc500::tags::INT_, slc500::tags::REAL_, slc500::tags::DINT_,
		slc500::tags::STRING_, slc500::tags::BIT_A, slc500::tags::BIT_B
	};

	for (const char *tag : expected)
		EXPECT_NE(findDatapoint(readings, tag), nullptr)
			<< "Missing datapoint for PCCC address: " << tag;

	for (auto *r : readings) delete r;
}

// ===========================================================================
// Round trips - the simulator starts zeroed, so write a value and read it back
// ===========================================================================

TEST_F(SLC500Test, IntegerFileRoundTrip)
{
	vector<Reading *> readings;
	ASSERT_NO_FATAL_FAILURE(writeThenRead(slc500::tags::INT_, "-1234", readings));

	auto *dp = findDatapoint(readings, slc500::tags::INT_);
	ASSERT_NE(dp, nullptr);
	EXPECT_EQ(dp->getData().toInt(), -1234);

	for (auto *r : readings) delete r;
}

TEST_F(SLC500Test, FloatFileRoundTrip)
{
	vector<Reading *> readings;
	ASSERT_NO_FATAL_FAILURE(writeThenRead(slc500::tags::REAL_, "3.25", readings));

	auto *dp = findDatapoint(readings, slc500::tags::REAL_);
	ASSERT_NE(dp, nullptr);
	// 3.25 is exact in binary32, so this needs no tolerance beyond the
	// float -> double widening.
	EXPECT_NEAR(dp->getData().toDouble(), 3.25, 1e-6);

	for (auto *r : readings) delete r;
}

TEST_F(SLC500Test, LongFileRoundTrip)
{
	vector<Reading *> readings;
	ASSERT_NO_FATAL_FAILURE(writeThenRead(slc500::tags::DINT_, "123456789", readings));

	auto *dp = findDatapoint(readings, slc500::tags::DINT_);
	ASSERT_NE(dp, nullptr);
	EXPECT_EQ(dp->getData().toInt(), 123456789);

	for (auto *r : readings) delete r;
}

TEST_F(SLC500Test, StringFileRoundTrip)
{
	vector<Reading *> readings;
	ASSERT_NO_FATAL_FAILURE(writeThenRead(slc500::tags::STRING_, "HELLO SLC", readings));

	auto *dp = findDatapoint(readings, slc500::tags::STRING_);
	ASSERT_NE(dp, nullptr);
	EXPECT_EQ(dp->getData().toStringValue(), "HELLO SLC");

	for (auto *r : readings) delete r;
}

TEST_F(SLC500Test, BitFileRoundTrip)
{
	vector<Reading *> readings;

	ASSERT_NO_FATAL_FAILURE(writeThenRead(slc500::tags::BIT_A, "1", readings));
	auto *dp = findDatapoint(readings, slc500::tags::BIT_A);
	ASSERT_NE(dp, nullptr);
	EXPECT_EQ(dp->getData().toInt(), 1);
	for (auto *r : readings) delete r;

	ASSERT_NO_FATAL_FAILURE(writeThenRead(slc500::tags::BIT_A, "0", readings));
	dp = findDatapoint(readings, slc500::tags::BIT_A);
	ASSERT_NE(dp, nullptr);
	EXPECT_EQ(dp->getData().toInt(), 0);
	for (auto *r : readings) delete r;
}

// A PCCC bit write is a masked write against the whole 16-bit word. Setting
// one bit must not disturb the others sharing it.
TEST_F(SLC500Test, BitWriteLeavesNeighbouringBitAlone)
{
	vector<Reading *> readings;

	// Start from a known state: both bits of the shared word clear.
	ASSERT_TRUE(plctag.write(slc500::tags::BIT_A, "0"));
	ASSERT_TRUE(plctag.write(slc500::tags::BIT_B, "0"));

	ASSERT_NO_FATAL_FAILURE(writeThenRead(slc500::tags::BIT_A, "1", readings));
	EXPECT_EQ(findDatapoint(readings, slc500::tags::BIT_A)->getData().toInt(), 1);
	EXPECT_EQ(findDatapoint(readings, slc500::tags::BIT_B)->getData().toInt(), 0)
		<< "Writing " << slc500::tags::BIT_A << " cleared its neighbour";
	for (auto *r : readings) delete r;

	// Now set the neighbour; the first bit must survive.
	ASSERT_NO_FATAL_FAILURE(writeThenRead(slc500::tags::BIT_B, "1", readings));
	EXPECT_EQ(findDatapoint(readings, slc500::tags::BIT_B)->getData().toInt(), 1);
	EXPECT_EQ(findDatapoint(readings, slc500::tags::BIT_A)->getData().toInt(), 1)
		<< "Writing " << slc500::tags::BIT_B << " cleared its neighbour";
	for (auto *r : readings) delete r;

	// Leave the word clear for whichever test runs next.
	plctag.write(slc500::tags::BIT_A, "0");
	plctag.write(slc500::tags::BIT_B, "0");
}

// ===========================================================================
// Read modes and reading structure
// ===========================================================================

TEST_F(SLC500Test, SequentialReadModeReadsTheSameValues)
{
	plctag.stop();
	configureWith("Reading per PLC tag", "sequential");

	vector<Reading *> readings;
	ASSERT_NO_FATAL_FAILURE(writeThenRead(slc500::tags::INT_, "4242", readings));

	auto *dp = findDatapoint(readings, slc500::tags::INT_);
	ASSERT_NE(dp, nullptr);
	EXPECT_EQ(dp->getData().toInt(), 4242);

	for (auto *r : readings) delete r;
}

TEST_F(SLC500Test, ReadingPerAssetBundlesEveryDataFile)
{
	plctag.stop();
	configureWith("Reading per asset", "scatter-gather");

	auto readings = plctag.takeReading();
	ASSERT_EQ(readings.size(), 1u)
		<< "Reading per asset should emit exactly one Reading";
	EXPECT_EQ(readings[0]->getAssetName(), "SLC500Test");
	EXPECT_EQ(readings[0]->getReadingData().size(), 6u)
		<< "Expected all six data file elements bundled into one Reading";

	for (auto *r : readings) delete r;
}
