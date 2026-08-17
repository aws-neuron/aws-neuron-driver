// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019-2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */
#ifndef __FWIO_H__
#define __FWIO_H__

#include <linux/types.h>

union fw_io_request_hdr {
	struct {
		u8 sequence_number; // sequence number to be copied in the next response.
		u8 command_id; // command to hw.
		u16 size; // request size in bytes including the header.
		u32 crc32; // crc32 of the entire request, crc32 must be set to 0 before calculating
	} hdr;
	struct {
		u32 dw0; // bytes 0-3: sequence_number, command_id, size
		u32 dw1; // bytes 4-7: crc32
	} reg;
};

// Note: Firmware updated to include crc32 field in response header, but 
// to maintain backward compatibility, keeping original response header
// struct and adding crc32 field to new header.

// Response header for legacy protocol
// Used by fw_io_execute_request() for legacy commands
union fw_io_response_hdr {
	struct {
		u8 sequence_number; // request sequence number
		u8 error_code; // 0 means request was successfully completed
		u16 size; // response size in bytes including this header
	} hdr;
	u32 dw0; // bytes 0-3: sequence_number, error_code, size
};

// Response header for new protocol
// Used by fw_io_execute_request_new() for new commands
union fw_io_response_hdr_new {
	struct {
		u8 sequence_number; // request sequence number
		u8 error_code; // 0 means request was successfully completed
		u16 size; // response size in bytes including this header
		u32 crc32;
	} hdr;
	struct {
		u32 dw0; // bytes 0-3: sequence_number, error_code, size
		u32 dw1; // bytes 4-7: crc32
	} reg;
};

struct fw_io_request {
	union fw_io_request_hdr request_hdr;
	u8 data[];
};

struct fw_io_response {
	union fw_io_response_hdr response_hdr;
	u8 data[];
};

struct fw_io_response_new {
	union fw_io_response_hdr_new response_hdr;
	u8 data[];
};

union fw_io_req_perfprofile_data {
	struct {
		uint32_t reserved;
		uint8_t profile;
		uint8_t voltage_margin;
		uint8_t frequency_index;
		uint8_t ocw_index;
	} rec;
	uint32_t raw[2];
};

enum fw_io_get_data_type {
	FW_IO_GET_DATA_PERF_PROFILE = 0x01,
	FW_IO_GET_DATA_AVAILABLE_PROFILES = 0x02,
	FW_IO_GET_DATA_BAR_INFO = 0x10,
};

struct fw_io_get_data_request {
	uint8_t type;
	union {
		struct {
			uint16_t operation;
		} available_profiles;
		struct {
			uint8_t query_type;
		} bar_info;
	};
} __packed;

struct fw_io_get_perfprofile_response {
	uint8_t reserved[4];
	uint8_t profile;
	uint8_t voltage_margin;
	uint8_t frequency;
	uint8_t ocw;
};

struct fw_io_get_available_profiles_response {
	uint8_t num_profiles;
	uint8_t profiles_bitmap[32];
};

#define NEURON_MAX_SWITCH_BARS 16

struct fw_io_bar_entry_response {
	uint8_t bar_type;
	uint8_t reserved[3];
	uint32_t bar_address;
} __packed;

struct fw_io_bar_entry {
	uint8_t bar_type;
	uint64_t bar_address;
};


enum fw_io_get_available_profiles_feature {
	FW_IO_AVAILABLE_PERF_PROFILES_ALL = 0,
	FW_IO_AVAILABLE_PERF_PROFILES_HBM_7200 = 5
};

// Feature bitmap for FW_IO_CMD_SET_FEATURE
// Each bit represents a different feature that can be enabled/disabled
enum fw_io_feature_bits {
	FW_IO_FEATURE_THROTTLING_NOTIFICATIONS = (0x1 << 0),  // bit 0: enable throttling notifications
};

