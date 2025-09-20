// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/* Neuron devices supports a communication protocol(FWIO) between software and hardware.
 * The protocol works by providing a queue where software posts requests and hardware responds in
 * another queue. The messages exchanged in this queues are struct fw_io_request and struct fw_io_response.
 * Sequence number and CRC are used to make sure the the communication is reliable.
 *
 * The hardware supports two requests(commands):
 * 1. Read Register - To read register, software must use FWIO.
 * 2. Post Metric - Write statistics to CloudWatch.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fault-inject.h>

#include "neuron_reg_access.h"
#include "neuron_device.h"
#include "neuron_arch.h"
#include "v1/fw_io.h"
#include "neuron_fw_io.h"
#include "neuron_dhal.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(neuron_fail_fwio_read);
DECLARE_FAULT_ATTR(neuron_fail_fwio_post_metric);
#endif

int fw_io_ecc_read(void *bar0, uint64_t ecc_offset, uint32_t *ecc_err_count)
{
	if (ecc_offset != FW_IO_REG_SRAM_ECC_OFFSET &&
		ecc_offset != FW_IO_REG_HBM0_ECC_OFFSET &&
		ecc_offset != FW_IO_REG_HBM1_ECC_OFFSET &&
		ecc_offset != FW_IO_REG_HBM2_ECC_OFFSET &&
		ecc_offset != FW_IO_REG_HBM3_ECC_OFFSET) {
		pr_err("wrong ecc offset is given\n");
		return -EINVAL;
	}

	void *addr = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + ecc_offset;
	int ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(&addr, ecc_err_count, 1, false);
	if (ret) {
		pr_err("failed to get ecc error count from the device for ecc_offset=%llu\n", ecc_offset);
		return -EIO;
	}

	return 0;
}

int fw_io_hbm_uecc_repair_state_read(void *bar0, uint32_t *hbm_repair_state)
{
	int ret;
	void *addr = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_HBM_REPAIR_STATE_OFFSET;

	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(&addr, hbm_repair_state, 1, false);
	if (ret) {
		pr_err("failed to get hbm reapirable state\n");
		return -EIO;
	}
	/*
	 *  HBM Repair State Bitfield notes:
	 *      2 bits to represent the state of hbm repair
	 *      0x0 means no pending repair
	 *      0x1 means pending repair
	 *      0x2 means repair failure
	 */
	*hbm_repair_state = *hbm_repair_state & 0x3;
	return 0;
}

int fw_io_serial_number_read(void *bar0, uint64_t *serial_number)
{
	int ret = 0;

	uint32_t serial_number_lo = 0;
	void *addr_lo = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_SERIAL_NUMBER_LO_OFFSET;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(&addr_lo, &serial_number_lo, 1, false);
	if (ret) {
		pr_err("failed to get the lower 32 bits of the serial number from the device\n");
		return -EIO;
	}

	uint32_t serial_number_hi = 0;
	void *addr_hi = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_SERIAL_NUMBER_HI_OFFSET;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(&addr_hi, &serial_number_hi, 1, false);
	if (ret) {
		pr_err("failed to get the higher 32 bits of the serial number from the device\n");
		return -EIO;
	}

	*serial_number = ((uint64_t)serial_number_hi << 32) | serial_number_lo;

	return ret;
}

int fw_io_device_power_read(void *bar0, u32 *power, unsigned die)
{
	int ret;

	if (die >= ndhal->ndhal_address_map.dice_per_device) {
		pr_err("die %u is out of range\n", die);
		return -EINVAL;
	}

    // Read power utilization from MiscRAM.  The power utilization for each die are set up in contiguous 32 bit
    // miscram registers, so we can treat it like an array of uint32s for our purposes.
	void *addr = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_POWER_UTIL_D0_OFFSET + 4*die;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(&addr, power, 1, false);
	if (ret) {
		pr_err("failed to get device power from the device, ret = %d\n", ret);
	}

	return ret;
}

int fw_io_api_version_read(void * bar0, u32 *version)
{
	int ret;

	void *addr = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_API_VERSION_OFFSET;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array(&addr, version, 1, false);
	if (ret) {
		pr_err("failed to get api version from the device, ret = %d\n", ret);
	}

	return ret;
}

