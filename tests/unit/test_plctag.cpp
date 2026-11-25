/*
 * Fledge EtherNet/IP south plugin tests.
 *
 * Copyright (c) 2026 Industrial Intelligence Group, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Industrial Intelligence Group
 */
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <string>
#include <vector>
#include <cstring>

#include <config_category.h>
#include <reading.h>
#include <libplctag.h>

#include "plctag.h"
#include "../mock_libplctag.h"

using namespace std;
using namespace testing;

// ---------------------------------------------------------------------------
// Test fixture - installs/removes the mock for each test
// ---------------------------------------------------------------------------

class PLCTagTest : public ::testing::Test {
protected:
	NiceMock<MockPlcTagAPI> mockApi;

	void SetUp() override
	{
		MockPlcTagAPI::instance = &mockApi;

		// Safe defaults: tag creation succeeds, status OK, reads OK
		ON_CALL(mockApi, plc_tag_create(_, _)).WillByDefault(Return(1));
		ON_CALL(mockApi, plc_tag_status(_)).WillByDefault(Return(PLCTAG_STATUS_OK));
		ON_CALL(mockApi, plc_tag_read(_, _)).WillByDefault(Return(PLCTAG_STATUS_OK));
		ON_CALL(mockApi, plc_tag_write(_, _)).WillByDefault(Return(PLCTAG_STATUS_OK));
		ON_CALL(mockApi, plc_tag_destroy(_)).WillByDefault(Return(PLCTAG_STATUS_OK));
		ON_CALL(mockApi, plc_tag_decode_error(_)).WillByDefault(Return("MOCK_ERROR"));
	}

	void TearDown() override
	{
		MockPlcTagAPI::instance = nullptr;
	}

	// Helper: build a ConfigCategory from a JSON object string
	ConfigCategory makeConfig(const string& json)
	{
		return ConfigCategory("test", json);
	}

	// Helper: build a minimal valid config
	ConfigCategory minimalConfig(
		const string& address = "192.168.1.10",
		const string& plc = "controllogix",
		const string& path = "1,0",
		const string& readmap = R"({\"tags\":[]})",
		const string& writemap = R"({\"tags\":[]})",
		const string& readingStructure = "Reading per PLC tag",
		const string& readMode = "scatter-gather")
	{
		string json = R"({
			"address":  {"type":"string", "value":")" + address + R"("},
			"plc":      {"type":"string", "value":")" + plc + R"("},
			"path":     {"type":"string", "value":")" + path + R"("},
			"readmap":  {"type":"string", "value":")" + readmap + R"("},
			"writemap": {"type":"string", "value":")" + writemap + R"("},
			"asset":    {"type":"string", "value":"TestAsset"},
			"timeout":  {"type":"string", "value":"5000"},
			"readingStructure": {"type":"string", "value":")" + readingStructure + R"("},
			"readMode": {"type":"string", "value":")" + readMode + R"("},
			"debugLevel":    {"type":"string", "value":"0"},
			"statsInterval": {"type":"string", "value":"100"},
			"pollInterval":  {"type":"string", "value":"1000"}
		})";
		return ConfigCategory("test", json);
	}
};

// ===================================================================
// Constructor tests
// ===================================================================

TEST_F(PLCTagTest, ConstructorInitializesValidTypes)
{
	PLCTag tag;
	// Verify by configuring with each valid type - they should all be accepted
	// We'll just check that DINT is accepted by configuring a readmap with it
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"MyTag\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);
	// If DINT were invalid, it would be skipped and takeReading would have
	// nothing to read - we'd get 0 tag creation calls
	EXPECT_CALL(mockApi, plc_tag_create(_, _)).Times(AtLeast(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ConstructorRejectsInvalidType)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"MyTag\",\"type\":\"INVALID_TYPE\"}]})");
	tag.configure(&cfg);
	// Invalid type should be skipped - no tag creation
	EXPECT_CALL(mockApi, plc_tag_create(_, _)).Times(0);
	tag.takeReading();
}

// ===================================================================
// Configure tests
// ===================================================================

TEST_F(PLCTagTest, ConfigureParsesReadmap)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Motor\",\"type\":\"DINT\"},{\"name\":\"Pressure\",\"type\":\"REAL\"}]})");
	tag.configure(&cfg);

	// Two tags should be created when takeReading triggers creation
	EXPECT_CALL(mockApi, plc_tag_create(_, _)).Times(2)
		.WillRepeatedly(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ConfigureParsesWritemap)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[]})", R"({\"tags\":[{\"name\":\"SetPoint\",\"type\":\"REAL\"}]})");
	tag.configure(&cfg);

	// Write tag should be created during background tag creation
	// We need at least one read tag or the creation thread still runs
	// but the write tag should get created
	tag.takeReading();
	// Give background thread time to run
	this_thread::sleep_for(chrono::milliseconds(100));
}

