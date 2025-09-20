// SPDX-License-Identifier: GPL-2.0
/*
* Copyright 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
*/

/*
 * Pod Election framework (TODO - rename to nutd - Neuron UltraServer Topology Discovery)
 *
 *   The ultraserver election framework validates connectivity between ultraserver nodes and performs an election to determine
 *   node ids for the nodes within the ultraserver along with the unique id of the ultraserver.
 *
 *   There's nothing physically or materially that changes after we perform a ultraserver election, it's more an agreement between
 *   software entities on the ultraserver that they want to cooperate and in that spirit, assign Ids that are used to map resources
 *   and their associated connectivity within the ultraserver in a particular way.
 *
 *   A ultraserver election triggers this "node assignment" process.  There are more details elsewhere that describe additional
 *   topology details. Here we'll just discuss the election process and node id assignment.
 *
 *   The election process requires the use of DMA engines across all devices on an instance.  The DMA engines are also used 
 *   by the runtime for model execution, so election has to occur before runtime starts executing models.  The election
 *   software has to logically own (gather) all devices before it performs an election.  Aka you can't have a model executing
 *   on a neuron device during the election.
 *
 *   The election has to be prosecuted on all four nodes at the same time (at least initially) because the nodes need to
 *   exchange information to discover topology and perform node assignment.
 *
 *   There are two methods by which an election can be initiated.  The first (option) is at driver load time, the second option is "on demand"
 *   triggered by a standalone program using ultraserver control APIs.  Election at driver load time is performed immediately after all
 *   devices have successfully completed reset.  For on demand Election, the standalone program should check if all cores are free
 *   prior to initiating the election via the ultraserver control ioctl.
 *
 *   Election results are stored in miscram so that if the driver is unloaded/reloaded/upgraded, the driver can reload the election
 *   results from miscram.
 *
 *   Prosecuting an Election:
 *
 *   There are logically two aspects to the election process.
 *     1. Acquistion of DMA resources (only when crwl mark = 0)
 *     2. Prosecuting the actual election in the driver which involves
 *        checking links, exchanging data between nodes, determining topology, etc.
 *
 *   Acquiring DMA resources
 *    - The driver uses crwl "mark" count to track ownership.  The election code uses changes in the "mark" count
 *      to track allocation and freeing of cores to drive internal state.
 *    - A new election is allowed to start only after core ownership has gone to zero.
 *    - An election will abort on a timeout or (b) if an election kill request is received.
 *    - The application can poll for election completion from user space via ioctl or sysfs
 *
 *   Prosecuting the election:
 *
 *   - There are 16 devices on a node.  One device is designated as the primary (device 0) and is responsible for overall election duties,
 *     the other 15 are secondaries and are responsible for checking their neighbor links and reporting back to the primary election function.
 *
 *   - Secondaries report if (a) they reset successfully, (b) there links are good (c), their link is bad, 
 *     or (d) they aren't wired to the right ultraserver neighbors
 *
 *   - Election operations are state driven to maintain consistency.
 *
 *   - The election process needs to be (more or less) transactional (ACID)
 * 
 *   - Election data:
 *     - election data            - this is data that a node has collected and wants to show to its neighbors (both neighbor's serial numbers).
 *                                  this data is populated during the election and cleared at election completion.
 *     - election status  		  - FIXME cleared at the start of a ultraserver election.
 *     - election node_id 		  - cleared at the start of a ultraserver election, set at end of a ultraserver election.
 *     - election server_id        - set at the end of a ultraserver election.
 *
 *   - Election flow (primary)
 *     - primary reads it's own serial number
 *     - primary reads neighbors serial numbers and checks link connectivity is correct
 *     - If neighbor serial number reads are successful and match up on link pairs
 *       - update local copy of neighbor serial numbers (election data) so our neighbors know we successfully read their 
 *         serial numbers.  
 *       - next check neighbor's local "election data" to see if they successfully read their neighbors serial numbers.
 *         This valid election data check also represents a state transition during the election process.  The election 
 *         process for this device won't proceed to the status check phase until it sees valid election data.
 *     - otherwise, fail
 *     - primary also receives count of secondaries that passed their connectivity check.
 *     - if secondaryies report back good continue
 *     - otherwise fail
 *     - determine node id and server  unique id.
 *     - set election state to Successful
 *     - read election status from neighbors 
 *     - if neighbors report success, countinue, otherwise set election status to failure
 *     - set state to "ultraserver" and update miscram with ultraserver serial number, status, node id, node cnt and mask of links used in the election.
 *     - clear election data to prevent partial elections by creating an interlock at the step where neighbor's local "election data" is checked.
 *
 *   - Election flow (secondary)
 *     - Largely the same as primary in terms of connectivity checking.  They just don't do all the final miscram updating w/ node id, etc.
 *
 *   - An Election is run from a single thread which processes devices serially.  This means all nodes need to process
 *     devices in the same order to avoid deadlocking which would eventually result in a timeout.  Originally the code
 *     piggybacked the election on the reset threads so that device could process the primary/secondary elections in parallel.
 *
 *   - Lame Duck election
 *     - If an election fails due to broken links, we attempt to run the election using only one link pair in an attempt to for two 2-node pairs.
 *       Currently we first attempt this on the right link, then if that fails, attempt the election again on the left link.
 *
 *   Election Results:
 *     Results of the election are reported in sysfs under /sys/class/neuron_device.
 *
 *   Operating Modes:
 *
 *     Ultraservers can be used in 4-node, 2-node horizonal, 2-node vertical or 1-node configuration referred to as modes.
 *     The election results determine what modes an ultraserver supports. Aka, if the driver successfully prosecutes a 4
 *     node election, it can support 4-node, 2-node vertical and 1-node configuration modes.
 *
 *     Runtime requests the mode it wants to operate the ultraserver node in through the pod_ctrl ioctl.
 *
 *     Once an operating mode is set, it cannot be changed until all neuron cores have been released.
 *
 *   APIs - Pod Control and Status
 *   - The pod control APIs can 
 *     (a) request to start an election 
 *     (b) request to make the node single node (suppress election results)
 *     (c) request to kill the election.
 *
 *   Other notes:
 *   - Logically we only need to run the election itself to successful completion once.  At this point
 *     we know the topology of the nodes in the Pod which is orthogonal to whether or not the software
 *     running on the pod nodes desire to be part of the cluster.
 *
 *   userver_ctl parameter:
 *   - Bitmask that can override various aspects of the election 
 *   - Things it can control
 *     - skipping election at driver load time
 *     - verbose messages
 *     - clearing prior election results from miscram
 *     - injecting various link faults
 *     - force use of right link only 
 *   - Interesting combos (bit values)
 *     - 65   (0x041) = clear election results from miscram and skip election.  This will clear any old state.  Requires resets to complete so don't unload the driver too early.
 *     - 192  (0x000) = clear election results from miscram and Force left link failure.  This will trigger a lame duck election using right link.
 *     - 512  (0x200) = use right link only to prosecute the election.  Basically force 2-node configuration at driver load.
 *     - 1088 (0x440) = use left link only to prosecute the election.  Basically force 2-node configuration at driver load.
 *
 */
#include "share/neuron_driver_shared.h"
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/mutex.h>

#include "udma/udma.h"
#include "sdma.h"
#include "../neuron_dhal.h"
#include "../neuron_reset.h"
#include "../neuron_core.h"
#include "../neuron_dma.h"
#include "../neuron_fw_io.h"
#include "../neuron_pci.h"
#include "../neuron_trace.h"
#include "../neuron_ring.h"
#include "../neuron_mempool.h"
#include "../neuron_crwl.h"
#include "neuron_pelect.h"

/*
 * UltraServer ctl to
 * - control of when election is triggered
 * - control links used for election
 * - fault injection of various failure scenarios
 */
#define NPE_POD_CTL_RST_SKIP_ELECTION  (1<<0)
#define NPE_POD_CTL_SPOOF_BASE         (1<<1)
#define NPE_POD_CTL_VERBOSE            (1<<2)
#define NPE_POD_CTL_FAULT_SEC_LNK_FAIL (1<<3)
#define NPE_POD_CTL_FAULT_PRI_LNK_FAIL (1<<4)
#define NPE_POD_CTL_SKIP_NBR_CPY_READ  (1<<5)
#define NPE_POD_CTL_CLR_MISCRAM        (1<<6)
#define NPE_POD_CTL_FAULT_L_LNK_FAIL   (1<<7)
#define NPE_POD_CTL_FAULT_R_LNK_FAIL   (1<<8)
#define NPE_POD_CTL_USE_R_LNK_ONLY     (1<<9)
#define NPE_POD_CTL_USE_L_LNK_ONLY     (1<<10)