int fw_io_device_id_read(void *bar0, u32 *device_id)
{
	void * addr = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_DEVICE_ID_OFFSET;
	return ndhal->ndhal_fw_io.fw_io_read_csr_array( &addr, device_id, 1, false);
}

void fw_io_device_id_write(void *bar0, u32 device_id)
{
	void * addr = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_DEVICE_ID_OFFSET;
	reg_write32(addr, device_id);
}

/** Set base address of request and response queues.
 */
static int fw_io_init(void __iomem *bar0, u64 request_addr, u64 response_addr)
{
	reg_write32(bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_REQUEST_BASE_ADDR_LOW_OFFSET,
		    UINT64_LOW(request_addr));
	reg_write32(bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_REQUEST_BASE_ADDR_HIG_OFFSET,
		    UINT64_HIGH(request_addr));
	reg_write32(bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_RESPONSE_BASE_ADDR_LOW_OFFSET,
		    UINT64_LOW(response_addr));
	reg_write32(bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_RESPONSE_BASE_ADDR_HIGH_OFFSET,
		    UINT64_HIGH(response_addr));
	return 0;
}

/** Trigger doorbell so that FWIO request can be processed.
 */
static void fw_io_trigger(void __iomem *bar0)
{
	reg_write32(bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_TRIGGER_INT_NOSEC_OFFSET, 1);
}