/*
 * Note:
 * GET_DATA can retrieve information such as current power profile,
 * but there's no corresponding GET_FEATURE to read current feature bitmap.
 * This limitation affects fw_io_enable_throttling_notifications (see note).
 */ 
enum {
	FW_IO_CMD_READ = 1, // read a register value
	FW_IO_CMD_POST_TO_CW = 2, // post given blob as metrics to CloudWatch
	FW_IO_CMD_SET_POWER_PROFILE = 3, // set power profile
	FW_IO_CMD_GET_DATA = 4, // get various FW data (see fw_io_get_data_request)
	FW_IO_CMD_SET_FEATURE = 5 // set feature bitmap
};

enum {
	FW_IO_SUCCESS = 0, // completed successfully
	FW_IO_FAIL, // request failed, no further information should be returned
	FW_IO_UNKNOWN_COMMAND // request failed because command is not supported
};

// Bitmap of PIR reset types to be written to FW_IO_REG_RESET_OFFSET
enum {
	FW_IO_RESET_TYPE_DEVICE = 1,
	FW_IO_RESET_TYPE_TPB = 2  // Requires FW_IO_REG_RESET_TPB_MAP_LO_OFFSET to be populated with a tpb map prior to use
};

// offsets in MISC RAM for FWIO
enum {
	FW_IO_REG_DEVICE_ID_OFFSET = 0x24,
	FW_IO_REG_SERVER_RACK_ID_OFFSET = 0x70,

	// MISC RAM register for API version
	//   - This register is used to determine the API version of the firmware.
	//   - The value of this register is used to determine the offset of other registers.
	FW_IO_REG_API_VERSION_OFFSET = 0x00,

	FW_IO_REG_HEALTH_CHECK_STATUS_OFFSET = 0x04, // reg 1
	FW_IO_REG_HEALTH_CHECK_SEQ_OFFSET = 0x0C, // reg 3

	// MISC RAM instance/partition size info
	// (0:5) instance size, 16:30 partition size, 31 partition size valid
	FW_IO_REG_INSTANCE_PARTITION_SZ_OFFSET = 0x30,

	// MISC RAM slots for serial number for V2
	//   - The lower 32 bits and the upper 32 bits together represent the 64-bit serial number.
	FW_IO_REG_SERIAL_NUMBER_LO_OFFSET = 0x38, // 14 * 4 bytes
	FW_IO_REG_SERIAL_NUMBER_HI_OFFSET = 0x3c, // 15 * 4 bytes

	// MISC RAM slots for ECC error counters for V2
	//   - ECC counters for V2 which are currently being placed in MISC RAM register 16, 17 and 18 by the device. 
	//   - The upper 16 bits of each register represent corrected errors, and the lower 16 bits represent uncorrected errors.
	FW_IO_REG_SRAM_ECC_OFFSET = 0x40, // 16 * 4 bytes
	FW_IO_REG_HBM0_ECC_OFFSET = 0x44, // 17 * 4 bytes
	FW_IO_REG_HBM1_ECC_OFFSET = 0x48, // 18 * 4 bytes
	FW_IO_REG_HBM2_ECC_OFFSET = 0x4c, // 19 * 4 bytes
	FW_IO_REG_HBM3_ECC_OFFSET = 0x50, // 20 * 4 bytes

	// MISC RAM slots for power utilization.
	//   - The lower 16 bits contain power utilization as a percentage of max with baseline backed out
	//   - The upper 16 bits contain a counter indicating the sample number, used to detect cases in
	//     which we are sampling faster than the firmware is providing new data
	// Note that this is expressed as a single offset, as we would prefer to read it all in one go for both
	// performance and atomicity.
	//
	// All devices will have the D0 offset.  Devices with two dice will also have the D1 offset.
	FW_IO_REG_POWER_UTIL_D0_OFFSET = 0x54, // 21 * 4 bytes
	FW_IO_REG_POWER_UTIL_D1_OFFSET = 0x58, // 22 * 4 bytes
	FW_IO_REG_HBM_REPAIR_STATE_OFFSET = 0x64, // 25 * 4 bytes
	FW_IO_REG_FW_BUILD_OFFSET = 0x74, // 29 * 4 bytes
											  //