/* Enable ultraserver auto election (4 node configuration) by default  */
int userver_ctl = 0;
module_param(userver_ctl, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(userver_ctl, "ultraserver election control");

/*TODO tmp until we do wholesale name change */
#define pod_ctl userver_ctl

/* default ultraserver election timeout 10 min */
int userver_etimeout = 600;
module_param(userver_etimeout, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(userver_etimeout, "ultraserver election timeout");

/*TODO tmp until we do wholesale name change */
#define pod_etimeout userver_etimeout

#define NPE_RETRY_WAIT_MS 50
#define NPE_RETRY_WAIT_REPORTING_INTERVAL ((30 * 1000) / NPE_RETRY_WAIT_MS)

#define NPE_ETIMEOUT_EXTENSION_MS (120 * 1000)

/**
 * internal state of the pod election module
 *
 */
enum neuron_pod_state_internal {
	NEURON_NPE_POD_ST_INIT = 0,             	// initializing - before pod formation
	NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS = 1,	// on demand election in progress
	NEURON_NPE_POD_ST_ELECTION_SUCCESS = 2, 	// successful pod formation
	NEURON_NPE_POD_ST_ELECTION_FAILURE = 3, 	// failure forming pod or pod not formed yet
};

/**
 * election status reported in miscram
 *
 */
enum neuron_pod_election_mr_sts {
	NEURON_NPE_POD_ELECTION_MR_STS_INIT = 0,		// initializing - before pod formation
	NEURON_NPE_POD_ELECTION_MR_STS_DEPRECATED = 1, 	// deprecated orginal successful pod formation status (to invalidate older driver caches)
	NEURON_NPE_POD_ELECTION_MR_STS_FAILURE = 2, 	// failure forming pod
	NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS = 3, 	// successful pod formation
};

#define NPE_MR_STS_GET(v)  			((v) & 0xFF)
#define NPE_MR_LR_MASK_GET(v)  		(((v) >>  8) & 0xFF)
#define NPE_MR_NODE_ID_GET(v)		(((v) >> 16) & 0xFF)
#define NPE_MR_NODE_CNT_GET(v)		(((v) >> 24) & 0xFF)
#define NPE_MR_STS_SET(s,m, n,c)	(((s) & 0x0FF) | (((m) & 0xFF) << 8) | (((n) & 0xFF) << 16) |  (((c) & 0xFF) << 24))

struct ndhal_v3ext_pelect {
	struct mutex lock;          		// pod control api lock
	struct task_struct *thread; 		// election thread
	wait_queue_head_t wait_queue;		// election thread's wait queue
	volatile bool stop; 				// if set, election thread would exit the loop
	bool kill_election;					// if set, kill the election.
	enum neuron_ultraserver_mode mode;	// current pod operating mode (requested by application)
	u32 lr_mask_default;				// default left/right link mask to use in election
	u32 lr_mask;						// left/right link mask to use in election
	int pod_state_internal;   			// state of the pod
	int node_id;			  			// node id
	int node_cnt;			  			// node count
	u64  pod_serial_num;	  			// serial number of the pod (node 0's serial number)
	ktime_t nbr_data_read_deadline; 	// timeout deadline for neighbor data read
	u64  nbr_data_read_timeout;			// timeout for the neighbor data read
	struct neuron_device *pnd[16]; 		// array of device pointers, populated at reset completion.
};

/**
 * pod election & state tracking struct.
 *   TODO - probably should encapsulate as dhal v3 extension.
 *
 */
static struct ndhal_v3ext_pelect ndhal_pelect_data = {
	.lock = __MUTEX_INITIALIZER(ndhal_pelect_data.lock),
	.thread = NULL,
	.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(ndhal_pelect_data.wait_queue),
	.stop = false,
	.kill_election = false,
	.mode = NEURON_ULTRASERVER_MODE_UNSET,
	.lr_mask_default = 0x3,
	.lr_mask = 0x3,
	.pod_state_internal = NEURON_NPE_POD_ST_INIT,
	.node_id = -1,
	.node_cnt = 0,
	.pod_serial_num = 0,
	.nbr_data_read_deadline = 0,
	.nbr_data_read_timeout = 0,
	.pnd = {NULL},
};

/**
 * pod neighbor io resource tracking structure
 *
 *   all the resources needs to send a neighbor read request to miscram.
 *   If we wanted to encapsulate more, it could include ops for init/destroy/read/read_ack
 *
 */
typedef struct pod_neighbor_io {
	struct neuron_device *nd;
	u32 eng_id;
	u32 ring_size;
	size_t data_size;
	struct mem_chunk *tx_mc;
	struct mem_chunk *rx_mc;
	struct mem_chunk *data_mc;
} pod_neighbor_io_t;

static bool npe_pod_ctl_is_set(int value)
{
	return (pod_ctl & value);
}

static bool npe_pod_ctl_inject_link_error(int link)
{
	return ((link == 0) && npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_L_LNK_FAIL)) || ((link == 1) && npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_R_LNK_FAIL));
}

static bool npe_election_canceled(void)
{
	return (ndhal_pelect_data.kill_election || ndhal_pelect_data.stop);
}

static int npe_msleep_stoppable(uint32_t msec) 
{
	unsigned long timeout = msecs_to_jiffies(msec);

	while (timeout && !npe_election_canceled())
		timeout = schedule_timeout_interruptible(timeout);

	return jiffies_to_msecs(timeout);
}
	
/**
 * npe_pod_neighbor_io_init
 *   
 *   initialize the neighbor io structure - which is basically all the stuff
 *   you need to do a dma.
 *
 */
static int npe_pod_neighbor_io_init(pod_neighbor_io_t* pnio, struct neuron_device *nd, u32 eng_id)
{
	int ret;

	pnio->nd = nd;		
	pnio->eng_id = eng_id;		
	pnio->ring_size = 1024;		
	pnio->data_size = PAGE_SIZE;		

	ret = ndmar_eng_init(nd, eng_id);
	if (ret) {
		pr_err("ultraserver election io dma init failed");
		goto done;
	}

	ret = mc_alloc_align(nd, MC_LIFESPAN_LOCAL, pnio->ring_size * sizeof(union udma_desc), 0, MEM_LOC_HOST, 0, 0, 0, NEURON_MEMALLOC_TYPE_MISC_HOST, &pnio->tx_mc);
	if (ret) {
		pr_err("ultraserver election io memory allocation failed");
		goto done;
	}
	
	ret = mc_alloc_align(nd, MC_LIFESPAN_LOCAL, pnio->ring_size * sizeof(union udma_desc), 0, MEM_LOC_HOST, 0, 0, 0, NEURON_MEMALLOC_TYPE_MISC_HOST, &pnio->rx_mc);
	if (ret) {
		pr_err("ultraserver election io memory allocation failed");
		goto done;
	}
	
	ret = mc_alloc_align(nd, MC_LIFESPAN_LOCAL, pnio->data_size, 0, MEM_LOC_HOST, 0, 0, 0, NEURON_MEMALLOC_TYPE_MISC_HOST, &pnio->data_mc);
	if (ret) {
		pr_err("ultraserver election io memory allocation failed");
		goto done;
	}

	ret = ndmar_queue_init(nd, pnio->eng_id, 0, pnio->ring_size, pnio->ring_size, pnio->tx_mc, pnio->rx_mc, NULL, 0, true);
	if (ret) {
		pr_err("pod election io queue init failed");
		goto done;
	}

done:
	return ret;
}

static void npe_pod_neighbor_io_destroy(pod_neighbor_io_t* pnio)
{
	ndmar_queue_release(pnio->nd, pnio->eng_id, 0);

	if (pnio->tx_mc) {
		mc_free(&pnio->tx_mc);
	}
	if (pnio->rx_mc) {
		mc_free(&pnio->rx_mc);
	}
	if (pnio->data_mc) {
		mc_free(&pnio->data_mc);
	}
}

/** 
 * npe_pod_neighbor_io_read_ack_completed()
 *
 *    Ack the read descriptors
 */
static void npe_pod_neighbor_io_read_ack_completed(struct ndma_eng *eng, int qid, u32 desc_cnt)
{
	struct udma_q *rxq, *txq;
	udma_q_handle_get(&eng->udma, qid, UDMA_TX, &txq);
	udma_q_handle_get(&eng->udma, qid, UDMA_RX, &rxq);
	udma_cdesc_ack(rxq, desc_cnt);
	udma_cdesc_ack(txq, desc_cnt);
}