static const u32 crc32c_table[256] = {
	0x00000000, 0xf26b8303, 0xe13b70f7, 0x1350f3f4, 0xc79a971f, 0x35f1141c, 0x26a1e7e8,
	0xd4ca64eb, 0x8ad958cf, 0x78b2dbcc, 0x6be22838, 0x9989ab3b, 0x4d43cfd0, 0xbf284cd3,
	0xac78bf27, 0x5e133c24, 0x105ec76f, 0xe235446c, 0xf165b798, 0x030e349b, 0xd7c45070,
	0x25afd373, 0x36ff2087, 0xc494a384, 0x9a879fa0, 0x68ec1ca3, 0x7bbcef57, 0x89d76c54,
	0x5d1d08bf, 0xaf768bbc, 0xbc267848, 0x4e4dfb4b, 0x20bd8ede, 0xd2d60ddd, 0xc186fe29,
	0x33ed7d2a, 0xe72719c1, 0x154c9ac2, 0x061c6936, 0xf477ea35, 0xaa64d611, 0x580f5512,
	0x4b5fa6e6, 0xb93425e5, 0x6dfe410e, 0x9f95c20d, 0x8cc531f9, 0x7eaeb2fa, 0x30e349b1,
	0xc288cab2, 0xd1d83946, 0x23b3ba45, 0xf779deae, 0x05125dad, 0x1642ae59, 0xe4292d5a,
	0xba3a117e, 0x4851927d, 0x5b016189, 0xa96ae28a, 0x7da08661, 0x8fcb0562, 0x9c9bf696,
	0x6ef07595, 0x417b1dbc, 0xb3109ebf, 0xa0406d4b, 0x522bee48, 0x86e18aa3, 0x748a09a0,
	0x67dafa54, 0x95b17957, 0xcba24573, 0x39c9c670, 0x2a993584, 0xd8f2b687, 0x0c38d26c,
	0xfe53516f, 0xed03a29b, 0x1f682198, 0x5125dad3, 0xa34e59d0, 0xb01eaa24, 0x42752927,
	0x96bf4dcc, 0x64d4cecf, 0x77843d3b, 0x85efbe38, 0xdbfc821c, 0x2997011f, 0x3ac7f2eb,
	0xc8ac71e8, 0x1c661503, 0xee0d9600, 0xfd5d65f4, 0x0f36e6f7, 0x61c69362, 0x93ad1061,
	0x80fde395, 0x72966096, 0xa65c047d, 0x5437877e, 0x4767748a, 0xb50cf789, 0xeb1fcbad,
	0x197448ae, 0x0a24bb5a, 0xf84f3859, 0x2c855cb2, 0xdeeedfb1, 0xcdbe2c45, 0x3fd5af46,
	0x7198540d, 0x83f3d70e, 0x90a324fa, 0x62c8a7f9, 0xb602c312, 0x44694011, 0x5739b3e5,
	0xa55230e6, 0xfb410cc2, 0x092a8fc1, 0x1a7a7c35, 0xe811ff36, 0x3cdb9bdd, 0xceb018de,
	0xdde0eb2a, 0x2f8b6829, 0x82f63b78, 0x709db87b, 0x63cd4b8f, 0x91a6c88c, 0x456cac67,
	0xb7072f64, 0xa457dc90, 0x563c5f93, 0x082f63b7, 0xfa44e0b4, 0xe9141340, 0x1b7f9043,
	0xcfb5f4a8, 0x3dde77ab, 0x2e8e845f, 0xdce5075c, 0x92a8fc17, 0x60c37f14, 0x73938ce0,
	0x81f80fe3, 0x55326b08, 0xa759e80b, 0xb4091bff, 0x466298fc, 0x1871a4d8, 0xea1a27db,
	0xf94ad42f, 0x0b21572c, 0xdfeb33c7, 0x2d80b0c4, 0x3ed04330, 0xccbbc033, 0xa24bb5a6,
	0x502036a5, 0x4370c551, 0xb11b4652, 0x65d122b9, 0x97baa1ba, 0x84ea524e, 0x7681d14d,
	0x2892ed69, 0xdaf96e6a, 0xc9a99d9e, 0x3bc21e9d, 0xef087a76, 0x1d63f975, 0x0e330a81,
	0xfc588982, 0xb21572c9, 0x407ef1ca, 0x532e023e, 0xa145813d, 0x758fe5d6, 0x87e466d5,
	0x94b49521, 0x66df1622, 0x38cc2a06, 0xcaa7a905, 0xd9f75af1, 0x2b9cd9f2, 0xff56bd19,
	0x0d3d3e1a, 0x1e6dcdee, 0xec064eed, 0xc38d26c4, 0x31e6a5c7, 0x22b65633, 0xd0ddd530,
	0x0417b1db, 0xf67c32d8, 0xe52cc12c, 0x1747422f, 0x49547e0b, 0xbb3ffd08, 0xa86f0efc,
	0x5a048dff, 0x8ecee914, 0x7ca56a17, 0x6ff599e3, 0x9d9e1ae0, 0xd3d3e1ab, 0x21b862a8,
	0x32e8915c, 0xc083125f, 0x144976b4, 0xe622f5b7, 0xf5720643, 0x07198540, 0x590ab964,
	0xab613a67, 0xb831c993, 0x4a5a4a90, 0x9e902e7b, 0x6cfbad78, 0x7fab5e8c, 0x8dc0dd8f,
	0xe330a81a, 0x115b2b19, 0x020bd8ed, 0xf0605bee, 0x24aa3f05, 0xd6c1bc06, 0xc5914ff2,
	0x37faccf1, 0x69e9f0d5, 0x9b8273d6, 0x88d28022, 0x7ab90321, 0xae7367ca, 0x5c18e4c9,
	0x4f48173d, 0xbd23943e, 0xf36e6f75, 0x0105ec76, 0x12551f82, 0xe03e9c81, 0x34f4f86a,
	0xc69f7b69, 0xd5cf889d, 0x27a40b9e, 0x79b737ba, 0x8bdcb4b9, 0x988c474d, 0x6ae7c44e,
	0xbe2da0a5, 0x4c4623a6, 0x5f16d052, 0xad7d5351,
};

static void dx_crc32c_add(const u8 *data, size_t len, u32 *csum)
{
	size_t i;

	for (i = 0; i < len; i++) {
		*csum = (*csum >> 8) ^ crc32c_table[(*csum ^ data[i]) & 0xff];
	}
}

static u32 crc32c(const u8 *data, size_t len)
{
	u32 csum = 0xffffffff;
	dx_crc32c_add(data, len, &csum);
	return csum ^ 0xffffffff;
}

