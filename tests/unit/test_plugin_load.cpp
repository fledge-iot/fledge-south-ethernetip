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
 * Plugin load tests - verify that libethernetip.so can be loaded by
 * the dynamic linker and that all required Fledge south plugin symbols
 * are present and callable.
 *
 * This is the same sequence Fledge's PluginManager performs when it
 * loads a south plugin .so at runtime.
 */

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <string>
#include <vector>
#include <cstring>

#include <plugin_api.h>
#include <config_category.h>
#include <reading.h>
#include <version.h>

// Fledge south plugin callback types
typedef void (*INGEST_CB2)(void *, std::vector<Reading *>*);

class PluginLoadTest : public ::testing::Test {
protected:
	void *handle = nullptr;

	// Path to the built shared library - set via cmake define
	std::string soPath()
	{
#ifdef PLUGIN_SO_PATH
		return PLUGIN_SO_PATH;
#else
		// Fallback: look relative to test binary
		return "./libethernetip.so";
#endif
	}

	void SetUp() override
	{
		handle = dlopen(soPath().c_str(), RTLD_NOW);
		if (!handle)
		{
			FAIL() << "dlopen failed: " << dlerror()
			       << "\n  path: " << soPath();
		}
	}

	void TearDown() override
	{
		if (handle)
			dlclose(handle);
	}

	// Resolve a symbol or fail the test
	void *resolve(const char *name)
	{
		dlerror(); // clear
		void *sym = dlsym(handle, name);
		const char *err = dlerror();
		EXPECT_EQ(err, nullptr)
			<< "dlsym(\"" << name << "\") failed: " << (err ? err : "");
		EXPECT_NE(sym, nullptr)
			<< "Symbol \"" << name << "\" resolved to nullptr";
		return sym;
	}
};

// ===========================================================================
// Symbol presence tests
// ===========================================================================

TEST_F(PluginLoadTest, LibraryLoads)
{
	// SetUp already opened the library - if we get here, it loaded
	SUCCEED();
}

TEST_F(PluginLoadTest, HasPluginInfo)
{
	resolve("plugin_info");
}

TEST_F(PluginLoadTest, HasPluginInit)
{
	resolve("plugin_init");
}

TEST_F(PluginLoadTest, HasPluginStart)
{
	resolve("plugin_start");
}

TEST_F(PluginLoadTest, HasPluginPoll)
{
	resolve("plugin_poll");
}

TEST_F(PluginLoadTest, HasPluginReconfigure)
{
	resolve("plugin_reconfigure");
}

TEST_F(PluginLoadTest, HasPluginShutdown)
{
	resolve("plugin_shutdown");
}

TEST_F(PluginLoadTest, HasPluginWrite)
{
	resolve("plugin_write");
}

TEST_F(PluginLoadTest, HasPluginRegisterIngest)
{
	resolve("plugin_register_ingest");
}

// ===========================================================================
// plugin_info() validation
// ===========================================================================

TEST_F(PluginLoadTest, PluginInfoReturnsValidStruct)
{
	typedef PLUGIN_INFORMATION *(*InfoFn)();
	auto fn = (InfoFn)resolve("plugin_info");
	ASSERT_NE(fn, nullptr);

	PLUGIN_INFORMATION *info = fn();
	ASSERT_NE(info, nullptr);

	// Name
	ASSERT_NE(info->name, nullptr);
	EXPECT_STREQ(info->name, "ethernetip");

	// Version - must come from the generated version.h, not a hardcoded string
	ASSERT_NE(info->version, nullptr);
	EXPECT_GT(strlen(info->version), 0u);
	EXPECT_STREQ(info->version, PLUGIN_BUILD_ID);
	EXPECT_EQ(std::string(info->version).find(VERSION), 0u)
		<< "Reported version '" << info->version
		<< "' should start with the VERSION file contents '" << VERSION << "'";

	// Type
	EXPECT_STREQ(info->type, PLUGIN_TYPE_SOUTH);

	// Flags - must include SP_ASYNC and SP_CONTROL
	EXPECT_TRUE(info->options & SP_ASYNC)
		<< "Plugin should have SP_ASYNC flag";
	EXPECT_TRUE(info->options & SP_CONTROL)
		<< "Plugin should have SP_CONTROL flag";

	// Interface version - should be non-empty
	ASSERT_NE(info->interface, nullptr);
	EXPECT_GT(strlen(info->interface), 0u);

	// Default config - should be non-null JSON
	ASSERT_NE(info->config, nullptr);
	EXPECT_GT(strlen(info->config), 10u)
		<< "Default config seems too short";
}

// ===========================================================================
// Default config JSON is parseable
// ===========================================================================

TEST_F(PluginLoadTest, DefaultConfigIsValidJson)
{
	typedef PLUGIN_INFORMATION *(*InfoFn)();
	auto fn = (InfoFn)resolve("plugin_info");
	ASSERT_NE(fn, nullptr);

	PLUGIN_INFORMATION *info = fn();
	ASSERT_NE(info, nullptr);
	ASSERT_NE(info->config, nullptr);

	// ConfigCategory constructor will throw if JSON is malformed
	EXPECT_NO_THROW({
		ConfigCategory cfg("test_defaults", info->config);
	}) << "Default config JSON failed to parse";
}