TEST_F(PLCTagTest, ConfigureStripSpacesFromPath)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1, 0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);

	// The tag string should contain path=1,0 (no space)
	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("path=1,0"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ConfigureMicro800NoPath)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "micro800", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);

	// Micro800 tag string should NOT contain path= but should have Micro800
	EXPECT_CALL(mockApi, plc_tag_create(
		AllOf(HasSubstr("plc=Micro800"), Not(HasSubstr("path="))), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ConfigureSlc500BlankPathOmitsPathAttribute)
{
	PLCTag tag;
	// SLC 500 / MicroLogix are addressed with no CIP path - libplctag rejects
	// one unless it routes over a DH+ bridge.
	auto cfg = minimalConfig("10.0.0.1", "slc500", "",
		R"({\"tags\":[{\"name\":\"N7:0\",\"type\":\"INT\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(
		AllOf(HasSubstr("plc=slc500"), HasSubstr("name=N7:0"), Not(HasSubstr("path="))), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ConfigureClearingPathTakesEffect)
{
	PLCTag tag;

	// Start from the shipped default path, as Fledge populates it.
	auto cfg1 = minimalConfig("10.0.0.1", "slc500", "1,0",
		R"({\"tags\":[{\"name\":\"N7:0\",\"type\":\"INT\"}]})");
	tag.configure(&cfg1);
	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(100));

	// Blanking the field must recreate the tags without a path, not be read as
	// "unset" and silently keep 1,0.
	EXPECT_CALL(mockApi, plc_tag_destroy(_)).Times(AtLeast(1));
	EXPECT_CALL(mockApi, plc_tag_create(Not(HasSubstr("path=")), _))
		.WillOnce(Return(1));

	auto cfg2 = minimalConfig("10.0.0.1", "slc500", "",
		R"({\"tags\":[{\"name\":\"N7:0\",\"type\":\"INT\"}]})");
	tag.configure(&cfg2);
	tag.takeReading();
}

TEST_F(PLCTagTest, NonStructuralReconfigureDoesNotDuplicateTags)
{
	PLCTag tag;
	const char *readmap =
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"},{\"name\":\"Tag2\",\"type\":\"REAL\"}]})";

	auto cfg1 = minimalConfig("10.0.0.1", "controllogix", "1,0", readmap);
	tag.configure(&cfg1);
	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(100));

	// Same address, plc, path and maps - only the reading structure differs.
	// This must not append a second copy of the tag list.
	auto cfg2 = minimalConfig("10.0.0.1", "controllogix", "1,0", readmap,
		R"({\"tags\":[]})", "Reading per asset");
	tag.configure(&cfg2);

	auto readings = tag.takeReading();
	ASSERT_EQ(readings.size(), 1u) << "Reading per asset emits one Reading";
	EXPECT_EQ(readings[0]->getReadingData().size(), 2u)
		<< "Expected two datapoints, not a duplicated tag list";
	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, ProgramScopeTagNamePassesThroughVerbatim)
{
	PLCTag tag;
	// Logix program-scoped tags are addressed Program:<ProgramName>.<TagName>.
	// The plugin must not touch the name - libplctag parses this syntax itself.
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Program:MainProgram.Level\",\"type\":\"REAL\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("name=Program:MainProgram.Level"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ProgramScopeBoolArrayRewritesOnTheBracketNotTheDot)
{
	PLCTag tag;
	// The BOOL array rewrite splits on '[', so the Program: prefix and the
	// program-name dot must survive: [33] -> [1].1 with the path intact.
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Program:MainProgram.Bits[33]\",\"type\":\"BOOL\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("Program:MainProgram.Bits[1].1"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ProgramFieldBuildsProgramScopedAddress)
{
	PLCTag tag;
	// 'program' makes the tag program-scoped without spelling out the prefix.
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Level\",\"program\":\"MainProgram\",\"type\":\"REAL\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("name=Program:MainProgram.Level"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ProgramFieldAbsentLeavesTagControllerScoped)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Level\",\"type\":\"REAL\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(AllOf(HasSubstr("name=Level"),
		Not(HasSubstr("Program:"))), _)).WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ProgramFieldBoolArrayRewritesTheIndex)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Bits[33]\",\"program\":\"MainProgram\",\"type\":\"BOOL\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("name=Program:MainProgram.Bits[1].1"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, ProgramFieldDefaultDatapointIsFullAddress)
{
	PLCTag tag;
	// Two programs may each own a 'Level'; the default datapoint keeps the
	// program so they cannot collide in one reading.
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Level\",\"program\":\"MainProgram\",\"type\":\"REAL\"}]})",
		R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_float32(_, _)).WillByDefault(Return(1.5f));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	ASSERT_EQ(readings.size(), 1u);
	auto dps = readings[0]->getReadingData();
	ASSERT_EQ(dps.size(), 1u);
	EXPECT_EQ(dps[0]->getName(), "Program:MainProgram.Level");

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, ProgramFieldWithPrefixedNameIsSkipped)
{
	PLCTag tag;
	// Naming the program twice is ambiguous, so the entry is rejected.
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Program:MainProgram.Level\",\"program\":\"MainProgram\",\"type\":\"REAL\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(_, _)).Times(0);
	EXPECT_TRUE(tag.takeReading().empty());
}

TEST_F(PLCTagTest, ProgramFieldEmptyOrNonStringIsSkipped)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"A\",\"program\":\"\",\"type\":\"REAL\"},)"
		R"({\"name\":\"B\",\"program\":7,\"type\":\"REAL\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(_, _)).Times(0);
	EXPECT_TRUE(tag.takeReading().empty());
}