/** 
 * npe_pod_neighbor_io_read()
 *
 *   Read miscram from a neighbor
 *
 */
static int npe_pod_neighbor_io_read(pod_neighbor_io_t* pnio, u32 *buf, u64 offset, u32 size)
{
	int ret;
	int i;
	int loop;
	u32 *data_va;
	struct ndma_eng   *eng   = &pnio->nd->ndma_engine[pnio->eng_id];
	struct ndma_queue *queue = &eng->queues[0];
	struct ndma_ring  *ring  = &queue->ring_info;
	u64 engid_2_b0_base[] = {V3_PCIE_B0_0_BASE, V3_PCIE_B0_1_BASE, V3_PCIE_B0_2_BASE, V3_PCIE_B0_3_BASE};
	u64 base_addr;
	
	// size check
	if (size + sizeof(*data_va) * 2 > pnio->data_size) {
		return -E2BIG;
	}
	
	data_va = (u32*)pnio->data_mc->va;	
	if (npe_pod_ctl_is_set(NPE_POD_CTL_SPOOF_BASE)) {
		base_addr = 0;
	} else {
		base_addr = engid_2_b0_base[pnio->eng_id / V3_NUM_DMA_ENG_PER_SENG];
	}

	// clear memory and setup completion data
	//	
	memset(data_va, 0, size + sizeof(*data_va) * 2);
	data_va[0] = 1;
	data_va[1] = 0;

	// create remote read descriptors
	//
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, base_addr + V3_APB_MISC_RAM_OFFSET + offset, 
			                        (pnio->data_mc->pa+8) | ndhal->ndhal_address_map.pci_host_base, size, UDMA_M2M_BARRIER_WRITE_BARRIER, false);

	if (ret) {
		pr_err("failed to create dma descriptor");
		return ret;
	}

	// create completion descriptor
	//
	ret = udma_m2m_copy_prepare_one(&eng->udma, ring->qid, 
									(pnio->data_mc->pa+0) | ndhal->ndhal_address_map.pci_host_base, 
									(pnio->data_mc->pa+4) | ndhal->ndhal_address_map.pci_host_base, 4, UDMA_M2M_BARRIER_WRITE_BARRIER, false);
	if (ret) {
		pr_err("failed to create dma descriptor");
		return ret;
	}
	mb();

	ret = udma_m2m_copy_start(&eng->udma, ring->qid, 2, 2);

	// loop waiting for completion
	//
	loop = 1000;
	for (i=0; i < loop; i++) {
		volatile u32 *dst = (volatile u32 *)(&data_va[1]);
		u32 dst_val = READ_ONCE(*dst);
		if (dst_val == 1) {
			// ack completed descriptors
			npe_pod_neighbor_io_read_ack_completed(eng, 0, 2);
			// copy out data		
			memcpy(buf, &data_va[2], size);
			return 0;
		}	
		udelay(4);
	}

	if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
		pr_info("nd%02d: neighbor read failed on eng: %d", pnio->nd->device_index, pnio->eng_id);
	}
	return -EIO;
}

static bool npe_election_timeout(void) 
{
	if (ktime_compare(ktime_get(), ndhal_pelect_data.nbr_data_read_deadline) > 0) {
		return true;
	}
	return false;
}

/** 
 * npe_read_neighbor_serial_numbers()
 *
 *   read neighbor serial numbers over b-links via dma
 *
 *   caller passes in a 2x2 matrix of pnio structs that represent the neighbor 
 *   pcie b link pairs.  Basically we read the serial numbers over the link
 *   pairs, verify the wiring is correct (link pair reads the same serial number from neighbor)
 *   and report the serial numbers back.
 *
 */
static int npe_read_neighbor_serial_numbers(pod_neighbor_io_t pnio[][2],  u64 * nbr_serial_number, volatile long unsigned int lr_neighbor_mask)
{
	int ret = 0;
	int retry_cnt = 0;
	struct neuron_device *nd = pnio[0][0].nd;
	volatile long unsigned int lr_neighbor_read_mask = 0;

	while (1) {
		int i,j;
		u32 tmp[2][2] = {0};  // temp buffer for storing serial number data read

		for (i=0; i<2; i++) {
			if (!test_bit(i, &lr_neighbor_mask)) continue;
			if (nbr_serial_number[i] != 0ull) continue;

			for (j=0; j<2; j++) {
				ret = npe_pod_neighbor_io_read(&(pnio[i][j]), tmp[j], FW_IO_REG_SERIAL_NUMBER_LO_OFFSET, sizeof(u64));
				if (ret || npe_pod_ctl_inject_link_error(i)) {
					pr_err("nd%02d: Read ultraserver neighbor serial number failed on %s link for seng %d\n", 
							nd->device_index, (i == 0) ? "left" : "right",  pnio[i][j].eng_id / V3_NUM_DMA_ENG_PER_SENG);
					if (!ret) {
						ret = -EIO;
					}
					goto done;
				} 
			}

			// check if we got valid data, if not, neighbor is likely not up yet so we'll retry
			//
			if ((tmp[0][0] != 0xdeadbeef) && (tmp[1][0] != 0xdeadbeef) && (tmp[0][0] != 0) && (tmp[1][0] != 0)) {
				if (memcmp(tmp[0], tmp[1], 8) != 0) {
					pr_err("nd%02d: Serial numbers on %s link pair don't match: %08x%08x vs.  %08x%08x\n", nd->device_index, 
							(i==0) ? "left" : "right", tmp[0][1], tmp[0][0], tmp[1][1], tmp[1][0]);
					ret = -EPIPE;
					goto done;
				}
				nbr_serial_number[i] = ((uint64_t)tmp[0][1])<<32 | (uint64_t)tmp[0][0];
				set_bit(i, &lr_neighbor_read_mask);
			}
		}

		if (lr_neighbor_read_mask == lr_neighbor_mask) {
			break;
		}

		// check for driver unload or canceled election
		if (npe_msleep_stoppable(NPE_RETRY_WAIT_MS)) {
			ret = -EINTR;
			goto done;
		}

		if (++retry_cnt % NPE_RETRY_WAIT_REPORTING_INTERVAL == 0) {
			pr_info("nd%02d: ultraserver waiting on neigbors", nd->device_index);
		}

		if (npe_election_timeout()) {
			pr_info("nd%02d: neighbor election data read timeout after %llu seconds", nd->device_index, ndhal_pelect_data.nbr_data_read_timeout);
			ret = -EINTR;
			goto done;
		}
	}
done:
	return ret;
}

/**
 * npe_read_neighbor_election_data()
 *
 *    read election data from neighbor, which is basically the serial numbers of their
 *    neighbors (which we need to determine connectivity)
 *
 */
static int npe_read_neighbor_election_data(pod_neighbor_io_t pnio[][2],  u64 nbr_serial_number_copy[][2], volatile long unsigned int lr_neighbor_mask)
{
	int ret = 0;
	int retry_cnt = 0;
	struct neuron_device *nd = pnio[0][0].nd;
	volatile long unsigned int lr_neighbor_read_mask = 0;

	memset(nbr_serial_number_copy, 0, sizeof(nbr_serial_number_copy[0])*2);

    while (1) {
		int i;
		int last_valid_word = test_bit(1, &lr_neighbor_mask) ? 3 : 1;
		u32 tmp[4] = {0};

        for (i=0; i<2; i++) {
			if (!test_bit(i, &lr_neighbor_mask)) continue;
			ret = npe_pod_neighbor_io_read(&(pnio[i][0]), tmp, FW_IO_REG_LH_NEIGHBOR_SERNUM_HI, sizeof(tmp));
			if (ret || npe_pod_ctl_inject_link_error(i)) {
            	pr_err("nd%02d: Read ultraserver neighbor election data failed for seng link %d\n", nd->device_index, pnio[i][0].eng_id / V3_NUM_DMA_ENG_PER_SENG);
				if (!ret) {
					ret = -EIO;
				}
				goto done;
            } 

			// check if last word of the copy is valid (for left only this is tmp[1], for both/right this is tmp[3])
			// TODO this is pretty klunky just make this a function that takes lr_neighbor_mask to figure out
			// last valid word.
			//
            if ((tmp[last_valid_word] != 0) && (tmp[last_valid_word] != 0xdeadbeef)) {
                nbr_serial_number_copy[i][0] = ((uint64_t)tmp[0] << 32) | (uint64_t)tmp[1];
                nbr_serial_number_copy[i][1] = ((uint64_t)tmp[2] << 32) | (uint64_t)tmp[3];
				set_bit(i, &lr_neighbor_read_mask);
			}
        }

		// got both neighbors election data
		//
		if (lr_neighbor_read_mask == lr_neighbor_mask) {
			break;
		}

		// check for driver unload or canceled election
		if (npe_msleep_stoppable(NPE_RETRY_WAIT_MS)) {
			ret = -EINTR;
			goto done;
		}

		if (++retry_cnt % NPE_RETRY_WAIT_REPORTING_INTERVAL == 0) {
			pr_info("nd%02d: ultraserver waiting on neigbors to update election data", nd->device_index);
		}

		if (npe_election_timeout()) {
			pr_info("nd%02d: neighbor election data read timeout after %llu seconds", nd->device_index, ndhal_pelect_data.nbr_data_read_timeout);
			ret = -EINTR;
			goto done;
		}
	}

done:
	return ret;
}