static int fw_io_execute_request(struct fw_io_ctx *ctx, u8 command_id, const u8 *req, u32 req_size,
			  u8 *resp, u32 resp_size)
{
	int ret;
	const u32 max_req_size = ctx->request_response_size - sizeof(struct fw_io_request);
	const u32 max_resp_size = ctx->request_response_size - sizeof(struct fw_io_response);
	u8 resp_seq;
	if (req_size > max_req_size) {
		pr_err("invalid request size: %u, max: %u\n", req_size, max_req_size);
		return -EINVAL;
	}
	if (resp_size > max_resp_size) {
		pr_err("invalid response size: %u, max: %u\n", resp_size, max_resp_size);
		return -EINVAL;
	}

	mutex_lock(&ctx->lock);

	int i;
	for (i=0; i < FW_IO_RD_RETRY; i++){

		fw_io_init(ctx->bar0, ctx->request_addr, ctx->response_addr);
		if (++ctx->next_seq_num == 0)
			ctx->next_seq_num = 1;

		memcpy(ctx->request->data, req, req_size);
		ctx->request->sequence_number = ctx->next_seq_num;
		ctx->request->command_id = command_id;
		ctx->request->size = req_size + sizeof(struct fw_io_request);
		ctx->request->crc32 = 0;
		ctx->request->crc32 = crc32c((const u8 *)ctx->request, ctx->request->size);
		// make sure the sequence number we will wait on is not the same
		ctx->response->sequence_number = 0;
		dma_rmb();
		fw_io_trigger(ctx->bar0);
		// now wait for resp->seq == req->seq which indicates that request has been completed and
		// we have a response
		ktime_t start_time = ktime_get();

		volatile u8 *fwio_seq = (volatile u8 *)&ctx->response->sequence_number;
		do {
			resp_seq = READ_ONCE(*fwio_seq);
			if (resp_seq == ctx->next_seq_num)
				break;
			msleep(1);
		} while ( ktime_to_us(ktime_sub(ktime_get(), start_time)) <  FW_IO_RD_TIMEOUT);

		ret = -1;
		if (resp_seq != ctx->next_seq_num) {
			if (command_id != FW_IO_CMD_POST_TO_CW)
				pr_err("seq: %u, cmd: %u timed out\n", ctx->next_seq_num, command_id);
			continue;
		}
		if (ctx->response->error_code == FW_IO_SUCCESS) {
			if ((ctx->response->size - sizeof(struct fw_io_response)) > resp_size) {
				// this is probably not possible
				pr_err("seq: %u, cmd: %u response too large (%u)\n", ctx->next_seq_num,
			       	command_id, ctx->response->size);
				goto done;
			}
			memcpy(resp, ctx->response->data,
		       	ctx->response->size - sizeof(struct fw_io_response));
			ret = 0;
			goto done;
		}
		ctx->fw_io_err_count++;
		pr_err(KERN_ERR "seq: %u, cmd: %u failed %u\n", ctx->next_seq_num, command_id,
	       	ctx->response->error_code);
		// if we get an unsupported command response, don't retry
		if (ctx->response->error_code == FW_IO_UNKNOWN_COMMAND) {
			ret = -1;
			goto done;
		}
	}
done:

	mutex_unlock(&ctx->lock);
	return ret;
}

/** Read the list of addresses given in the address list and returns it's values in the value list
 *
 * @param ctx[in]	- FWIO context
 * @param addr_in[in]	- List of registers to read
 * @param values[out]	- Buffer to store results.
 * @param num_req[in]	- Total number of registers in the addr_in
 *
 * @return 0 on success 1 on error
 */
int fw_io_read(struct fw_io_ctx *ctx, u64 addr_in[], u32 val_out[], u32 num_req)
{
#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_fwio_read, 1))
		return -ETIMEDOUT;
#endif
	return fw_io_execute_request(ctx, FW_IO_CMD_READ, (u8 *)addr_in, sizeof(u64) * num_req,
			     	(u8 *)val_out, sizeof(u32) * num_req);
}

/** Handle offset to device physical address mapping.
 *   1. All hardware reads must go through FWIO.
 *   2. FWIO understands only device physical addresses.
 *   3. Driver code uses BAR+offset address.
 * To map, device address to BAR address, each the BAR is mapped to a region.
 */
struct fwio_read_region {
	struct fw_io_ctx *ctx; // FWIO context associated with this device.
	void __iomem *region_ptr_start; // BAR address
	void __iomem *region_ptr_end;
	u64 device_physical_address; // device address mapping this region.
};