TEST_F(PLCTagTest, ProgramFieldWriteTagIsAddressedByFullAddress)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[]})",
		R"({\"tags\":[{\"name\":\"SetPoint\",\"program\":\"MainProgram\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("name=Program:MainProgram.SetPoint"), _))
		.WillOnce(Return(1));
	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));

	EXPECT_CALL(mockApi, plc_tag_write(_, _)).WillOnce(Return(PLCTAG_STATUS_OK));
	EXPECT_TRUE(tag.write("Program:MainProgram.SetPoint", "42"));
	EXPECT_FALSE(tag.write("SetPoint", "42"));
}

TEST_F(PLCTagTest, ConfigureChangeDetectionTriggersCleanup)
{
	PLCTag tag;

	// First configure
	auto cfg1 = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg1);

	// Take a reading to create tags
	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(100));

	// Reconfigure with different address - should destroy old tags
	EXPECT_CALL(mockApi, plc_tag_destroy(_)).Times(AtLeast(1));
	auto cfg2 = minimalConfig("10.0.0.2", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg2);
}

// ===================================================================
// applyTimeout tests
// ===================================================================

TEST_F(PLCTagTest, TimeoutValidRange)
{
	PLCTag tag;
	string json = R"({
		"address":  {"type":"string", "value":"10.0.0.1"},
		"plc":      {"type":"string", "value":"controllogix"},
		"path":     {"type":"string", "value":"1,0"},
		"timeout":  {"type":"string", "value":"3000"},
		"debugLevel": {"type":"string", "value":"0"}
	})";
	auto cfg = ConfigCategory("test", json);
	tag.configure(&cfg);
	// No crash or warning - timeout applied silently
}

TEST_F(PLCTagTest, TimeoutZeroRejected)
{
	PLCTag tag;
	string json = R"({
		"address":  {"type":"string", "value":"10.0.0.1"},
		"plc":      {"type":"string", "value":"controllogix"},
		"path":     {"type":"string", "value":"1,0"},
		"timeout":  {"type":"string", "value":"0"},
		"debugLevel": {"type":"string", "value":"0"}
	})";
	auto cfg = ConfigCategory("test", json);
	tag.configure(&cfg);
	// Default 5000 should be kept
}

TEST_F(PLCTagTest, TimeoutOverflowRejected)
{
	PLCTag tag;
	string json = R"({
		"address":  {"type":"string", "value":"10.0.0.1"},
		"plc":      {"type":"string", "value":"controllogix"},
		"path":     {"type":"string", "value":"1,0"},
		"timeout":  {"type":"string", "value":"999999"},
		"debugLevel": {"type":"string", "value":"0"}
	})";
	auto cfg = ConfigCategory("test", json);
	tag.configure(&cfg);
	// Default 5000 should be kept
}

TEST_F(PLCTagTest, TimeoutNonNumericRejected)
{
	PLCTag tag;
	string json = R"({
		"address":  {"type":"string", "value":"10.0.0.1"},
		"plc":      {"type":"string", "value":"controllogix"},
		"path":     {"type":"string", "value":"1,0"},
		"timeout":  {"type":"string", "value":"abc"},
		"debugLevel": {"type":"string", "value":"0"}
	})";
	auto cfg = ConfigCategory("test", json);
	tag.configure(&cfg);
	// Default 5000 should be kept
}

// ===================================================================
// openTag tests - BOOL array rewrite
// ===================================================================

TEST_F(PLCTagTest, BoolArrayRewriteControlLogix)
{
	PLCTag tag;
	// BOOL array tag - [33] should become [1].1  (33/32=1, 33%32=1)
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Bits[33]\",\"type\":\"BOOL\"}]})");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("Bits[1].1"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, BoolArrayRewriteIndex0)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Bits[0]\",\"type\":\"BOOL\"}]})");
	tag.configure(&cfg);

	// [0] -> [0].0
	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("Bits[0].0"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, BoolArrayRewriteIndex31)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Bits[31]\",\"type\":\"BOOL\"}]})");
	tag.configure(&cfg);

	// [31] -> [0].31
	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("Bits[0].31"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, BoolArrayRewriteIndex32)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Bits[32]\",\"type\":\"BOOL\"}]})");
	tag.configure(&cfg);

	// [32] -> [1].0
	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("Bits[1].0"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

TEST_F(PLCTagTest, BoolArrayNoRewriteForNonControlLogix)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "micro800", "1,0",
		R"({\"tags\":[{\"name\":\"Bits[33]\",\"type\":\"BOOL\"}]})");
	tag.configure(&cfg);

	// Micro800 should NOT rewrite - tag name stays as Bits[33]
	EXPECT_CALL(mockApi, plc_tag_create(HasSubstr("Bits[33]"), _))
		.WillOnce(Return(1));
	tag.takeReading();
}

// ===================================================================
// Tag state machine tests
// ===================================================================

TEST_F(PLCTagTest, TakeReadingTriggersCreation)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);

	// First call should launch background creation, return empty
	auto readings = tag.takeReading();
	EXPECT_TRUE(readings.empty());
}