/**
 * npe_read_neighbor_read_election_status()
 *
 *    read the neighbor's election status - indicator that their node has successfully completed
 *    the election process
 */
static int npe_read_neighbor_read_election_status(pod_neighbor_io_t pnio[][2], u32 *election_status, u32 prev_election_status, volatile long unsigned int lr_neighbor_mask)
{
	int ret = 0;
	int retry_cnt = 0;
	struct neuron_device *nd = pnio[0][0].nd;
	volatile long unsigned int lr_neighbor_read_mask = 0;

    memset(election_status, 0, sizeof(u32)*2);

    while (1) {
		int i;
		u32 tmp = {0};

        for (i=0; i<2; i++) {
			if (!test_bit(i, &lr_neighbor_mask)) continue;
			ret = npe_pod_neighbor_io_read(&(pnio[i][0]), &tmp, FW_IO_REG_POD_ELECTION_STS, sizeof(tmp));
            if (ret) {
            	pr_err("nd%02d: Read ultraserver neighbor election status failed for seng link %d\n", nd->device_index, pnio[i][0].eng_id / V3_NUM_DMA_ENG_PER_SENG);
				goto done;
            } 

			// extract status from status/node_id pair
			tmp = NPE_MR_STS_GET(tmp);

			// check if status is valid
			//
            if ((tmp != prev_election_status) && (tmp != 0xdeadbeef)) {
                election_status[i] = tmp;
				set_bit(i, &lr_neighbor_read_mask);
			}
        }

		// got both neighbor's new election status?   This has a unimportant race with prev election status explained in the header of this file. 
		//
		if (lr_neighbor_read_mask == lr_neighbor_mask) {
			break;
		}

		// check for driver unload or canceled election
		if (npe_msleep_stoppable(NPE_RETRY_WAIT_MS)) {
			// Note: This could possibly leave our neighbors in pod state while this node is not in pod state as it's logically an ABORT transaction.
			ret = -EINTR;
			goto done;
		}

		if (++retry_cnt % NPE_RETRY_WAIT_REPORTING_INTERVAL == 0) {
			pr_info("nd%02d: ultraserver waiting on neigbor to update status", nd->device_index);
		}
	}

done:
	return ret;
}

static int npe_check_election_results(struct neuron_device *nd, u32 *nbr_election_status, volatile long unsigned int lr_neighbor_mask)
{
	int ret = 0;
	int i;

    for (i=0; i<2; i++) {
		if (!test_bit(i, &lr_neighbor_mask)) continue;
		if (nbr_election_status[i] != NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS) {
			pr_err("nd%02d: election failed. %s neighbor reported bad election status", nd->device_index, (i == 0) ? "left" : "right");
			ret = -ENODEV;
		}
	}
	return ret;
}

static u32 npe_miscram_read(struct neuron_device *nd, u64 offset)
{
	return readl(nd->npdev.bar0 + V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + offset);
}

static void npe_miscram_write(struct neuron_device *nd, u64 offset, u32 data)
{
	writel(data, nd->npdev.bar0 + V3_MMAP_BAR0_APB_IO_0_MISC_RAM_OFFSET + offset);
}

static void npe_miscram_neighbor_election_data_set(struct neuron_device *nd, u64 lh_val, u64 rh_val)
{
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_HI, (lh_val>>32) & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_LO, lh_val & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_HI, (rh_val>>32) & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_LO, rh_val & 0xffffffff);
}

static void npe_miscram_neighbor_election_data_clr(struct neuron_device *nd)
{
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_LO, 0);
	npe_miscram_write(nd, FW_IO_REG_RH_NEIGHBOR_SERNUM_HI, 0);
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_LO, 0);
	npe_miscram_write(nd, FW_IO_REG_LH_NEIGHBOR_SERNUM_HI, 0);
}

static void npe_miscram_sts_info_set(struct neuron_device *nd, enum neuron_pod_election_mr_sts sts, u32 lr_mask, int node_id, int node_cnt)
{
	npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NPE_MR_STS_SET(sts, lr_mask, node_id, node_cnt));
}

static void npe_miscram_sts_info_get(struct neuron_device *nd, enum neuron_pod_election_mr_sts *sts, u32 *lr_mask, int *node_id, int *node_cnt)
{
	u32 tmp;
	tmp = npe_miscram_read(nd, FW_IO_REG_POD_ELECTION_STS);
	*sts = NPE_MR_STS_GET(tmp);
	*lr_mask = NPE_MR_LR_MASK_GET(tmp);
	*node_id = NPE_MR_NODE_ID_GET(tmp);
	*node_cnt = NPE_MR_NODE_CNT_GET(tmp);
}

static void npe_miscram_pod_sernum_set(struct neuron_device *nd, u64 pod_sernum)
{
	npe_miscram_write(nd, FW_IO_REG_POD_SERNUM_HI, (pod_sernum>>32) & 0xffffffff);
	npe_miscram_write(nd, FW_IO_REG_POD_SERNUM_LO, pod_sernum & 0xffffffff);
}

static void npe_miscram_pod_sernum_get(struct neuron_device *nd, u64 *pod_sernum)
{
	*pod_sernum = ((u64)npe_miscram_read(nd, FW_IO_REG_POD_SERNUM_HI))<<32 | (u64)npe_miscram_read(nd, FW_IO_REG_POD_SERNUM_LO);
}

/** npe_get_node_id() - determine node id and return it along w/ node cnt and pod serial number
 *
 *    determine node id of a group of nodes connected in a ring topology.
 *    Lowest node id is leader (0). For a 4 node group, Leader's two neighbors 1 & 2.  The
 *    Lower of the Leader's two neighbors is 1.  Node ids 1 & 2 will
 *    have their die addressing flipped.  A 2 node topology is explained below in the code.
 *
 */
static int npe_get_node_id(u64 self, u64 left, u64 right, u64 diagonal, int * node_cnt, u64 *pod_serial_number)
{
	// check if we are in a 2 node cluster and run a simplified election.  There are three link
	// connectivity/topology possibilities for a 2 node cluster.  The two nodes are connected
	// by right link only, left only, or each 2 node pair is connected in a separate ring (left == right).
	//
	if ((ndhal_pelect_data.lr_mask != 0x3) || (left == right)) {
		u64 neighbor = (ndhal_pelect_data.lr_mask == 0x2) ? right : left;

		*node_cnt = 2;
		if (self < neighbor) {
			*pod_serial_number = self;
			return 0;
		} else {
			*pod_serial_number = neighbor;
			return 1;
		}
	}

	*node_cnt = 4;
	if (self < diagonal) {
        // My diagonal node is not a leader.
		if ((self < left) && (self < right)) {
			*pod_serial_number = self;
            // I'm a leader
			return 0;
		}
    }  else {
    /* my diagonal node is a leader or not */
        if ((diagonal < left) && (diagonal < right)) {
            *pod_serial_number = diagonal;
            /* my diagonal is a leader. I'm 2. */
            return 2;
        }
    }
    if (left < right) {
        //the other node in the same rack is a leader.
        *pod_serial_number = left;
        return 1;
    }
    /* the lead is in the different rack. */
    *pod_serial_number = right;
	return 3;
}

/** 
 * npe_primary_device_do_election() - exec election on primary node
 *
 *   @nd                 - device the election is being prosecuted on
 *   @secondary_good_cnt - count of secondary devices that passed link checks
 *   @lr_neighbor_mask   - mask of left/right neighbor links to use for election
 *
 */