	FW_IO_REG_RESERVATION_ID_HI = 0x80,	// 32 * 4 bytes
	FW_IO_REG_RESERVATION_ID_LO = 0x84,	// 33 * 4 bytes

	FW_IO_REG_RUNTIME_RESERVED0 = 0xC0, // 0xC0 to 0xF0

	FW_IO_REG_DATA_OFFSET = 0x100, // 0x100 to 0x17F, 128 bytes
	FW_IO_REG_LH_NEIGHBOR_SERNUM_HI = 0x180, // LH/RH neighbors
	FW_IO_REG_LH_NEIGHBOR_SERNUM_LO = 0x184, 
	FW_IO_REG_RH_NEIGHBOR_SERNUM_HI = 0x188, 
	FW_IO_REG_RH_NEIGHBOR_SERNUM_LO = 0x18c, 
	FW_IO_REG_POD_ELECTION_STS = 0x190, // Election status + node id
	FW_IO_REG_POD_SERNUM_HI = 0x194, // Pod serial number
	FW_IO_REG_POD_SERNUM_LO = 0x198, 
	FW_IO_REG_RUNTIME_RESERVED1  = 0x1a0, // 0x1a0 to 1d0
	
	FW_IO_REG_RESET_TPB_MAP_HI_OFFSET = 0x1d4,
	FW_IO_REG_RESET_TPB_MAP_LO_OFFSET = 0x1d8,
	FW_IO_REG_RESET_OFFSET = 0x1ec,
	FW_IO_REG_REQUEST_BASE_ADDR_LOW_OFFSET = 0x1f4,
	FW_IO_REG_REQUEST_BASE_ADDR_HIG_OFFSET = 0x1f0,
	FW_IO_REG_RESPONSE_BASE_ADDR_LOW_OFFSET = 0x1fc,
	FW_IO_REG_RESPONSE_BASE_ADDR_HIGH_OFFSET = 0x1f8,
	FW_IO_REG_TRIGGER_INT_NOSEC_OFFSET = 0x800,
	FW_IO_REG_ACK_OFFSET = 0xf0,
};

// Instance/partition register field decode
//
#define _REG_INSTPARTSZ_INSTBITS	6
#define _REG_INSTPARTSZ_INSTSHIFT 	0
#define _REG_INSTPARTSZ_INSTMASK	((1 << _REG_INSTPARTSZ_INSTBITS)-1)
#define _REG_INSTPARTSZ_INST(inst)	(((inst) >> _REG_INSTPARTSZ_INSTSHIFT) & _REG_INSTPARTSZ_INSTMASK)

#define _REG_INSTPARTSZ_PARTBITS	15
#define _REG_INSTPARTSZ_PARTSHIFT 	16
#define _REG_INSTPARTSZ_PARTMASK	((1 << _REG_INSTPARTSZ_PARTBITS)-1)
#define _REG_INSTPARTSZ_PART(part)	(((part) >> _REG_INSTPARTSZ_PARTSHIFT) & _REG_INSTPARTSZ_PARTMASK)

#define _REG_INSTPARTSZ_VALBITS		1
#define _REG_INSTPARTSZ_VALSHIFT 	31
#define _REG_INSTPARTSZ_VALMASK		((1 << _REG_INSTPARTSZ_VALBITS)-1)
#define _REG_INSTPARTSZ_VAL(val)	(((val) >> _REG_INSTPARTSZ_VALSHIFT) & _REG_INSTPARTSZ_VALMASK)