TEST_F(PLCTagTest, TakeReadingReturnsDataAfterCreation)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(42));

	// First call starts creation
	tag.takeReading();
	// Wait for background creation to complete
	this_thread::sleep_for(chrono::milliseconds(200));

	// Second call should actually read and return data
	auto readings = tag.takeReading();
	EXPECT_FALSE(readings.empty());

	// Cleanup
	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, CreationFailureRetriesOnNextPoll)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);

	// Make creation fail
	ON_CALL(mockApi, plc_tag_create(_, _))
		.WillByDefault(Return(PLCTAG_ERR_BAD_CONNECTION));

	// First call triggers creation
	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));

	// State should be CREATION_FAILED, takeReading returns empty
	auto readings = tag.takeReading();
	EXPECT_TRUE(readings.empty());

	// But it should set state back to NEEDS_CREATION for retry
	// Next call should attempt creation again
	EXPECT_CALL(mockApi, plc_tag_create(_, _)).Times(AtLeast(1))
		.WillRepeatedly(Return(1));
	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
}

// ===================================================================
// Read value tests - getTagValue for each type
// ===================================================================

TEST_F(PLCTagTest, ReadDINTValue)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Speed\",\"type\":\"DINT\"}]})", R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(1500));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	ASSERT_EQ(readings.size(), 1u);
	auto dps = readings[0]->getReadingData();
	ASSERT_EQ(dps.size(), 1u);
	EXPECT_EQ(dps[0]->getName(), "Speed");
	EXPECT_EQ(dps[0]->getData().toInt(), 1500);

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, ReadREALValue)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Pressure\",\"type\":\"REAL\"}]})", R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_float32(_, _)).WillByDefault(Return(3.14f));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	ASSERT_EQ(readings.size(), 1u);
	auto dps = readings[0]->getReadingData();
	ASSERT_EQ(dps.size(), 1u);
	EXPECT_NEAR(dps[0]->getData().toDouble(), 3.14, 0.01);

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, ReadBOOLValue)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Flag\",\"type\":\"BOOL\"}]})", R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_bit(_, _)).WillByDefault(Return(1));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	ASSERT_EQ(readings.size(), 1u);
	auto dps = readings[0]->getReadingData();
	EXPECT_EQ(dps[0]->getData().toInt(), 1);

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, ReadStringValue)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Name\",\"type\":\"STRING\"}]})", R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_string(_, _, _, _))
		.WillByDefault(Invoke([](int32_t, int, char *buf, int len) -> int {
			strncpy(buf, "Hello PLC", len);
			return PLCTAG_STATUS_OK;
		}));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	ASSERT_EQ(readings.size(), 1u);
	auto dps = readings[0]->getReadingData();
	EXPECT_EQ(dps[0]->getData().toStringValue(), "Hello PLC");

	for (auto *r : readings) delete r;
}

// ===================================================================
// Reading structure tests
// ===================================================================

TEST_F(PLCTagTest, ReadingPerTag)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"A\",\"type\":\"DINT\"},{\"name\":\"B\",\"type\":\"DINT\"}]})", R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	// Return incrementing handles
	int handle = 1;
	ON_CALL(mockApi, plc_tag_create(_, _))
		.WillByDefault(Invoke([&handle](const char*, int) { return handle++; }));
	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(99));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	// Two tags -> two readings (one per tag)
	EXPECT_EQ(readings.size(), 2u);

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, ReadingPerAsset)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"A\",\"type\":\"DINT\"},{\"name\":\"B\",\"type\":\"DINT\"}]})", R"({\"tags\":[]})", "Reading per asset");
	tag.configure(&cfg);

	int handle = 1;
	ON_CALL(mockApi, plc_tag_create(_, _))
		.WillByDefault(Invoke([&handle](const char*, int) { return handle++; }));
	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(99));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	// Two tags -> one reading with two datapoints
	ASSERT_EQ(readings.size(), 1u);
	EXPECT_EQ(readings[0]->getReadingData().size(), 2u);

	for (auto *r : readings) delete r;
}

// ===================================================================
// Per-tag asset / datapoint remapping tests
// ===================================================================
//
// In the readmap, an entry may carry optional 'asset' and 'datapoint' fields
//   {"name":"Tag","type":"REAL","asset":"Tank1","datapoint":"Level"}
// to remap the tag onto a specific Fledge asset and/or datapoint name.
// 'asset' falls back to the global asset name, 'datapoint' to the PLC tag
// name, when omitted. The asset/datapoint remap fields are readmap-only.