static int npe_primary_device_do_election(struct neuron_device *nd, int secondary_good_cnt, volatile long unsigned int lr_neighbor_mask)
{
	int ret;
	int i;
	int node_id = -1;
	int node_cnt = 0;
	u32 routing_id;
	u64 serial_number;
	u64 diagonal;
	u64 pod_serial_number = 0;
	u64 nbr_serial_number[2] = {0};
	u64 nbr_serial_number_copy[2][2] = {0};
	u32 nbr_election_status[2] = {0};
	pod_neighbor_io_t pnio[2][2] = {0};

	pr_info("nd%02d: ultraserver election starting", nd->device_index);

	// Read local routing id and serial number
	//
	ret = fw_io_device_id_read(nd->npdev.bar0, &routing_id);
	if (ret) {
		pr_err("nd%02d: local routing id read failed", nd->device_index);
		goto done;
	}
	ret = fw_io_serial_number_read(nd->npdev.bar0, &serial_number);
	if (ret) {
		pr_err("nd%02d: local serial number read failed", nd->device_index);
		goto done;
	}

	pr_info("nd%02d: Routing id: %02d SerialNumber %016llx", nd->device_index, routing_id, serial_number);

	// Initialize neighbor io structures
	// Left
	ret = npe_pod_neighbor_io_init(&(pnio[0][0]), nd, 36);
	ret |= npe_pod_neighbor_io_init(&(pnio[0][1]), nd, 68);
	// Right
	ret |= npe_pod_neighbor_io_init(&(pnio[1][0]), nd, 4);
	ret |= npe_pod_neighbor_io_init(&(pnio[1][1]), nd, 100);
	if (ret) {
		pr_err("neighbor io initialization failed");
		goto done;
	}

	// clear election status before we start
	//
	npe_miscram_sts_info_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_INIT, 0, -1, 0);
	
	// read neighbors serial numbers
	// 
	ret = npe_read_neighbor_serial_numbers(pnio,  nbr_serial_number, lr_neighbor_mask);
	if (ret || npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_PRI_LNK_FAIL)) {
		goto done;
	}

	pr_info("nd%02d: acquired ultraserver neighbor serial numbers L: %016llx  R:  %016llx", nd->device_index, nbr_serial_number[0], nbr_serial_number[1]);

	// update LH/RH neighbor unique values in our local miscram copy 
	//
	npe_miscram_neighbor_election_data_set(nd, nbr_serial_number[0], nbr_serial_number[1]);

	if (npe_pod_ctl_is_set(NPE_POD_CTL_SKIP_NBR_CPY_READ)) {
		goto done;
	}

	// read neighbor's LH/RH copy of serial numbers to determine if neighbors successfully picked up both neighbors serial numbers
	// indicating we have a ring on the primary
	// 
	ret = npe_read_neighbor_election_data(pnio, nbr_serial_number_copy, lr_neighbor_mask);
	if (ret) {
		goto done;
	}

	//  check cabling if we are using both link pairs
	// 
	if (lr_neighbor_mask == 0x3) {
		for (i=0; i<2; i++) {
			if (nbr_serial_number_copy[i][0] <= 15) {
				u32 dev_id = (u32)(nbr_serial_number_copy[i][0]);
				pr_err("nd%02d: %s ultraserver link is miss-wired to nd%02d (%016llx)",
						nd->device_index, (i==0) ? "left":"right", (dev_id > 15) ? 0 : dev_id, nbr_serial_number[i]);
				ret = -EPIPE;
			}
		}
		if (ret) {
			goto done;
		}
	}

	diagonal = nbr_serial_number_copy[0][1];

	//  secondaries all good?
	// 
	if (secondary_good_cnt != 15) {
		pr_err("Only %d out of 15 secondary devices reported good links", secondary_good_cnt);
		ret = -EPIPE;
		goto done;
	}
	
	pr_info("nd%02d: ultraserver election - all secondary links good", nd->device_index);

	// determine our node id node cnt and pod serial number
	//
	node_id = npe_get_node_id(serial_number, nbr_serial_number[0], nbr_serial_number[1], diagonal, &node_cnt, &pod_serial_number);
	ret = 0;

	// set election status, with bad node id
	//
	npe_miscram_sts_info_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS, 0, -1, 0);

	// read neighbor's election status
	//	
	ret = npe_read_neighbor_read_election_status(pnio, nbr_election_status, NEURON_NPE_POD_ELECTION_MR_STS_INIT, lr_neighbor_mask);
	if (ret) {
		goto done;
	}

	// check neighbor's election status
	//
	if (npe_check_election_results(nd, nbr_election_status, lr_neighbor_mask)) {
		ret = -ENODEV;
		goto done;
	}

done:
	npe_pod_neighbor_io_destroy(&(pnio[0][0]));
	npe_pod_neighbor_io_destroy(&(pnio[0][1]));
	npe_pod_neighbor_io_destroy(&(pnio[1][0]));
	npe_pod_neighbor_io_destroy(&(pnio[1][1]));

	// clear neighbor election data to prevent partial elections
	//
	npe_miscram_neighbor_election_data_clr(nd);

	// update node_id and pod_serial_number, even in the failed case.
	//
	if (ret) {
		npe_miscram_sts_info_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_FAILURE, 0, -1, 0);
		npe_miscram_pod_sernum_set(nd, 0);
		ndhal_pelect_data.node_id = -1;
		ndhal_pelect_data.node_cnt = 0;
		ndhal_pelect_data.pod_serial_num = 0;
	} else {
		npe_miscram_sts_info_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS, lr_neighbor_mask, node_id, node_cnt);
		npe_miscram_pod_sernum_set(nd, pod_serial_number);
		ndhal_pelect_data.node_id = node_id;
		ndhal_pelect_data.node_cnt = node_cnt;
		ndhal_pelect_data.pod_serial_num = pod_serial_number;
	}

	return ret;
}

/** 
 * npe_secondary_device_vet()
 *
 *   Check the secondary's neighbors to see if:
 *   -  the links are connected correctly 
 *   - they have the same device id
 *   and report success or failure.
 *
 *   Vetting is currently done from a single thread (serialized).
 *
 *   @nd               - device the election is being prosecuted on
 *   @lr_neighbor_mask - mask of left/right neighbor links to use for election
 *
 */
static int npe_secondary_device_vet(struct neuron_device *nd, volatile long unsigned int lr_neighbor_mask)
{
	int ret;
	int i;
	u64 nbr_serial_number[2] = {0};
	u64 nbr_serial_number_copy[2][2] = {0};
	u32 nbr_election_status[2] = {0};
	pod_neighbor_io_t pnio[2][2] = {0};

	// Initialize neighbor io structures
	// Left
	ret = npe_pod_neighbor_io_init(&(pnio[0][0]), nd, 36);
	ret |= npe_pod_neighbor_io_init(&(pnio[0][1]), nd, 68);
	// Right
	ret |= npe_pod_neighbor_io_init(&(pnio[1][0]), nd, 4);
	ret |= npe_pod_neighbor_io_init(&(pnio[1][1]), nd, 100);

	if (ret) {
		pr_err("nd%02d: neighbor io initialization failed", nd->device_index);
		goto done;
	}

	// clear election status before we start, read neighbor serial numbers, 
	// check link pairs are wired correctly and populate election data
	//
	npe_miscram_write(nd, FW_IO_REG_POD_ELECTION_STS, NEURON_NPE_POD_ELECTION_MR_STS_INIT);

	// if neighbor reads are good the link is good (or at least wired in pairs)
	//
	ret = npe_read_neighbor_serial_numbers(pnio,  nbr_serial_number, lr_neighbor_mask);
	if (ret) {
		goto done;
	}

	if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
		pr_info("nd%02d: acquired ultraserver neighbor serial numbers L: %016llx  R:  %016llx", nd->device_index, nbr_serial_number[0], nbr_serial_number[1]);
	}

	// populate election data with device index to detect miss-cabling
	//
	npe_miscram_neighbor_election_data_set(nd, nd->device_index, nd->device_index);

	// read neighbor election data and check if R/L links match, then update status
	//
	ret = npe_read_neighbor_election_data(pnio, nbr_serial_number_copy, lr_neighbor_mask);
	if (ret) {
		goto done;
	}

	if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
		pr_info("nd%02d: read ultraserver neighbor election data", nd->device_index);
	}

	//  check cabling
	//
	for (i=0; i<2; i++) {
		if (!test_bit(i, &lr_neighbor_mask)) continue;
		if (nbr_serial_number_copy[i][0] != nd->device_index) {
			u32 dev_id = (u32)(nbr_serial_number_copy[i][0]);
			pr_err("nd%02d: %c ultraserver link is miss-wired to nd%02d (%016llx)", 
					nd->device_index, (i==0) ? 'L':'R', (dev_id > 15) ? 0 : dev_id, nbr_serial_number[i]);
			ret = -EPIPE;
		}
	}

	// set election status, check neighbor's election status, and
	// clear election data (but not election status) from miscram.
	// clearing miscram election data ensures subsequent elections
	// must be prosecuted on all participating nodes .
	//
	npe_miscram_sts_info_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS, 0, 0, 0);
	
	// check status and clear election data.
	ret = npe_read_neighbor_read_election_status(pnio, nbr_election_status, NEURON_NPE_POD_ELECTION_MR_STS_INIT, lr_neighbor_mask);
	if (ret) {
		goto done;
	}

	if (npe_check_election_results(nd,  nbr_election_status, lr_neighbor_mask)) {
		ret = -ENODEV;
		goto done;
	}