// server info register field decode
//
#define _REG_SERVERINFO_SERVERBITS		15
#define _REG_SERVERINFO_SERVERSHIFT 	0
#define _REG_SERVERINFO_SERVERMASK		((1 << _REG_SERVERINFO_SERVERBITS)-1)
#define _REG_SERVERINFO_SERVER(serv)	(((serv) >> _REG_SERVERINFO_SERVERSHIFT) & _REG_SERVERINFO_SERVERMASK)

#define _REG_SERVERINFO_SVALIDBITS		1 
#define _REG_SERVERINFO_SVALIDSHIFT 	15
#define _REG_SERVERINFO_SVALIDMASK		((1 << _REG_SERVERINFO_SVALIDBITS)-1)
#define _REG_SERVERINFO_SVALID(sval)	(((sval) >> _REG_SERVERINFO_SVALIDSHIFT) & _REG_SERVERINFO_SVALIDMASK)

#define _REG_SERVERINFO_RACKBITS		15
#define _REG_SERVERINFO_RACKSHIFT 		16
#define _REG_SERVERINFO_RACKMASK		((1 << _REG_SERVERINFO_RACKBITS)-1)
#define _REG_SERVERINFO_RACK(rack)		(((rack) >> _REG_SERVERINFO_RACKSHIFT) & _REG_SERVERINFO_RACKMASK)

#define _REG_SERVERINFO_RVALIDBITS		1 
#define _REG_SERVERINFO_RVALIDSHIFT 	31
#define _REG_SERVERINFO_RVALIDMASK		((1 << _REG_SERVERINFO_RVALIDBITS)-1)
#define _REG_SERVERINFO_RVALID(rval)	(((rval) >> _REG_SERVERINFO_RVALIDSHIFT) & _REG_SERVERINFO_RVALIDMASK)

// healthcheck regs - Reg 3 field extraction
#define _REG_HEALTHCHECK_HEARTBEATBITS	8
#define _REG_HEALTHCHECK_HEARTBEATSHIFT 12
#define _REG_HEALTHCHECK_HEARTBEATMASK	((1 << _REG_HEALTHCHECK_HEARTBEATBITS)-1)
#define _REG_HEALTHCHECK_HEARTBEAT(val)	(((val) >> _REG_HEALTHCHECK_HEARTBEATSHIFT) & _REG_HEALTHCHECK_HEARTBEATMASK)


//
#define FW_IO_REG_METRIC_BUF_SZ 128

struct fw_io_ctx {
	void __iomem *bar0;
	u8 next_seq_num;
	struct fw_io_request *request;
	struct fw_io_response *response;
	u64 request_addr;
	u64 response_addr;
	u32 request_response_size; // for simplicity always use the same buffer size for request and response
	u64 fw_io_err_count;
	struct mutex lock;
};

#define UINT64_LOW(x) ((u32)(((u64)(x)) & 0xffffffffULL))
#define UINT64_HIGH(x) ((u32)((x) >> 32))

#define FW_IO_CMD_MAX 6

// Wait up to 30 seconds in worst case.
// Hardware can in some cases take longer to come out of reset but for some reads
// (like reading device ID before creating the device) we cannot wait too long
// because it's confusing if driver load appears to hang and kernel may complain
#define FW_IO_RD_TIMEOUT (1000 * 1000 * 1)
#define FW_IO_RD_RETRY   30

// max number of registers can be read in single function call
#define FW_IO_MAX_READLESS_READ_REGISTER_COUNT 100


/**
 * fw_io_register_read_region - Read a BAR region
 * 
 * @param ctx 
 * @param region_ptr 
 * @param region_size 
 * @param device_physical_address 
 * 
 * @return int: 0 on success; -1 on failure
 */
int fw_io_register_read_region(struct fw_io_ctx *ctx, void __iomem *region_ptr,
				      u64 region_size, u64 device_physical_address);