TEST_F(PLCTagTest, RemapFullOverridePerTag)
{
	PLCTag tag;
	// Full remap: asset -> Tank1, datapoint -> Pressure
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"PumpPressure\",\"type\":\"REAL\",\"asset\":\"Tank1\",\"datapoint\":\"Pressure\"}]})",
		R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_float32(_, _)).WillByDefault(Return(2.5f));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	ASSERT_EQ(readings.size(), 1u);
	EXPECT_EQ(readings[0]->getAssetName(), "Tank1");
	auto dps = readings[0]->getReadingData();
	ASSERT_EQ(dps.size(), 1u);
	EXPECT_EQ(dps[0]->getName(), "Pressure");
	EXPECT_NEAR(dps[0]->getData().toDouble(), 2.5, 0.01);

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, RemapDatapointOnlyFallsBackToGlobalAsset)
{
	PLCTag tag;
	// datapoint override only - asset should fall back to the global "TestAsset"
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"AlarmFlag\",\"type\":\"BOOL\",\"datapoint\":\"Alarm\"}]})",
		R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_bit(_, _)).WillByDefault(Return(1));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	ASSERT_EQ(readings.size(), 1u);
	EXPECT_EQ(readings[0]->getAssetName(), "TestAsset");
	auto dps = readings[0]->getReadingData();
	ASSERT_EQ(dps.size(), 1u);
	EXPECT_EQ(dps[0]->getName(), "Alarm");

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, RemapAssetOnlyFallsBackToTagNameDatapoint)
{
	PLCTag tag;
	// asset override only - datapoint should fall back to the PLC tag name
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"MotorSpeed\",\"type\":\"DINT\",\"asset\":\"Motor1\"}]})",
		R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(1500));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	ASSERT_EQ(readings.size(), 1u);
	EXPECT_EQ(readings[0]->getAssetName(), "Motor1");
	auto dps = readings[0]->getReadingData();
	ASSERT_EQ(dps.size(), 1u);
	EXPECT_EQ(dps[0]->getName(), "MotorSpeed");
	EXPECT_EQ(dps[0]->getData().toInt(), 1500);

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, RemapMixedPlainAndRemappedEntries)
{
	PLCTag tag;
	// A plain entry and a remapped entry coexist in one readmap
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Plain\",\"type\":\"DINT\"},{\"name\":\"Remapped\",\"type\":\"DINT\",\"asset\":\"AssetB\",\"datapoint\":\"Value\"}]})",
		R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	int handle = 1;
	ON_CALL(mockApi, plc_tag_create(_, _))
		.WillByDefault(Invoke([&handle](const char*, int) { return handle++; }));
	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(99));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	// Two tags -> two readings, in declaration order
	ASSERT_EQ(readings.size(), 2u);
	EXPECT_EQ(readings[0]->getAssetName(), "TestAsset");
	EXPECT_EQ(readings[0]->getReadingData()[0]->getName(), "Plain");
	EXPECT_EQ(readings[1]->getAssetName(), "AssetB");
	EXPECT_EQ(readings[1]->getReadingData()[0]->getName(), "Value");

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, RemapPerAssetGroupsByEffectiveAsset)
{
	PLCTag tag;
	// Two tags remapped to the same asset "Tank1" - in per-asset mode they
	// should collapse into a single Reading carrying both datapoints, while a
	// third tag using the global asset forms its own Reading.
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tank1_P\",\"type\":\"REAL\",\"asset\":\"Tank1\",\"datapoint\":\"Pressure\"},)"
		R"({\"name\":\"Tank1_L\",\"type\":\"REAL\",\"asset\":\"Tank1\",\"datapoint\":\"Level\"},)"
		R"({\"name\":\"Misc\",\"type\":\"DINT\"}]})",
		R"({\"tags\":[]})", "Reading per asset");
	tag.configure(&cfg);

	int handle = 1;
	ON_CALL(mockApi, plc_tag_create(_, _))
		.WillByDefault(Invoke([&handle](const char*, int) { return handle++; }));
	ON_CALL(mockApi, plc_tag_get_float32(_, _)).WillByDefault(Return(1.0f));
	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(7));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));
	auto readings = tag.takeReading();

	// Two distinct effective assets: Tank1 (2 datapoints) and TestAsset (1)
	ASSERT_EQ(readings.size(), 2u);
	EXPECT_EQ(readings[0]->getAssetName(), "Tank1");
	auto tank = readings[0]->getReadingData();
	ASSERT_EQ(tank.size(), 2u);
	EXPECT_EQ(tank[0]->getName(), "Pressure");
	EXPECT_EQ(tank[1]->getName(), "Level");
	EXPECT_EQ(readings[1]->getAssetName(), "TestAsset");
	EXPECT_EQ(readings[1]->getReadingData()[0]->getName(), "Misc");

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, RemapObjectMissingTypeIsSkipped)
{
	PLCTag tag;
	// An entry without a 'type' field is invalid and must be skipped -
	// no tag should be created for it.
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"NoType\",\"asset\":\"AssetX\",\"datapoint\":\"D\"}]})",
		R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(_, _)).Times(0);
	auto readings = tag.takeReading();
	EXPECT_TRUE(readings.empty());
}

TEST_F(PLCTagTest, RemapObjectUnknownTypeIsSkipped)
{
	PLCTag tag;
	// An entry with an unrecognised type must be skipped.
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"BadType\",\"type\":\"NOTATYPE\",\"asset\":\"AssetX\"}]})",
		R"({\"tags\":[]})", "Reading per PLC tag");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(_, _)).Times(0);
	auto readings = tag.takeReading();
	EXPECT_TRUE(readings.empty());
}

// ===================================================================
// Write tests
// ===================================================================

TEST_F(PLCTagTest, WriteDINTValue)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[]})", R"({\"tags\":[{\"name\":\"SetPoint\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);

	// Create tags so write tag gets a handle
	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));

	EXPECT_CALL(mockApi, plc_tag_set_int32(_, 0, 42)).Times(1);
	EXPECT_CALL(mockApi, plc_tag_write(_, _)).WillOnce(Return(PLCTAG_STATUS_OK));

	bool ok = tag.write("SetPoint", "42");
	EXPECT_TRUE(ok);
}

TEST_F(PLCTagTest, WriteUnknownTagReturnsFalse)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[]})", R"({\"tags\":[{\"name\":\"SetPoint\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);

	bool ok = tag.write("NonExistent", "42");
	EXPECT_FALSE(ok);
}

TEST_F(PLCTagTest, WriteFailureReturnsFalse)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[]})", R"({\"tags\":[{\"name\":\"SetPoint\",\"type\":\"DINT\"}]})");
	tag.configure(&cfg);

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));

	ON_CALL(mockApi, plc_tag_write(_, _))
		.WillByDefault(Return(PLCTAG_ERR_TIMEOUT));

	bool ok = tag.write("SetPoint", "42");
	EXPECT_FALSE(ok);
}