#define MAX_REGIONS MAX_NEURON_DEVICE_COUNT * 2 // bar0 and bar2 for every device
static struct fwio_read_region fwio_read_regions[MAX_REGIONS] = { { 0 } };

int fw_io_register_read_region(struct fw_io_ctx *ctx, void __iomem *region_ptr,
				      u64 region_size, u64 device_physical_address)
{
	int i;
	for (i = 0; i < MAX_REGIONS; i++) {
		if (fwio_read_regions[i].ctx == NULL) { // free
			fwio_read_regions[i].ctx = ctx;
			fwio_read_regions[i].region_ptr_start = region_ptr;
			fwio_read_regions[i].region_ptr_end = region_ptr + region_size;
			fwio_read_regions[i].device_physical_address = device_physical_address;
			return 0;
		}
	}
	return -1;
}

int fw_io_read_csr_array_direct(void **addrs, u32 *values, u32 num_csrs, bool operational)
{
	int i;

	if (operational) {
		for (i = 0; i < num_csrs; i++) {
			values[i] = readl(addrs[i]);
		}
		return 0;
	}

	// reading during initialization/reset (only allow 1 read at a time)
	if (num_csrs != 1) {
		pr_err("error: attempting multi-csr read during reset/initialization\n");
		return -1;
	}
	
	for (i=0; i < FW_IO_RD_RETRY; i++) {
		ktime_t start_time = ktime_get();
		do {
			values[0] = readl(addrs[0]);

			if (values[0] != 0xdeadbeef) return 0;
			msleep(1);
		} while (ktime_to_us(ktime_sub(ktime_get(), start_time)) < FW_IO_RD_TIMEOUT);
	}
	return -1;
}

int fw_io_read_csr_array_readless(void **ptrs, u32 *values, u32 num_csrs)
{
	int i, j;
	for (i = 0; i < MAX_REGIONS; i++) {
		void *start, *end;
		if (fwio_read_regions[i].ctx == NULL) {
			pr_err("No region found for address %px\n", ptrs[0]);
			break;
		}
		start = (void *)fwio_read_regions[i].region_ptr_start;
		end = (void *)(fwio_read_regions[i].region_ptr_end - sizeof(u32));
		if (ptrs[0] >= start && ptrs[0] <= end) {
			u64 addrs[FW_IO_MAX_READLESS_READ_REGISTER_COUNT];
			u64 off;
			for (j = 0; j < num_csrs; j++) {
				// CSRs must be in the same readless region
				if (j > 0 && (ptrs[j] < start || ptrs[j] > end))
					return -1;
				off = ptrs[j] - start;
				addrs[j] = fwio_read_regions[i].device_physical_address + off;
			}
			return fw_io_read(fwio_read_regions[i].ctx, addrs, values, num_csrs);
		}
	}

	return -1;
}

void fw_io_initiate_reset(void __iomem *bar0, bool device_reset, u32 tpb_reset_map)
{
	u32 reset_type;
	void *address;
	if (device_reset) {
		reset_type = FW_IO_RESET_TYPE_DEVICE;
	} else {
		reset_type = FW_IO_RESET_TYPE_TPB;
		address = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_RESET_TPB_MAP_OFFSET;
		reg_write32((u32 *)address, tpb_reset_map);
		mb();
	}
	address = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_RESET_OFFSET;
	reg_write32((u32 *)address, reset_type);
	mb();
	fw_io_trigger(bar0);
}

bool fw_io_is_reset_initiated(void __iomem *bar0)
{
	void *address;
	int ret;
	u32 val;

	address = bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_RESET_OFFSET;
	ret = ndhal->ndhal_fw_io.fw_io_read_csr_array((void **)&address, &val, 1, false);
	if (ret == 0 && val == 0)
		return true;
	return false;
}