/**
 * fw_io_read_csr_array_direct
 * 
 * @param addrs: an array of register addresses to read
 * @param values: read values stored here
 * @param num_csrs: the number of CSRs to read
 * @param operational: true if the read expects the device to be in operational state
 * 
 * @return int: 0 on success; -1 on failure
 */
int fw_io_read_csr_array_direct(void **addrs, u32 *values, u32 num_csrs, bool operational);

/**
 * fw_io_read_csr_array_readless
 * 
 * @param ptrs: an array of register addresses to read
 * @param values: read values stored here
 * @param num_csrs: the number of CSRs to read
 * 
 * @return int: 0 on success; -1 on failure
 */
int fw_io_read_csr_array_readless(void **ptrs, u32 *values, u32 num_csrs);

/**
 * fw_io_read_csr_array() - Read CSR(s) and return the value(s).
 *
 * @ptrs: Array of register address to read
 * @values: Read values stored here
 * @num_csrs: Number of CSRs to read
 * @operational: true if the read expects the device to be in operational state
 *
 * Return: 0 if CSR read is successful, a negative error code otherwise.
 */
int fw_io_read_csr_array(void **ptrs, u32 *values, u32 num_csrs, bool operational);

/** Read the list of addresses given in the address list and returns it's values in the value list
 *
 * @param ctx[in]	- FWIO context
 * @param addr_in[in]	- List of registers to read
 * @param values[out]	- Buffer to store results.
 * @param num_req[in]	- Total number of registers in the addr_in
 *
 * @return 0 on success 1 on error
 */
int fw_io_read(struct fw_io_ctx *ctx, u64 addr_in[], u32 val_out[], u32 num_req);


/**
 * fw_io_setup() - Setup new FWIO for given device.
 *
 * @bar0: BAR0 virtual address
 * @bar0_size: Size of BAR0
 * @bar2: BAR2 virtual address
 * @bar2_size: Size of BAR2
 *
 * Return: fwio context on success, NULL on failure.
 */
struct fw_io_ctx *fw_io_setup(void __iomem *bar0, u64 bar0_size,
				  void __iomem *bar2, u64 bar2_size);

/**
 * fw_io_destroy() - Removes previously setup FWIO.
 *
 * @ctx: fwio context
 */
void fw_io_destroy(struct fw_io_ctx *ctx);

/**
 * fw_io_post_metric() - Post given block data as metric to FWIO
 *
 * @ctx: fwio context
 * @data: data to post
 * @size: size of the data
 *
 * Return: 0 if metric is successfully posted, a negative error code otherwise.
 */
int fw_io_post_metric(struct fw_io_ctx *ctx, u8 *data, u32 size);

/**
 * fw_io_post_metric_new() - Post given block data as metric using new framework
 * @param ctx: FWIO context
 * @param data: data to post
 * @param size: size of the data
 * Return: 0 if metric is successfully posted, negative on failure
 */
int fw_io_post_metric_new(struct fw_io_ctx *ctx, u8 *data, u32 size);

/**
 * fw_io_initiate_reset() - Initiate device local reset.
 *
 * @bar0: Device's BAR0 base address
 * @device_reset: True if we are doing a device-level reset
 * @tpb_reset_map_lo: If device_reset is false (tpb reset), bitmap of blocks to reset (bits 0-31)
 *     [7:0]   TPB mask
 *     [15:8]  SDMA group mask
 *     [23:16] TOP_SP mask
 *     [31:24] CC_TOP mask
 * @tpb_reset_map_hi:
 *     [3:0]   Top-Level DMA group mask
 */
void fw_io_initiate_reset(void __iomem *bar0, bool device_reset, u32 tpb_reset_map_lo, u32 tpb_reset_map_hi);

/**
 * fw_io_is_reset_initiated() - Check if local reset is initiated or not.
 *
 * @bar0: Device's BAR0 base address
 *
 * Return: true if reset is initiated, false if reset is not yet started.
 */