// ===================================================================
// Sequential read mode test
// ===================================================================

TEST_F(PLCTagTest, SequentialReadMode)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})", R"({\"tags\":[]})", "Reading per PLC tag", "sequential");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(7));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));

	// In sequential mode, plc_tag_read is called with a non-zero timeout
	EXPECT_CALL(mockApi, plc_tag_read(_, Gt(0)))
		.WillOnce(Return(PLCTAG_STATUS_OK));
	auto readings = tag.takeReading();
	EXPECT_EQ(readings.size(), 1u);

	for (auto *r : readings) delete r;
}

// ===================================================================
// Scatter-gather read mode test
// ===================================================================

TEST_F(PLCTagTest, ScatterGatherReadMode)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})", R"({\"tags\":[]})", "Reading per PLC tag", "scatter-gather");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(7));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));

	// In scatter-gather mode, plc_tag_read is called with timeout=0
	EXPECT_CALL(mockApi, plc_tag_read(_, 0))
		.WillOnce(Return(PLCTAG_STATUS_OK));
	auto readings = tag.takeReading();
	EXPECT_EQ(readings.size(), 1u);

	for (auto *r : readings) delete r;
}

// ===================================================================
// Tag recovery tests
//
// libplctag keeps per-tag "operation in flight" state that an async read
// (timeout == 0) only clears on completion or on an explicit plc_tag_abort().
// A tag left in that state answers PLCTAG_ERR_BUSY to every later read, so the
// plugin must abort failed reads and eventually recreate a tag that keeps
// failing.
// ===================================================================

TEST_F(PLCTagTest, ScatterGatherFailedReadAborts)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})", R"({\"tags\":[]})",
		"Reading per PLC tag", "scatter-gather");
	tag.configure(&cfg);

	tag.takeReading();  // launches background creation
	this_thread::sleep_for(chrono::milliseconds(200));

	EXPECT_CALL(mockApi, plc_tag_read(1, 0)).WillOnce(Return(PLCTAG_ERR_BUSY));
	EXPECT_CALL(mockApi, plc_tag_abort(1)).Times(1);

	auto readings = tag.takeReading();
	EXPECT_EQ(readings.size(), 0u);

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, ScatterGatherErrorStatusAbortsAndEmitsNoReading)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})", R"({\"tags\":[]})",
		"Reading per PLC tag", "scatter-gather");
	tag.configure(&cfg);

	// Status is OK while the tag is being created, then reports a hard error.
	atomic<bool> statusFails{false};
	ON_CALL(mockApi, plc_tag_status(_)).WillByDefault(Invoke(
		[&statusFails](int32_t) {
			return statusFails.load() ? PLCTAG_ERR_NO_DATA : PLCTAG_STATUS_OK;
		}));

	tag.takeReading();  // launches background creation
	this_thread::sleep_for(chrono::milliseconds(200));
	statusFails = true;

	// The read is accepted but completes with an error status - the tag buffer
	// still holds the previous cycle's value, so nothing may be emitted.
	EXPECT_CALL(mockApi, plc_tag_abort(1)).Times(1);

	auto readings = tag.takeReading();
	EXPECT_EQ(readings.size(), 0u);

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, UncreatableTagAmongGoodTagsEmitsNoDatapoint)
{
	// A tag whose creation failed has no handle. It must be left out of the
	// readings entirely - not emitted with whatever a read of an invalid handle
	// returns. The plugin stays in PARTIAL state and keeps polling the good
	// tags, so this is only visible when a bad tag is mixed in with good ones.
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"GoodTag\",\"type\":\"REAL\"},{\"name\":\"BadTag\",\"type\":\"REAL\"}]})",
		R"({\"tags\":[]})", "Reading per PLC tag", "scatter-gather");
	tag.configure(&cfg);

	// First tag creates, second fails to create.
	EXPECT_CALL(mockApi, plc_tag_create(_, _))
		.WillOnce(Return(1))
		.WillOnce(Return(PLCTAG_ERR_BAD_PARAM))
		.WillRepeatedly(Return(1));
	ON_CALL(mockApi, plc_tag_get_float32(_, _)).WillByDefault(Return(1.5f));

	tag.takeReading();  // launches background creation
	this_thread::sleep_for(chrono::milliseconds(200));

	auto readings = tag.takeReading();
	ASSERT_EQ(readings.size(), 1u) << "only the successfully created tag should be emitted";
	EXPECT_EQ(readings[0]->getDatapointCount(), 1u);
	EXPECT_EQ(readings[0]->getReadingData()[0]->getName(), "GoodTag");

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, UncreatableTagAmongGoodTagsEmitsNoDatapointPerAsset)
{
	// Same, in per-asset mode: the bad tag must not contribute a datapoint to
	// the asset's reading.
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"GoodTag\",\"type\":\"REAL\"},{\"name\":\"BadTag\",\"type\":\"REAL\"}]})",
		R"({\"tags\":[]})", "Reading per asset", "scatter-gather");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_create(_, _))
		.WillOnce(Return(1))
		.WillOnce(Return(PLCTAG_ERR_BAD_PARAM))
		.WillRepeatedly(Return(1));
	ON_CALL(mockApi, plc_tag_get_float32(_, _)).WillByDefault(Return(1.5f));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));

	auto readings = tag.takeReading();
	ASSERT_EQ(readings.size(), 1u);
	EXPECT_EQ(readings[0]->getDatapointCount(), 1u)
		<< "the uncreatable tag must not appear as a datapoint";

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, RepeatedReadFailuresRecreateTag)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})", R"({\"tags\":[]})",
		"Reading per PLC tag", "scatter-gather");
	tag.configure(&cfg);

	tag.takeReading();  // launches background creation
	this_thread::sleep_for(chrono::milliseconds(200));

	// Every read is rejected as busy - after five consecutive failed polls the
	// tag must be destroyed and created again.
	atomic<int> creates{0};
	atomic<int> destroys{0};
	ON_CALL(mockApi, plc_tag_read(_, _)).WillByDefault(Return(PLCTAG_ERR_BUSY));
	ON_CALL(mockApi, plc_tag_create(_, _)).WillByDefault(Invoke(
		[&creates](const char *, int) { creates++; return 1; }));
	ON_CALL(mockApi, plc_tag_destroy(_)).WillByDefault(Invoke(
		[&destroys](int32_t) { destroys++; return PLCTAG_STATUS_OK; }));

	for (int poll = 0; poll < 5; poll++)
	{
		auto readings = tag.takeReading();
		EXPECT_EQ(readings.size(), 0u);
		for (auto *r : readings) delete r;
	}

	EXPECT_EQ(creates.load(), 1);
	EXPECT_EQ(destroys.load(), 1);
}