TEST_F(PluginLoadTest, DefaultConfigHasRequiredKeys)
{
	typedef PLUGIN_INFORMATION *(*InfoFn)();
	auto fn = (InfoFn)resolve("plugin_info");
	PLUGIN_INFORMATION *info = fn();

	ConfigCategory cfg("test_defaults", info->config);

	// Every south plugin config should have these
	EXPECT_TRUE(cfg.itemExists("plugin"));
	EXPECT_TRUE(cfg.itemExists("asset"));
	EXPECT_TRUE(cfg.itemExists("address"));
	EXPECT_TRUE(cfg.itemExists("plc"));
	EXPECT_TRUE(cfg.itemExists("timeout"));
	EXPECT_TRUE(cfg.itemExists("readmap"));
	EXPECT_TRUE(cfg.itemExists("writemap"));
	EXPECT_TRUE(cfg.itemExists("readingStructure"));
	EXPECT_TRUE(cfg.itemExists("readMode"));
	EXPECT_TRUE(cfg.itemExists("debugLevel"));
	EXPECT_TRUE(cfg.itemExists("statsInterval"));
	EXPECT_TRUE(cfg.itemExists("pollInterval"));

	// A new instance must bundle a cycle's tags into one reading per asset.
	// The per-tag default emitted one single-datapoint reading per tag, which
	// stores as one row per datapoint instead of one row per scan.
	EXPECT_EQ(cfg.getDefault("readingStructure"), "Reading per asset");
}

TEST_F(PluginLoadTest, DefaultConfigPluginNameMatches)
{
	typedef PLUGIN_INFORMATION *(*InfoFn)();
	auto fn = (InfoFn)resolve("plugin_info");
	PLUGIN_INFORMATION *info = fn();

	ConfigCategory cfg("test_defaults", info->config);
	// Default config uses "default" not "value", so use getDefault()
	EXPECT_EQ(cfg.getDefault("plugin"), "ethernetip");
}

// ===========================================================================
// plugin_init / plugin_shutdown round-trip
// ===========================================================================

TEST_F(PluginLoadTest, InitAndShutdownDoNotCrash)
{
	typedef PLUGIN_INFORMATION *(*InfoFn)();
	typedef PLUGIN_HANDLE (*InitFn)(ConfigCategory *);
	typedef void (*ShutdownFn)(PLUGIN_HANDLE);

	auto infoFn     = (InfoFn)resolve("plugin_info");
	auto initFn     = (InitFn)resolve("plugin_init");
	auto shutdownFn = (ShutdownFn)resolve("plugin_shutdown");
	ASSERT_NE(initFn, nullptr);
	ASSERT_NE(shutdownFn, nullptr);

	PLUGIN_INFORMATION *info = infoFn();
	ConfigCategory cfg("load_test", info->config);

	// plugin_init should return a non-null handle
	PLUGIN_HANDLE pluginHandle = nullptr;
	EXPECT_NO_THROW({
		pluginHandle = initFn(&cfg);
	}) << "plugin_init threw an exception";
	ASSERT_NE(pluginHandle, nullptr);

	// plugin_shutdown should not crash
	EXPECT_NO_THROW({
		shutdownFn(pluginHandle);
	}) << "plugin_shutdown threw an exception";
}

// ===========================================================================
// plugin_start / plugin_register_ingest resolve correctly
// ===========================================================================

TEST_F(PluginLoadTest, StartAndRegisterIngestCallable)
{
	typedef PLUGIN_INFORMATION *(*InfoFn)();
	typedef PLUGIN_HANDLE (*InitFn)(ConfigCategory *);
	typedef void (*StartFn)(PLUGIN_HANDLE);
	typedef void (*RegisterFn)(PLUGIN_HANDLE, INGEST_CB2, void *);
	typedef void (*ShutdownFn)(PLUGIN_HANDLE);

	auto infoFn     = (InfoFn)resolve("plugin_info");
	auto initFn     = (InitFn)resolve("plugin_init");
	auto startFn    = (StartFn)resolve("plugin_start");
	auto registerFn = (RegisterFn)resolve("plugin_register_ingest");
	auto shutdownFn = (ShutdownFn)resolve("plugin_shutdown");

	PLUGIN_INFORMATION *info = infoFn();
	ConfigCategory cfg("lifecycle_test", info->config);
	PLUGIN_HANDLE pluginHandle = initFn(&cfg);
	ASSERT_NE(pluginHandle, nullptr);

	// Register a no-op ingest callback
	auto noop = [](void *, std::vector<Reading *> *readings) {
		for (auto *r : *readings) delete r;
		delete readings;
	};
	EXPECT_NO_THROW(registerFn(pluginHandle, noop, nullptr));

	// Start should launch the poll thread
	EXPECT_NO_THROW(startFn(pluginHandle));

	// Brief pause then shutdown
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	EXPECT_NO_THROW(shutdownFn(pluginHandle));
}