done:
	if (!ret && npe_pod_ctl_is_set(NPE_POD_CTL_FAULT_SEC_LNK_FAIL)) {
		ret = -EPIPE;
	}

	npe_pod_neighbor_io_destroy(&(pnio[0][0]));
	npe_pod_neighbor_io_destroy(&(pnio[0][1]));
	npe_pod_neighbor_io_destroy(&(pnio[1][0]));
	npe_pod_neighbor_io_destroy(&(pnio[1][1]));

	// clear neighbor election data to prevent partial elections
	//
	npe_miscram_neighbor_election_data_clr(nd);

	if (ret) {	
		// clear election data in miscram so next election will sequence through the
		// steps above w/o using stale data.
		//
		npe_miscram_neighbor_election_data_clr(nd);
		npe_miscram_sts_info_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_FAILURE, 0, 0, 0);
	}
	return ret;
}

/**
 * npe_initiate_election()
 *
 *   change election state to in progress and kick the election thread unless election is 
 *   already in progress.
 *   This must be called with the lock held
 */
static void npe_initiate_election(u64  nbr_data_read_timeout)
{
	if (ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS) {
		pr_info("initiating ultraserver election.  State: %d", ndhal_pelect_data.pod_state_internal);
		ndhal_pelect_data.kill_election = false;
		ndhal_pelect_data.nbr_data_read_deadline = ktime_add(ktime_get(), ms_to_ktime(nbr_data_read_timeout));
		ndhal_pelect_data.nbr_data_read_timeout = nbr_data_read_timeout;
		ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS;
		ndhal_pelect_data.node_id = -1;
		ndhal_pelect_data.node_cnt = 0;
		ndhal_pelect_data.pod_serial_num = 0;
		ndhal_pelect_data.lr_mask = ndhal_pelect_data.lr_mask_default;
		wake_up(&ndhal_pelect_data.wait_queue);
	}
}

/**
 * npe_all_rst_complete()
 *
 *   returns true if all devices have been successfully reset
 *   indicated by the npe module having a cached copy of all
 *   neuron device pointers.
 */
static bool npe_all_rst_complete(void)
{
	int i;
	for (i=0; i < 16; i++) {
		if (ndhal_pelect_data.pnd[i] == NULL) {
			return false;
		}
	}
	return true;
}

/** 
 * npe_election_exec_on_rst() 
 *
 *   Populdate the pod code's neuron device table.  Then if we are in the 
 *   initial state and election post reset was request, star the election process.
 *
 *
 */
int npe_election_exec_on_rst(struct neuron_device *nd, bool reset_successful)
{
	enum neuron_pod_election_mr_sts sts; 
	int node_id;
	int node_cnt;
	u32 lr_neighbor_mask;
	u64 pod_serial_number;
	
	if (!reset_successful) {
		return 0;
	}

	mutex_lock(&ndhal_pelect_data.lock);

	// populate the device
	//
	ndhal_pelect_data.pnd[nd->device_index] = nd;

	// Device 0 is the primary actor in the election/topology discovery process, so 
	// when we process Device 0 reset completions, we need to do some bookkeeping.
	//
	if (nd->device_index == 0) {
		// Prior election results are cached in miscram, for testing purposes, 
		// we can clear the results through a module parameter, allowing us
		// to ignore the cached results.
		//
		if (npe_pod_ctl_is_set(NPE_POD_CTL_CLR_MISCRAM)) {
			pr_info("clearing miscram ultraserver election results");
			npe_miscram_neighbor_election_data_clr(nd);
			npe_miscram_sts_info_set(nd, NEURON_NPE_POD_ELECTION_MR_STS_INIT, 0, 0, 0);
			npe_miscram_pod_sernum_set(nd, 0);
		}

		// If there are cached election results, use them in lieu of running a new election.
		//
		npe_miscram_sts_info_get(nd, &sts, &lr_neighbor_mask, &node_id, &node_cnt);
		npe_miscram_pod_sernum_get(nd, &pod_serial_number);

		if (sts == NEURON_NPE_POD_ELECTION_MR_STS_SUCCESS) {
			ndhal_pelect_data.lr_mask = lr_neighbor_mask;
			ndhal_pelect_data.node_id = node_id;
			ndhal_pelect_data.node_cnt = node_cnt;
			ndhal_pelect_data.pod_serial_num = pod_serial_number;
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_SUCCESS;
			goto done;
		}

		// Check if module parameter has been set to skip election at driver load (post device reset). 
		// Primarily used for testing.
		//
		if (npe_pod_ctl_is_set(NPE_POD_CTL_RST_SKIP_ELECTION)) {
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;  
			goto done;
		}
	}
	
	// if we aren't kicking off election on first driver reset (testing) or 
	// if we aren't in init state then we've already made an election decision.
	//
	if ((ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_INIT) || npe_pod_ctl_is_set(NPE_POD_CTL_RST_SKIP_ELECTION)) {
		goto done;
	}

	// if all devices are done with reset, start the election.
	//
	if (!npe_all_rst_complete()) {
			goto done;
	}

	npe_initiate_election(ndhal_pelect_data.nbr_data_read_timeout);

done:
	mutex_unlock(&ndhal_pelect_data.lock);
	return 0;
}

/**
 * npe_notify_mark
 *
 *   crwl mark/unmark operations represent a change in core ownership
 *   which is tracked in the crwl mark table.  This function is called
 *   every time there is an ownership change.
 *
 *   The election code currently uses mark notifications to clear
 *   "SINGLE_NODE" behavior which asks the election code to
 *   temporarily suppresses election results until all cores
 *   have been released (unmarked).
 */
void npe_notify_mark(int mark_cnt, bool mark)
{
	mutex_lock(&ndhal_pelect_data.lock);
	if (!mark && (mark_cnt == 0)) {
		ndhal_pelect_data.mode = NEURON_ULTRASERVER_MODE_UNSET;
	}
	mutex_unlock(&ndhal_pelect_data.lock);
}

/**
 * npe_pod_state_busy()
 *
 *     generic busy check.  Used to check if election data is in a valid state.
 */
static bool npe_pod_state_busy(void)
{
	return ((ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS) ||
			(ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_INIT));
}

/**
 * npe_get_modal_node_id()
 *
 *    return node_id based on specified operating mode.
 *
 */
static int npe_get_modal_node_id(enum neuron_ultraserver_mode mode)
{
	int node_id = ndhal_pelect_data.node_id;

	switch (mode) {
		case NEURON_ULTRASERVER_MODE_UNSET:
			break;

		case NEURON_ULTRASERVER_MODE_X4:
			if (ndhal_pelect_data.node_cnt != 4) {
				node_id = -1;
			}
			break;

		case NEURON_ULTRASERVER_MODE_X2H:
			if (ndhal_pelect_data.node_cnt == 4) {
				static const int x4hmap[] = {0,0,1,1};  // remapping node ids on x4 using H links
				node_id = x4hmap[node_id];
			}
			break;

		case NEURON_ULTRASERVER_MODE_X2V:
			if (ndhal_pelect_data.node_cnt == 4) {
				static const int x4vmap[] = {0,1,0,1}; //remapping node ids on x4 using V links
				node_id = x4vmap[node_id];
			}
			break;

		case NEURON_ULTRASERVER_MODE_X1:
			node_id = -1;
			break;

		default:
			pr_err("invalid ultraserver_mode %d", mode);
			node_id = -1;
			break;
	}
	if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
		pr_info("modal node id:  %d", node_id);
	}
	return node_id;
}

/**
 * npe_get_modal_node_cnt()
 *
 *    return node_cnt based on specified operating mode.
 *
 */