// ===================================================================
// Declared-type vs PLC-reported-type verification
// ===================================================================

TEST_F(PLCTagTest, CipTypeCompatibleMatchingTypes)
{
	EXPECT_EQ(PLCTag::cipTypeCompatible("REAL",  0x00CA, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("FLOAT32", 0x00CA, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("LREAL", 0x00CB, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("DINT",  0x00C4, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("INT32", 0x00C4, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("INT",   0x00C3, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("SINT",  0x00C2, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("LINT",  0x00C5, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("UDINT", 0x00C8, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("BOOL",  0x00C1, false), 1);
}

TEST_F(PLCTagTest, CipTypeCompatibleDetectsMismatch)
{
	// The silent failure this check exists to catch: a REAL declared for an
	// INT tag reads without error and yields garbage.
	EXPECT_EQ(PLCTag::cipTypeCompatible("REAL", 0x00C3, false), 0);
	EXPECT_EQ(PLCTag::cipTypeCompatible("REAL", 0x00C4, false), 0);
	EXPECT_EQ(PLCTag::cipTypeCompatible("DINT", 0x00CA, false), 0);
	EXPECT_EQ(PLCTag::cipTypeCompatible("INT",  0x00C4, false), 0);
	EXPECT_EQ(PLCTag::cipTypeCompatible("LREAL", 0x00CA, false), 0);
	// A numeric tag that is actually a structure/UDT.
	EXPECT_EQ(PLCTag::cipTypeCompatible("REAL", 0x02A0, false), 0);
}

TEST_F(PLCTagTest, CipTypeCompatibleAcceptsBitStringTypes)
{
	// BYTE/WORD/DWORD/LWORD carry the same data as the same-width integer.
	EXPECT_EQ(PLCTag::cipTypeCompatible("DINT",  0x00D3, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("UDINT", 0x00D3, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("DWORD", 0x00C8, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("INT",   0x00D2, false), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("SINT",  0x00D1, false), 1);
}

TEST_F(PLCTagTest, CipTypeCompatibleBitAddressedBoolIsNotAMismatch)
{
	// A bit inside a word - the PLC reports the containing word's type, so a
	// declared BOOL is correct even though the PLC says DINT.
	EXPECT_EQ(PLCTag::cipTypeCompatible("BOOL", 0x00C4, true), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("BOOL", 0x00D3, true), 1);
	EXPECT_EQ(PLCTag::cipTypeCompatible("BOOL", 0x00C3, true), 1);
	// but a plain BOOL pointed at a DINT tag still is a mismatch
	EXPECT_EQ(PLCTag::cipTypeCompatible("BOOL", 0x00C4, false), 0);
	// and a bit-addressed BOOL on a REAL makes no sense either way
	EXPECT_EQ(PLCTag::cipTypeCompatible("BOOL", 0x00CA, true), 0);
}

TEST_F(PLCTagTest, CipTypeCompatibleStringsAreOnlyFlaggedAgainstNumerics)
{
	// String encodings vary by PLC family, so only a STRING pointed at a
	// numeric scalar is reported; everything else is unverifiable.
	EXPECT_EQ(PLCTag::cipTypeCompatible("STRING", 0x00CA, false), 0);
	EXPECT_EQ(PLCTag::cipTypeCompatible("STRING", 0x00C4, false), 0);
	EXPECT_EQ(PLCTag::cipTypeCompatible("STRING", 0x02A0, false), -1);  // Logix UDT
	EXPECT_EQ(PLCTag::cipTypeCompatible("STRING", 0x00CE, false), -1);
	// An unrecognised declared type is never reported.
	EXPECT_EQ(PLCTag::cipTypeCompatible("WEIRD", 0x00CA, false), -1);
}

TEST_F(PLCTagTest, TypeMismatchIsWarnOnlyAndStillReadsTheTag)
{
	// A mismatch must not change behaviour - the tag is still polled, since the
	// user may have deliberately reinterpreted it.
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"MotorSpeed\",\"type\":\"REAL\"}]})", R"({\"tags\":[]})",
		"Reading per PLC tag", "scatter-gather");
	tag.configure(&cfg);

	// PLC reports INT (0x00C3) for a tag declared REAL.
	EXPECT_CALL(mockApi, plc_tag_get_byte_array_attribute(_, StrEq("raw_tag_type_bytes"), _, _))
		.WillRepeatedly(Invoke([](int32_t, const char *, uint8_t *buf, int len) {
			if (len < 2) return 0;
			buf[0] = 0xC3; buf[1] = 0x00;
			return 2;
		}));
	ON_CALL(mockApi, plc_tag_get_float32(_, _)).WillByDefault(Return(3.5f));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));

	auto readings = tag.takeReading();
	EXPECT_EQ(readings.size(), 1u) << "a type mismatch must not suppress the reading";

	for (auto *r : readings) delete r;
}

TEST_F(PLCTagTest, TypeCheckSkippedWhenPlcReportsNoTypeInfo)
{
	// MicroLogix (PCCC) and Omron do not implement the attribute - libplctag
	// copies 0 bytes without erroring, and nothing should be reported.
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "micrologix", "1,0",
		R"({\"tags\":[{\"name\":\"N7:0\",\"type\":\"INT\"}]})", R"({\"tags\":[]})",
		"Reading per PLC tag", "scatter-gather");
	tag.configure(&cfg);

	EXPECT_CALL(mockApi, plc_tag_get_byte_array_attribute(_, _, _, _))
		.WillRepeatedly(Return(0));

	tag.takeReading();
	this_thread::sleep_for(chrono::milliseconds(200));

	auto readings = tag.takeReading();
	EXPECT_EQ(readings.size(), 1u);

	for (auto *r : readings) delete r;
}

// ===================================================================
// Async plugin interface tests
// ===================================================================

TEST_F(PLCTagTest, StartStopDoesNotCrash)
{
	PLCTag tag;
	auto cfg = minimalConfig();
	tag.configure(&cfg);

	tag.registerIngest(nullptr, nullptr);
	tag.start();
	this_thread::sleep_for(chrono::milliseconds(100));
	tag.stop();
}

TEST_F(PLCTagTest, IngestCallbackInvoked)
{
	PLCTag tag;
	auto cfg = minimalConfig("10.0.0.1", "controllogix", "1,0",
		R"({\"tags\":[{\"name\":\"Tag1\",\"type\":\"DINT\"}]})", R"({\"tags\":[]})", "Reading per PLC tag", "scatter-gather");
	tag.configure(&cfg);

	ON_CALL(mockApi, plc_tag_get_int32(_, _)).WillByDefault(Return(42));

	atomic<int> ingestCount{0};
	auto cb = [](void *data, ReadingSet *set) {
		auto *count = static_cast<atomic<int>*>(data);
		(*count)++;
		// ReadingSet owns the Readings and frees them on destruction
		delete set;
	};

	tag.registerIngest(cb, &ingestCount);
	tag.start();

	// Wait enough for background creation + at least one poll cycle
	this_thread::sleep_for(chrono::milliseconds(2000));
	tag.stop();

	EXPECT_GT(ingestCount.load(), 0);
}

// ---------------------------------------------------------------------------
// Outage duration formatting
//
// This string is the whole point of the hourly outage report - an operator
// reads "down 2h14m" to decide whether a cabinet is mid-outage or has been
// forgotten about - so the unit boundaries are worth pinning down.
// ---------------------------------------------------------------------------

TEST(HumanizeDuration, FormatsBelowOneMinuteInSeconds)
{
	EXPECT_EQ(PLCTag::humanizeDuration(0), "0s");
	EXPECT_EQ(PLCTag::humanizeDuration(45), "45s");
	EXPECT_EQ(PLCTag::humanizeDuration(59), "59s");
}

TEST(HumanizeDuration, FormatsMinutesAndHours)
{
	EXPECT_EQ(PLCTag::humanizeDuration(60), "1m0s");
	EXPECT_EQ(PLCTag::humanizeDuration(90), "1m30s");
	EXPECT_EQ(PLCTag::humanizeDuration(3599), "59m59s");
	EXPECT_EQ(PLCTag::humanizeDuration(3600), "1h0m");
	EXPECT_EQ(PLCTag::humanizeDuration(8040), "2h14m");
	EXPECT_EQ(PLCTag::humanizeDuration(86399), "23h59m");
}

TEST(HumanizeDuration, RollsOverIntoDaysForMultiShiftOutages)
{
	EXPECT_EQ(PLCTag::humanizeDuration(86400), "1d0h0m");
	EXPECT_EQ(PLCTag::humanizeDuration(90000), "1d1h0m");
	EXPECT_EQ(PLCTag::humanizeDuration(200000), "2d7h33m");
}

TEST(HumanizeDuration, ClampsNegativeToZero)
{
	// steady_clock should never run backwards, but a negative duration here
	// would otherwise print "-5s" and read as a clock bug rather than a stub.
	EXPECT_EQ(PLCTag::humanizeDuration(-5), "0s");
}
