/*
 * Fledge EtherNet/IP south plugin tests.
 *
 * Copyright (c) 2026 Industrial Intelligence Group, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Industrial Intelligence Group
 */
#ifndef _MOCK_LIBPLCTAG_H
#define _MOCK_LIBPLCTAG_H

/**
 * Link-time mock for the libplctag C API.
 *
 * A global MockPlcTagAPI instance receives all calls made by production code
 * to plc_tag_* functions.  Tests install expectations on this instance via
 * GMock's EXPECT_CALL / ON_CALL macros.
 *
 * Usage:
 *   // In test fixture SetUp():
 *   MockPlcTagAPI::instance = &mockApi;
 *
 *   // Set expectations:
 *   ON_CALL(mockApi, plc_tag_create(_, _)).WillByDefault(Return(1));
 *
 *   // In TearDown():
 *   MockPlcTagAPI::instance = nullptr;
 */

#include <cstdint>
#include <gmock/gmock.h>

class MockPlcTagAPI {
public:
	// Singleton pointer - tests set this before running
	static MockPlcTagAPI *instance;

	// Lifecycle
	MOCK_METHOD(int32_t, plc_tag_create, (const char *attrib_str, int timeout));
	MOCK_METHOD(int, plc_tag_destroy, (int32_t tag));
	MOCK_METHOD(int, plc_tag_abort, (int32_t tag));

	// Status / I/O
	MOCK_METHOD(int, plc_tag_status, (int32_t tag));
	MOCK_METHOD(int, plc_tag_read, (int32_t tag, int timeout));
	MOCK_METHOD(int, plc_tag_write, (int32_t tag, int timeout));

	// Logger
	MOCK_METHOD(int, plc_tag_register_logger,
		(void (*cb)(int32_t, int, const char *)));

	// Decode
	MOCK_METHOD(const char *, plc_tag_decode_error, (int err));

	// Getters
	MOCK_METHOD(int, plc_tag_get_bit, (int32_t tag, int offset));
	MOCK_METHOD(uint8_t, plc_tag_get_uint8, (int32_t tag, int offset));
	MOCK_METHOD(uint16_t, plc_tag_get_uint16, (int32_t tag, int offset));
	MOCK_METHOD(uint32_t, plc_tag_get_uint32, (int32_t tag, int offset));
	MOCK_METHOD(uint64_t, plc_tag_get_uint64, (int32_t tag, int offset));
	MOCK_METHOD(int8_t, plc_tag_get_int8, (int32_t tag, int offset));
	MOCK_METHOD(int16_t, plc_tag_get_int16, (int32_t tag, int offset));
	MOCK_METHOD(int32_t, plc_tag_get_int32, (int32_t tag, int offset));
	MOCK_METHOD(int64_t, plc_tag_get_int64, (int32_t tag, int offset));
	MOCK_METHOD(float, plc_tag_get_float32, (int32_t tag, int offset));
	MOCK_METHOD(double, plc_tag_get_float64, (int32_t tag, int offset));
	MOCK_METHOD(int, plc_tag_get_string, (int32_t tag, int offset, char *buf, int len));

	// Setters
	MOCK_METHOD(int, plc_tag_set_bit, (int32_t tag, int offset, int val));
	MOCK_METHOD(int, plc_tag_set_uint8, (int32_t tag, int offset, uint8_t val));
	MOCK_METHOD(int, plc_tag_set_uint16, (int32_t tag, int offset, uint16_t val));
	MOCK_METHOD(int, plc_tag_set_uint32, (int32_t tag, int offset, uint32_t val));
	MOCK_METHOD(int, plc_tag_set_uint64, (int32_t tag, int offset, uint64_t val));
	MOCK_METHOD(int, plc_tag_set_int8, (int32_t tag, int offset, int8_t val));
	MOCK_METHOD(int, plc_tag_set_int16, (int32_t tag, int offset, int16_t val));
	MOCK_METHOD(int, plc_tag_set_int32, (int32_t tag, int offset, int32_t val));
	MOCK_METHOD(int, plc_tag_set_int64, (int32_t tag, int offset, int64_t val));
	MOCK_METHOD(int, plc_tag_set_float32, (int32_t tag, int offset, float val));
	MOCK_METHOD(int, plc_tag_set_float64, (int32_t tag, int offset, double val));
	MOCK_METHOD(int, plc_tag_set_string, (int32_t tag, int offset, const char *val));

	// Attributes
	MOCK_METHOD(int, plc_tag_get_int_attribute,
		(int32_t tag, const char *name, int default_value));
	MOCK_METHOD(int, plc_tag_get_byte_array_attribute,
		(int32_t tag, const char *name, uint8_t *buffer, int buffer_length));
};

#endif // _MOCK_LIBPLCTAG_H