int fw_io_post_metric(struct fw_io_ctx *ctx, u8 *data, u32 size)
{
	if (!ctx) {
		pr_err("error: attempting to post metrics without initializing fw_io_ctx");
		return -EINVAL;
	}
	u32 size_aligned = (size / sizeof(u32)) * sizeof(u32);
	u32 padded_u32 = 0;
	u32 *m = (u32 *)data;
	int i;

#ifdef CONFIG_FAULT_INJECTION
	if (should_fail(&neuron_fail_fwio_post_metric, 1))
		return -ETIMEDOUT;
#endif
	if (size > FW_IO_REG_METRIC_BUF_SZ) {
		return -E2BIG;
	}

	// Write the data in the misc ram first
	void * offset = (void *) (ctx->bar0 + ndhal->ndhal_address_map.bar0_misc_ram_offset + FW_IO_REG_METRIC_OFFSET);
	for (i = 0; i < (size / 4); i++) {
		reg_write32(offset + (i * 4), m[i]);
	}

	// Copy the remaining bytes
	if (size - size_aligned) {
		memcpy((uint8_t *)&padded_u32, data + size_aligned, size - size_aligned);
		reg_write32(offset + size_aligned, padded_u32);
	}

	return fw_io_execute_request(ctx, FW_IO_CMD_POST_TO_CW, data, size, data, size);
}


int fw_io_read_counters(struct fw_io_ctx *ctx, uint64_t addr_in[], uint32_t val_out[],
			uint32_t num_counters)
{
	return fw_io_read(ctx, addr_in, val_out, num_counters);
}

u64 fw_io_get_err_count(struct fw_io_ctx *ctx)
{
	if (ctx == NULL) {
		return 0;
	}
	return ctx->fw_io_err_count;
}

// Max size available for each message.
#define FW_IO_MAX_SIZE 0xffff

struct fw_io_ctx *fw_io_setup(void __iomem *bar0, u64 bar0_size,
			      void __iomem *bar2, u64 bar2_size)
{
	struct fw_io_ctx *ctx = (struct fw_io_ctx *)kzalloc(sizeof(struct fw_io_ctx), GFP_KERNEL);
	if (ctx == NULL) {
		pr_err("memory allocation failed\n");
		return NULL;
	}
	ctx->fw_io_err_count = 0;
	ctx->bar0 = bar0;
	ctx->next_seq_num = 1;
	mutex_init(&ctx->lock);

	ctx->request = kmalloc(FW_IO_MAX_SIZE, GFP_ATOMIC);
	if (ctx->request == NULL) {
		pr_err("memory allocation failed\n");
		goto error;
	}

	ctx->request_addr = virt_to_phys(ctx->request);
	ctx->request_addr |= ndhal->ndhal_address_map.pci_host_base;

	ctx->response = kmalloc(FW_IO_MAX_SIZE, GFP_ATOMIC);
	if (ctx->response == NULL) {
		pr_err("memory allocation failed\n");
		goto error;
	}

	ctx->response_addr = virt_to_phys(ctx->response);
	ctx->response_addr |= ndhal->ndhal_address_map.pci_host_base;

	if (ndhal->ndhal_fw_io.fw_io_register_readless_read_region(ctx, bar0, bar0_size, bar2, bar2_size)) {
		pr_err("failed to register readless read region\n");
		goto error;
	}

	return ctx;

error:
	fw_io_destroy(ctx);
	return NULL;
}

void fw_io_destroy(struct fw_io_ctx *ctx)
{
	if (ctx == NULL)
		return;

	if (ctx->request) {
		kfree(ctx->request);
		ctx->request = NULL;
	}

	if (ctx->response) {
		kfree(ctx->response);
		ctx->response = NULL;
	}

	kfree(ctx);
}

uint32_t fw_io_get_total_uecc_err_count(void *bar0) {
	uint32_t total_uncorrected_ecc_err_count = 0;
	uint32_t channel = 0;
	uint32_t ecc_err_count = 0;
	uint64_t ecc_offset = 0;

	for (channel = 0; channel < ndhal->ndhal_address_map.dram_channels; channel++) {
		ecc_offset = FW_IO_REG_HBM0_ECC_OFFSET + channel * sizeof(uint32_t);
		ecc_err_count = 0;
		int ret = fw_io_ecc_read(bar0, ecc_offset, &ecc_err_count);
		if (ret) {
			pr_err("sysfs failed to read ECC HBM%u error from FWIO\n", channel);
		} else if (ecc_err_count != 0xdeadbeef) {
			// ue count is in the lowest 16 bits
			total_uncorrected_ecc_err_count += (ecc_err_count & 0x0000ffff);
		}
	}
	return total_uncorrected_ecc_err_count;
}