bool fw_io_is_reset_initiated(void __iomem *bar0);

/**
 * fw_io_read_counters() - Reads hardware counters
 *
 * @ctx - FWIO context of the device for which counters are read.
 * @addr_in: hardware counter addresses to read
 * @val_out: counters values
 * @num_ctrs: number of counters to read
 *
 * Return: 0 on success.
 *
 */
int fw_io_read_counters(struct fw_io_ctx *ctx, uint64_t addr_in[], uint32_t val_out[],
			uint32_t num_counters);

/**
 * fw_io_server_info_read() - Read server info
 * @param bar - from bar
 * @param server_id - server id or -1 if invalid
 * @param rack_id - rack id or -1 if invalid
 * @return  0 on success.
 */
int fw_io_server_info_read(void *bar0, int *server_id, int * rack_id);


/**
 * fw_io_reservation_id_read() - Read reservation id
 * @param bar - from bar
 * @param reservation_id - server reservation id
 * @return  0 on success.
 */
int fw_io_reservation_id_read(void *bar0, uint64_t *reservation_id);

/**
 * fw_io_instance_partition_sz_read() - instance/partition sizes
 * @param bar - from bar
 * @param instance_sz - instance size.  -1 if invalid
 * @param partition_sz - partition size.  -1 if invalid
 * @return  0 on success.
 */
int fw_io_instance_partition_sz_read(void *bar0, int *instance_sz, int *partition_sz);

/**
 * fw_io_device_id_read() - Read device id
 * @param bar - from bar
 * @param device_id  - output device id
 * @return  0 on success.
 */
int fw_io_device_id_read(void *bar0, u32 *device_id);

/**
 * fw_io_device_power_read() - Read power utilization from firmware
 * @param bar - from bar
 * @param power  - output power, expressed as a percentage of maximum, in basis points (e.g. 0 - 10000)
 * @return  0 on success.
 */
int fw_io_device_power_read(void *bar0, u32 *power, unsigned die);

/**
 * fw_io_api_version_read() - Read the API version that the firmware is running
 * @param bar - from bar
 * @param version  - output API version
 * @return  0 on success.
 */
int fw_io_api_version_read(void * bar0, u32 *version);

/**
 * fw_io_fw_build_read() - Read the firmware build number
 * @param bar - from bar
 * @param fw_build  - output firmware build number
 * @return  0 on success.
 */
int fw_io_fw_build_read(void *bar0, u32 *fw_build);

/**
 * fw_io_device_id_write() - Read device id
 * @param bar - to bar
 * @param device_id  - output device id
 */
void fw_io_device_id_write(void *bar0, u32 device_id);

/**
 * fw_io_get_err_count() - gets the fw io error count
 * @ctx - FWIO context of the device for which counters are read.
 * @return  fw io error count on success.
 */
u64 fw_io_get_err_count(struct fw_io_ctx *ctx);

/**
 * fw_io_ecc_read() - Read ECC errors
 * 
 * @param bar: from bar
 * @param ecc_offset: one of
 *     FW_IO_REG_SRAM_ECC_OFFSET
 *     FW_IO_REG_HBM0_ECC_OFFSET
 *     FW_IO_REG_HBM1_ECC_OFFSET
 *     FW_IO_REG_HBM2_ECC_OFFSET
 *     FW_IO_REG_HBM3_ECC_OFFSET
 * @param ecc_err_count: output ecc error count
 * 
 * @return 0 on success
 */
int fw_io_ecc_read(void *bar0, uint64_t ecc_offset, uint32_t *ecc_err_count);

/**
 * fw_io_misc_ram_reg_read() - Read a single 32-bit misc RAM register by byte offset
 *
 * @param bar0: mapped BAR0 base
 * @param offset: byte offset of the register within the misc RAM block (e.g., FW_IO_REG_*_OFFSET)
 * @param val: output register value
 * @param log_error: whether to log an error in dmesg (false for periodic read which will create too much noise)
 * @return 0 on success
 */