static u32 npe_get_modal_node_cnt(enum neuron_ultraserver_mode mode)
{
	u32 node_cnt = ndhal_pelect_data.node_cnt;

	switch (mode) {
		case NEURON_ULTRASERVER_MODE_UNSET:
			break;

		case NEURON_ULTRASERVER_MODE_X4:
			if (node_cnt != 4) {
				node_cnt = 0;
			}
			break;

		case NEURON_ULTRASERVER_MODE_X2H:
		case NEURON_ULTRASERVER_MODE_X2V:
			if (node_cnt == 4) {
				node_cnt = 2;
			}
			break;

		case NEURON_ULTRASERVER_MODE_X1:
			node_cnt = 0;
			break;

		default:
			pr_err("invalid ultraserver_mode %d", mode);
			node_cnt = 0;
			break;
	}
	return node_cnt;
}

/**
 * npe_get_modal_serial_number()
 *
 *    return serial number based on specified operating mode.
 *
 */
static u64 npe_get_modal_serial_number(enum neuron_ultraserver_mode mode)
{
	u64 pod_serial_number = ndhal_pelect_data.pod_serial_num;

	switch (mode) {
		case NEURON_ULTRASERVER_MODE_UNSET: 
			break;

		case NEURON_ULTRASERVER_MODE_X4:
			if (ndhal_pelect_data.node_cnt != 4) {
				pod_serial_number = 0;
			}
			break;

		case NEURON_ULTRASERVER_MODE_X2H:
			if (ndhal_pelect_data.node_cnt == 4) {
				static const bool adj_serial_number[] = {false, true, false, true};
				if (adj_serial_number[ndhal_pelect_data.node_id]) {
					pod_serial_number -= 1;
				}
			}
			break;

		case NEURON_ULTRASERVER_MODE_X2V:
			if (ndhal_pelect_data.node_cnt == 4) {
				static const bool adj_serial_number[] = {false, false, true, true};
				if (adj_serial_number[ndhal_pelect_data.node_id]) {
					pod_serial_number -= 1;
				}
			}
			break;

		case NEURON_ULTRASERVER_MODE_X1:
			pod_serial_number = 0;
			break;

		default:
			pr_err("invalid ultraserver_mode %d", mode);
			break;
	}

	return pod_serial_number;
}

/**
 * npe_mode_is_supported()
 *
 *    Determines what is supported based on node count and link state.
 *    On a ultraserver w/ 4 good links, x2 mode is only allowed on the
 *    vertical links (since we have to report 2x2 information ins sysfs.
 *
 */
static bool npe_mode_is_supported(enum neuron_ultraserver_mode mode)
{
	switch (mode) {
		case NEURON_ULTRASERVER_MODE_X4:
			return (ndhal_pelect_data.node_cnt == 4);
		case NEURON_ULTRASERVER_MODE_X2V:
			return ((ndhal_pelect_data.node_cnt >= 2) && (ndhal_pelect_data.lr_mask & 0x1));
		case NEURON_ULTRASERVER_MODE_X2H:
			return ((ndhal_pelect_data.node_cnt == 2) && (ndhal_pelect_data.lr_mask & 0x2));
		case NEURON_ULTRASERVER_MODE_X1:
			return true;
		case NEURON_ULTRASERVER_MODE_UNSET:
		default:
			break;
	}
	return false;
}

/**
 * npe_get_pod_id()
 *
 *  return the pod id.  This is only valid if the election is complete
 *  (either successful or failed)
 */
int npe_get_pod_id(u8 *pod_id)
{
	u64 serial_number = npe_get_modal_serial_number(ndhal_pelect_data.mode);

	memcpy(pod_id, &serial_number, sizeof(serial_number));
	return 0;
}

/**
 * npe_get_pod_sz()
 *
 *  return the pod size.  This is only valid if the election is complete
 *  (either successful or failed)
 */
int npe_get_pod_sz(u8 *pod_sz)
{
	*pod_sz = npe_get_modal_node_cnt(ndhal_pelect_data.mode);
	return 0;
}

/**
 * npe_get_pod_mode()
 *
 *  return the current operating mode
 */
int npe_get_pod_mode(enum neuron_ultraserver_mode *mode)
{
	*mode = ndhal_pelect_data.mode;
	return 0;
}

/*
 * npe_get_pod_modes_supported()
 *
 *  return supported operating modes
 */
int npe_get_pod_modes_supported(u32 *modes_supported)
{
	enum neuron_ultraserver_mode mode;

	*modes_supported = 0;
	for (mode = NEURON_ULTRASERVER_MODE_X4; mode <= NEURON_ULTRASERVER_MODE_X1; mode++) {
		if (npe_mode_is_supported(mode)) {
			*modes_supported |= (1<<mode);
		}
	}
	return 0;
}

/**
 * _npe_get_pod_status()
 *
 *    return state of the pod along w/ node id.
 */
static int _npe_get_pod_status(u32 *state, s8 *node_id)
{
	int ret = 0;

	switch (ndhal_pelect_data.pod_state_internal) {
		case NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS:
			*state = NEURON_POD_E_STATE_IN_PROGRESS;
			ret = -EBUSY;
			break;

		case NEURON_NPE_POD_ST_ELECTION_SUCCESS:
			*state = NEURON_POD_E_STATE_ULTRASERVER;
			if (ndhal_pelect_data.mode == NEURON_ULTRASERVER_MODE_X1) {
				*state = NEURON_POD_E_STATE_SINGLE_NODE;
			}
			break;

		case NEURON_NPE_POD_ST_INIT:
			*state = NEURON_POD_E_STATE_IN_PROGRESS;
			ret = -EBUSY;
			break;

		case NEURON_NPE_POD_ST_ELECTION_FAILURE:
			*state = NEURON_POD_E_STATE_SINGLE_NODE;
			break;

		default: 
			ret = -EINVAL;
			break;
	}
	*node_id = npe_get_modal_node_id(ndhal_pelect_data.mode);
	return ret;	
}

int npe_get_pod_status(u32 *state, u8 *node_id)
{
	int ret;
	mutex_lock(&ndhal_pelect_data.lock);
	ret = _npe_get_pod_status(state, node_id);
	mutex_unlock(&ndhal_pelect_data.lock);
	return ret;
}

/**
 * npe_pod_ctrl() - on-demand request to change pod state or operational mode
 *
 *   From nrt_init() this function is called to control UltraServer configuration. It can
 *   kill and in progress election, request an on-demand election, or set an operating mode.
 *
 *   - To request an on-demand election, no cores can be owned (marked).
 *   - A kill request can be done at any time.
 *   - Setting an operating mode allows an application pick the way it wants to use the
 *     ultraserver.  For example, an ultraserver with 4 good links can be run as a x4, 2x2,
 *     1x2 + 2x1 or 4x1.
 *
 */
int npe_pod_ctrl(struct neuron_device *nd, u32 ctrl, enum neuron_ultraserver_mode mode, u32 timeout, u32 *state)

{
	int ret = 0;
	u8 node_id;

	mutex_lock(&ndhal_pelect_data.lock);

	// convert legacy single node mode request into new mode set
	//
	if (ctrl == NEURON_NPE_POD_CTRL_REQ_SINGLE_NODE) {
		ctrl = NEURON_NPE_POD_CTRL_SET_MODE;
		mode = NEURON_ULTRASERVER_MODE_X1;
	}

	if (ctrl == NEURON_NPE_POD_CTRL_REQ_KILL) {
		if (ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS) {
			ndhal_pelect_data.kill_election = true;
		} else if (ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_INIT) {
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
		}

	} else if (ctrl == NEURON_NPE_POD_CTRL_SET_MODE) {
		if (npe_pod_state_busy()) {
			ret = -EBUSY;
			goto done;
		}
		if ((ndhal_pelect_data.mode != NEURON_ULTRASERVER_MODE_UNSET) && (mode != ndhal_pelect_data.mode)) {
			pr_err("trying to override exiting mode setting %d with mode setting %d", ndhal_pelect_data.mode, mode);
			ret = -EEXIST;
			goto done;
		}
		if (!npe_mode_is_supported(mode)) {
			pr_info("current ultraserver configuration setting does not support requested mode %d", mode);
			ret = -ENOTSUPP;
			goto done;
		}
		ndhal_pelect_data.mode = mode;

	} else if (ctrl == NEURON_NPE_POD_CTRL_REQ_POD) {
		int mark_cnt = ncrwl_range_mark_cnt_get();

		if ((mark_cnt == 0) && npe_all_rst_complete()) {
			npe_initiate_election(timeout * 1000);
			ret = 0;
		} else {
			pr_info("ultraserver Election request failed. %d Neuron cores are still in use or not all devices are reset. Election can only be initiated when all cores are free and all devices are reset", 
					mark_cnt);
			ret = -EAGAIN;
		}
	} else {
		pr_err("Invalid ultraserver control request value %d", ctrl);
		ret = -EINVAL;
	}

done:
	_npe_get_pod_status(state, &node_id);
	mutex_unlock(&ndhal_pelect_data.lock);
	return ret;
}

