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
 * Link-time mock implementation of libplctag C API.
 *
 * Each C function delegates to MockPlcTagAPI::instance.  If no mock is
 * installed (instance == nullptr) the functions return safe defaults so
 * that constructor / destructor code running outside a test case does not
 * crash.
 */

#include "mock_libplctag.h"

#include <cstring>
#include <libplctag.h>

MockPlcTagAPI *MockPlcTagAPI::instance = nullptr;

// Helper: return default if no mock is installed
#define MOCK_OR_DEFAULT(fn, default_val, ...) \
	return MockPlcTagAPI::instance ? MockPlcTagAPI::instance->fn(__VA_ARGS__) : (default_val)

extern "C" {

int32_t plc_tag_create(const char *attrib_str, int timeout)
{
	MOCK_OR_DEFAULT(plc_tag_create, 1, attrib_str, timeout);
}

int plc_tag_destroy(int32_t tag)
{
	MOCK_OR_DEFAULT(plc_tag_destroy, PLCTAG_STATUS_OK, tag);
}

int plc_tag_abort(int32_t tag)
{
	MOCK_OR_DEFAULT(plc_tag_abort, PLCTAG_STATUS_OK, tag);
}

int plc_tag_status(int32_t tag)
{
	MOCK_OR_DEFAULT(plc_tag_status, PLCTAG_STATUS_OK, tag);
}

int plc_tag_read(int32_t tag, int timeout)
{
	MOCK_OR_DEFAULT(plc_tag_read, PLCTAG_STATUS_OK, tag, timeout);
}

int plc_tag_write(int32_t tag, int timeout)
{
	MOCK_OR_DEFAULT(plc_tag_write, PLCTAG_STATUS_OK, tag, timeout);
}

int plc_tag_register_logger(void (*cb)(int32_t, int, const char *))
{
	MOCK_OR_DEFAULT(plc_tag_register_logger, PLCTAG_STATUS_OK, cb);
}

const char *plc_tag_decode_error(int err)
{
	if (MockPlcTagAPI::instance)
		return MockPlcTagAPI::instance->plc_tag_decode_error(err);
	return "MOCK_ERROR";
}

// --- Getters ---

int plc_tag_get_bit(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_bit, 0, tag, offset);
}

uint8_t plc_tag_get_uint8(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_uint8, 0, tag, offset);
}

uint16_t plc_tag_get_uint16(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_uint16, 0, tag, offset);
}

uint32_t plc_tag_get_uint32(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_uint32, 0, tag, offset);
}

uint64_t plc_tag_get_uint64(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_uint64, 0, tag, offset);
}

int8_t plc_tag_get_int8(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_int8, 0, tag, offset);
}

int16_t plc_tag_get_int16(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_int16, 0, tag, offset);
}

int32_t plc_tag_get_int32(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_int32, 0, tag, offset);
}

int64_t plc_tag_get_int64(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_int64, 0, tag, offset);
}

float plc_tag_get_float32(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_float32, 0.0f, tag, offset);
}

double plc_tag_get_float64(int32_t tag, int offset)
{
	MOCK_OR_DEFAULT(plc_tag_get_float64, 0.0, tag, offset);
}

int plc_tag_get_string(int32_t tag, int offset, char *buf, int len)
{
	if (MockPlcTagAPI::instance)
		return MockPlcTagAPI::instance->plc_tag_get_string(tag, offset, buf, len);
	if (buf && len > 0) buf[0] = '\0';
	return PLCTAG_STATUS_OK;
}

// --- Setters ---

int plc_tag_set_bit(int32_t tag, int offset, int val)
{
	MOCK_OR_DEFAULT(plc_tag_set_bit, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_uint8(int32_t tag, int offset, uint8_t val)
{
	MOCK_OR_DEFAULT(plc_tag_set_uint8, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_uint16(int32_t tag, int offset, uint16_t val)
{
	MOCK_OR_DEFAULT(plc_tag_set_uint16, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_uint32(int32_t tag, int offset, uint32_t val)
{
	MOCK_OR_DEFAULT(plc_tag_set_uint32, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_uint64(int32_t tag, int offset, uint64_t val)
{
	MOCK_OR_DEFAULT(plc_tag_set_uint64, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_int8(int32_t tag, int offset, int8_t val)
{
	MOCK_OR_DEFAULT(plc_tag_set_int8, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_int16(int32_t tag, int offset, int16_t val)
{
	MOCK_OR_DEFAULT(plc_tag_set_int16, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_int32(int32_t tag, int offset, int32_t val)
{
	MOCK_OR_DEFAULT(plc_tag_set_int32, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_int64(int32_t tag, int offset, int64_t val)
{
	MOCK_OR_DEFAULT(plc_tag_set_int64, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_float32(int32_t tag, int offset, float val)
{
	MOCK_OR_DEFAULT(plc_tag_set_float32, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_float64(int32_t tag, int offset, double val)
{
	MOCK_OR_DEFAULT(plc_tag_set_float64, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_set_string(int32_t tag, int offset, const char *val)
{
	MOCK_OR_DEFAULT(plc_tag_set_string, PLCTAG_STATUS_OK, tag, offset, val);
}

int plc_tag_get_int_attribute(int32_t tag, const char *name, int default_value)
{
	MOCK_OR_DEFAULT(plc_tag_get_int_attribute, default_value, tag, name, default_value);
}

int plc_tag_get_byte_array_attribute(int32_t tag, const char *name,
                                     uint8_t *buffer, int buffer_length)
{
	// Default: report no type information, as PLC families that do not support
	// the attribute do (0 bytes copied, no error).
	MOCK_OR_DEFAULT(plc_tag_get_byte_array_attribute, 0, tag, name, buffer, buffer_length);
}

} // extern "C"