int fw_io_misc_ram_reg_read(void *bar0, u64 offset, u32 *val, bool log_error);

/**
 * fw_io_serial_number_read() - Read serial number
 * 
 * @param bar0: from bar
 * @param serial_number: the serial number (lower 32 bits and upper 32 bits together as 64 bits serial number value)
 * @return 0 on success
 */
int fw_io_serial_number_read(void *bar0, uint64_t *serial_number);

/**
 * fw_io_get_total_ecc_err_counts() - Get UE ecc error count
 * @param bar0: from bar
 * @param unrepairable_ecc_count: Pointer to the unrepairable ue counter
 * @param repairable_err_count: Pointer to the repairable ue counter
 */
void fw_io_get_total_ecc_err_counts(void *bar0, uint32_t *unrepairable_ecc_count, uint32_t *repairable_ecc_count);

/**
 * fw_io_hbm_uecc_repair_state_read() - Get HBM UE ecc repair state
 * @param bar0: from bar
 */
int fw_io_hbm_uecc_repair_state_read(void *bar0, uint32_t *hbm_repair_state);

/**
 * fw_io_execute_request() - Execute request (original protocols)
 * @param ctx: FWIO context
 * @param command_id: Command ID
 * @param req: Request data
 * @param req_size: Request size
 * @param resp: Response buffer
 * @param resp_size: Response buffer size
 * @return 0 on success, negative on failure
 */
int fw_io_execute_request(struct fw_io_ctx *ctx, u8 command_id, const u8 *req, u32 req_size, u8 *resp, u32 resp_size);

/**
 * fw_io_execute_request_new() - Execute request (new protocols)
 * @param ctx: FWIO context
 * @param command_id: Command ID
 * @param req: Request data
 * @param req_size: Request size
 * @param resp: Response buffer
 * @param resp_size: Response buffer size
 * @return 0 on success, negative on failure
 */
int fw_io_execute_request_new(struct fw_io_ctx *ctx, u8 command_id, const u8 *req, u32 req_size, u8 *resp, u32 resp_size);

/**
 * fw_io_set_power_profile() - Set power profile
 * @param ctx: FWIO context
 * @param profile: Power profile value
 * @return 0 on success, negative on failure
 */
int fw_io_set_power_profile(struct fw_io_ctx *ctx, uint32_t profile);

/**
 * fw_io_get_performance_profile() - Get current performance profile
 * @param ctx: FWIO context
 * @param profile: Pointer to store the current profile value
 * @return 0 on success, negative on failure
 */
int fw_io_get_performance_profile(struct fw_io_ctx *ctx, uint32_t *profile);

/**
 * fw_io_enable_throttling_notifications() - Enable throttling notifications
 * @param ctx: FWIO context
 * @param enable: true to enable, false to disable
 * @return 0 on success, negative on failure
 */
int fw_io_enable_throttling_notifications(struct fw_io_ctx *ctx, bool enable);

/**
 * fw_io_get_available_profiles() - Get available profiles
 * @param ctx: FWIO context
 * @param feature: Profiles with a particular feature (0 for all profiles supported by instance)
 * @param num_profiles: Number of valid profiles in response
 * @param bitmap: Bitmap of supported profiles in response
 */
int fw_io_get_available_profiles(struct fw_io_ctx *ctx, u16 feature, u8 *num_profiles, u8 bitmap[32]);

/**
 * fw_io_get_bar_info() - Get switch fabric BAR info
 * @param ctx: FWIO context
 * @param query_type: 1 (INTER_SERVER) or 2 (INTRA_SERVER)
 * @param entries: BAR entries in response
 * @param count: Number of valid entries in response
 */
int fw_io_get_bar_info(struct fw_io_ctx *ctx, u8 query_type, struct fw_io_bar_entry *entries, u8 *count);

#endif