static int npe_election_thread_fn(void *arg)
{
	int ret;
	int i;
	int secondary_good_cnt = 0;

	while (!kthread_should_stop() && !ndhal_pelect_data.stop) {
		
retry:
		wait_event_interruptible(ndhal_pelect_data.wait_queue, ndhal_pelect_data.pod_state_internal == NEURON_NPE_POD_ST_ELECTION_IN_PROGRESS || ndhal_pelect_data.stop);
		if (kthread_should_stop() || ndhal_pelect_data.stop)
			break;

		// Cheap sanity check to make sure we don't allow a election to commence unless all devices have been reset.
		// There are checks in pod_ctrl logic to prevent this, but no harm in having a sanity check.
		//
		if (!npe_all_rst_complete()) {
			WARN_ONCE(1, "ultraserver election attempted while some devices have not completed reset");
			mutex_lock(&ndhal_pelect_data.lock);
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
			mutex_unlock(&ndhal_pelect_data.lock);
			goto retry;
		}

		pr_info("ultraserver election in progress.  State: %d", ndhal_pelect_data.pod_state_internal);
	
		// prosecute the election
		//
		if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
			pr_info("ultraserver vetting secondaries. Internal state: %d", ndhal_pelect_data.pod_state_internal);
		}

		// vet secondaries
		//
		secondary_good_cnt = 0;
		for (i = 1; i < 16; i++) {
			ret = npe_secondary_device_vet(ndhal_pelect_data.pnd[i], ndhal_pelect_data.lr_mask);
			if (!ret) {
				secondary_good_cnt++;
			}
		}

		if (npe_pod_ctl_is_set(NPE_POD_CTL_VERBOSE)) {
			pr_info("secondary good cnt: %d", secondary_good_cnt);
		}

		// run election on primary
		//
		ret = npe_primary_device_do_election(ndhal_pelect_data.pnd[0], secondary_good_cnt, ndhal_pelect_data.lr_mask);

		// update and report election results.
		//    If we tried the election over both links and it failed, try again on the right links (2x2 config)
		//    Note, we could also try a 3rd time on the left links
		//
		mutex_lock(&ndhal_pelect_data.lock);
		if (ret) {
			switch (ndhal_pelect_data.lr_mask) {
				case 0x1: // election tried on Left link
					pr_info("nd%02d: ultraserver election failed", ndhal_pelect_data.pnd[0]->device_index);
					ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
					break;
				case 0x2: // election tried on right link
					// retry a lame duck election with left link only
					ndhal_pelect_data.lr_mask = 0x1;
					break;
				case 0x3: // election tried on both links
					// retry a lame duck election with right link only
					pr_info("nd%02d: retrying election as 2 node UltraServer", ndhal_pelect_data.pnd[0]->device_index);
					ndhal_pelect_data.lr_mask = 0x2;
					break;
				default:
					pr_err("unexpected ultraServer link mask: %x", ndhal_pelect_data.lr_mask);
					ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
					break;
			}
			// if we are still running the election and we have less than NPE_ETIMEOUT_EXTENSION_MS
			// left to run the lame duck election, bump the deadline
			//
			if ((ndhal_pelect_data.pod_state_internal != NEURON_NPE_POD_ST_ELECTION_FAILURE) &&
				(ktime_to_ms(ktime_sub(ndhal_pelect_data.nbr_data_read_deadline, ktime_get())) < NPE_ETIMEOUT_EXTENSION_MS)) {
					ndhal_pelect_data.nbr_data_read_deadline = ktime_add(ktime_get(), ms_to_ktime(NPE_ETIMEOUT_EXTENSION_MS));
			}
		} else {
			ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_SUCCESS;
			pr_info("nd%02d: ultraserver election complete.  node id: %d node cnt: %d ultraserver Unique id: %016llx", ndhal_pelect_data.pnd[0]->device_index, 
					ndhal_pelect_data.node_id, ndhal_pelect_data.node_cnt, ndhal_pelect_data.pod_serial_num);
		}
		mutex_unlock(&ndhal_pelect_data.lock);
	}

	ndhal_pelect_data.pod_state_internal = NEURON_NPE_POD_ST_ELECTION_FAILURE;
	return 0;
}

static int npe_create_thread(void)
{
	ndhal_pelect_data.thread = kthread_run(npe_election_thread_fn, NULL, "neuron election");
	if (IS_ERR_OR_NULL(ndhal_pelect_data.thread)) {
		pr_err("election thread creation failed\n");
		return -1;
	}
	return 0;
}

static void npe_stop_thread(void)
{
	if (ndhal_pelect_data.thread == NULL)
		return;

	ndhal_pelect_data.stop = true;
	wake_up(&ndhal_pelect_data.wait_queue);
	kthread_stop(ndhal_pelect_data.thread); //blocks till the thread exits
	ndhal_pelect_data.thread = NULL;
}

ssize_t npe_class_node_id_show_data(char *buf, u32 sz)
{
	int node_id;
	enum neuron_ultraserver_mode mode = NEURON_ULTRASERVER_MODE_X1;

	if (npe_pod_state_busy()) {
		return dhal_sysfs_emit(buf, "busy\n");
	}

	if (sz == 4) {
		mode = NEURON_ULTRASERVER_MODE_X4;
	} else if (sz == 2) {
		if (npe_mode_is_supported(NEURON_ULTRASERVER_MODE_X2H)) {
			mode = NEURON_ULTRASERVER_MODE_X2H;
		} else if (npe_mode_is_supported(NEURON_ULTRASERVER_MODE_X2V)) {
			mode = NEURON_ULTRASERVER_MODE_X2V;
		}
	} else {
		pr_err("Unexpected class entry: node_id_%d", sz);
		return dhal_sysfs_emit(buf, "invalid\n");
	}

	node_id = npe_get_modal_node_id(mode);
	return dhal_sysfs_emit(buf, "%d\n", node_id);
}

ssize_t npe_class_server_id_show_data(char *buf, u32 sz)
{
	u64 pod_serial_number;
	enum neuron_ultraserver_mode mode = NEURON_ULTRASERVER_MODE_X1;

	if (npe_pod_state_busy()) {
		return dhal_sysfs_emit(buf, "0000000000000000\n");
	}

	if (sz == 4) {
		mode = NEURON_ULTRASERVER_MODE_X4;
	} else if (sz == 2) {
		if (npe_mode_is_supported(NEURON_ULTRASERVER_MODE_X2H)) {
			mode = NEURON_ULTRASERVER_MODE_X2H;
		} else if (npe_mode_is_supported(NEURON_ULTRASERVER_MODE_X2V)) {
			mode = NEURON_ULTRASERVER_MODE_X2V;
		}
	} else {
		pr_err("Unexpected class entry: server_id_%d", sz);
		return dhal_sysfs_emit(buf, "invalid\n");
	}
	pod_serial_number = npe_get_modal_serial_number(mode);

	return dhal_sysfs_emit(buf, "%016llx\n", pod_serial_number);
}

ssize_t npe_class_ultraserver_mode_show_data(char *buf)
{
	enum neuron_ultraserver_mode mode;
	char output[32];
	int len = 0;

	if (npe_pod_state_busy()) {
		return dhal_sysfs_emit(buf, "busy\n");
	}

	for (mode = NEURON_ULTRASERVER_MODE_X4; mode <= NEURON_ULTRASERVER_MODE_X1; mode++) {
		if (npe_mode_is_supported(mode)) {
			len += snprintf(output + len, sizeof(output) - len,  "%d,", mode);
		}
	}

	if (len == 0) {
		return dhal_sysfs_emit(buf, "\n");
	}

	// zap the last "," in the output
	output[len-1] = 0;
	return dhal_sysfs_emit(buf, "%s\n", output);
}

int npe_init(void)
{
	// force election to use right or left link only
	//
	if (npe_pod_ctl_is_set(NPE_POD_CTL_USE_R_LNK_ONLY)) {
		ndhal_pelect_data.lr_mask_default = 0x2;
	} else if (npe_pod_ctl_is_set(NPE_POD_CTL_USE_L_LNK_ONLY)) {
		ndhal_pelect_data.lr_mask_default = 0x1;
	}

	ndhal_pelect_data.nbr_data_read_timeout = (u64)pod_etimeout * 1000;
	return npe_create_thread();
}

void npe_cleanup(void)
{
	npe_stop_thread();
	if (ndhal_pelect_data.pnd[0] != NULL) {
		ndhal_pelect_data.pnd[0] = NULL;
	}
}
