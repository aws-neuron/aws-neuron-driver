#ifndef NEURON_DHAL_H
#define NEURON_DHAL_H

#include "neuron_device.h"
#include "neuron_fw_io.h"
#include "neuron_mmap.h"
#include "neuron_sysfs_metrics.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0))
#define dhal_sysfs_emit(buf, ...) scnprintf((buf), PAGE_SIZE, __VA_ARGS__)
#else
#define dhal_sysfs_emit(buf, ...) sysfs_emit((buf), __VA_ARGS__)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
#define dhal_atomic_fetch_add(v,a)  atomic_fetch_add(v, (a))
#else
#define dhal_atomic_fetch_add(v,a)  (atomic_add_return(v, (a)) -1)
#endif



extern int force_die_flip;

struct ndhal_arch {
    int arch;
    enum neuron_platform_type platform_type;
    u32 server_id;
    int (*narch_platform_ready) (struct neuron_device *nd, enum neuron_platform_operation_type platform_operation);
};
struct ndhal_address_map {
	// addresses
	uint64_t pci_host_base;
	uint64_t mmap_nc_event_offset;
	uint64_t mmap_nc_sema_read_offset;
	uint64_t mmap_nc_sema_set_offset;
	uint64_t mmap_nc_sema_incr_offset;
	uint64_t mmap_nc_sema_decr_offset;
	uint64_t bar0_misc_ram_offset;

	// counts
	int nc_per_device;
	unsigned dice_per_device;
	uint32_t dev_nc_map;
	uint64_t semaphore_count;
	uint64_t event_count;
	uint32_t ts_per_device;
	int dma_eng_per_nc;
	int seng_dma_eng_per_nd;
    int h2d_dma_eng_per_nd;
	int num_hbms;
	u16 nq_per_nc;
};

struct ndhal_reset {
    uint64_t reset_tpb_initial_poll_delay;
	uint64_t initiate_max_wait_time;
    uint32_t retry_count;
    int (*nr_initiate_reset) (struct neuron_device *nd, uint32_t nc_map);
    int (*nr_wait_for_reset_completion) (struct neuron_device *nd);
	int (*nr_post_reset_config) (struct neuron_device *nd, bool reset_successful, bool is_no_reset);
};

struct ndhal_topsp {
    int (*ts_nq_init) (struct neuron_device *nd, u8 ts_id, u8 eng_index, u32 nq_type, u32 size,
                       u32 on_host_memory, u32 hbm_index,
                       bool force_alloc_mem, struct mem_chunk **nq_mc, u64 *mmap_offset);
    void (*ts_nq_destroy_one) (struct neuron_device *nd, u8 ts_id);
	u8 (*ts_nq_get_nqid)(struct neuron_device *nd, u8 index, u32 nq_type);
    void (*ts_nq_set_hwaddr) (struct neuron_device *nd, u8 ts_id, u8 index, u32 nq_type, u32 size, u64 queue_pa);
};

struct ndhal_nc {
    int (*nc_get_semaphore_base) (struct neuron_device *nd, u8 nc_id, void **sem_base);
    int (*nc_get_event_addr) (struct neuron_device *nd, u8 nc_id, u16 event_index, void **ev_addr);
};

struct ndhal_nq {
   u8 (*nnq_get_nqid) (struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type);
   void (*nnq_set_hwaddr) (struct neuron_device *nd, u8 nc_id, u8 index, u32 nq_type, u32 size, u64 queue_pa);
   u8 (*nnq_get_nq_queue_count) (void);
};

struct ndhal_mpset {
    int mp_min_alloc_size;
    u64 device_dram_effective_base_addr[MAX_NUM_HBMS];
    u64 device_dram_end_addr[MAX_NUM_HBMS];
    bool small_pool_supported;
    void (*mpset_set_dram_and_mpset_info) (struct neuron_mempool_set *mpset, u64 *device_dram_addr, u64 *device_dram_size);
};

struct ndhal_ndmar {
    uint32_t (*ndmar_get_h2t_eng_id) (struct neuron_device *nd, uint32_t nc_id);
    int (*ndmar_get_h2t_def_qid) (uint32_t nc_id);
    int (*ndmar_ctx_queue_bit) (uint32_t h2d_eng_id, uint32_t qid);
    void (*ndmar_ctx_queue_from_bit) (int bit, uint32_t *h2d_eng_id, uint32_t *qid);
    bool (*ndmar_is_h2t_def_q) (struct neuron_device *nd, uint32_t eng_id, uint32_t q_id);
    bool (*nr_init_h2t_eng) ( int nc_idx, uint32_t nc_map); 
    bool (*ndmar_is_nx_ring) (uint32_t eng_id, uint32_t q_id);
    int (*ndmar_quiesce_queues) (struct neuron_device *nd, u32 nc_id, u32 engine_count, u32 *queue_mask);
};

struct ndhal_fw_io {
    u32 new_readless_read_min_api_version;
    int (*fw_io_topology) (struct fw_io_ctx *ctx, int pdev_index, int device_id, u32 *connected_device_ids, int *count);
    int (*fw_io_register_readless_read_region) (struct fw_io_ctx *ctx, void __iomem *bar0, u64 bar0_size, void __iomem *bar2, u64 bar2_size);
    int (*fw_io_read_csr_array) (void **addrs, u32 *values, u32 num_csrs, bool operational);
    int (*fw_io_execute_request) (struct fw_io_ctx *ctx, u8 command_id, const u8 *req, u32 req_size, u8 *resp, u32 resp_size);
    int (*fw_io_post_metric) (struct fw_io_ctx *ctx, u8 *data, u32 size);
    void (*fw_io_cache_bar_info) (struct neuron_device *nd);
};

struct ndhal_mmap {
    struct neuron_dm_special_mmap_ent *dm_mmap_special;
    int (*mmap_get_bar4_offset) (u64 start_addr, u64 size, u64 *offset);
};

struct ndhal_sysfs_metrics {
    char *arch_nd_type_suffix;
    char *arch_nc_type_suffix;
    char *arch_instance_suffix;
    char *arch_device_name_suffix;
    int root_info_node_attrs_info_tbl_cnt;
    nsysfsmetric_attr_info_t *root_info_node_attrs_info_tbl;

    int (*nsysfsmetric_add_ecc_nodes) (struct nsysfsmetric_metrics *metrics, 
                                       struct nsysfsmetric_node *stats_node,
                                       int ecc_attrs_info_tbl_cnt,
                                       const nsysfsmetric_attr_info_t *attr_info_tbl);

    void (*nsysfsmetric_get_hbm_error_count) (struct neuron_device *nd,
                                                 bool repairable,
                                                 uint32_t *err_count);

    int (*nsysfsmetric_add_tensor_engine_node) (struct nsysfsmetric_metrics *metrics,
                                                struct nsysfsmetric_node *stats_node,
                                                int nc_id,
                                                int tensor_engine_attrs_info_tbl_cnt,
                                                const nsysfsmetric_attr_info_t *tensor_engine_attr_info_tbl);

    bool health_status_enabled;
};

struct ndhal_pci {
    int apb_bar;
    int axi_bar;
    int dram_bar;
    u64 dram_bar_size;

    int (*neuron_pci_get_device_id) (struct neuron_device *nd, struct pci_dev *dev);
    int (*neuron_pci_device_id_to_rid_map) (uint32_t * count, uint32_t * did_to_rid_map);
};

struct ndhal_cdev {
    struct ncdev_mem_region *ncdev_mem_regions;
    u64 *ncdev_bar0_write_blocked_addrs;

    void (*ncdev_compatible_version) (struct neuron_ioctl_compatible_version *arg);
    int (*ncdev_logical_to_physical_nc_map)(struct neuron_ioctl_nc_map *map, uint32_t max_num_entries, enum neuron_ioctl_nc_mapping_type mapping_type);
    void (*ncdev_get_default_tpbs_for_hbm) (u32 hbm_index, u32 tpbs[MAX_NC_PER_DEVICE], u32 *tpb_count);
};

struct ndhal_udma {
	unsigned int num_queues;
	unsigned int num_beats;
};

struct ndhal_ndma {
    bool ndma_retry_memcpy;

    void (*ndma_get_wait_for_completion_time) (u32 count, bool async, u64 *first_wait_time, u64 *following_wait_time);
    int (*ndma_validate_pa) (struct neuron_device *nd, phys_addr_t pa, struct mem_chunk *dst_mc, u32 desc_type);
    int (*ndma_init) (void __iomem *bar0, struct udma *udma, int eng_id);
    int (*ndma_is_bar0_write_blocked) (u64 off);
    int (*ndma_get_m2m_barrier_type) (bool set_dmb);
    void (*ndma_get_engines_with_host_connectivity) (u32 hbm_index, u32 engines[NUM_DMA_ENG_PER_DEVICE], u32 *num_engines);
};

struct ndhal_npe {
	void (*npe_notify_mark)(int mark_cnt, bool mark);
	int (*npe_pod_info)( u8 *pod_type, u8 *pod_id, u8 *pod_sz, enum neuron_ultraserver_mode *mode, u32 *modes_supported);
	int (*npe_pod_status)( u32 *pod_state, s8 *node_id);
	int (*npe_pod_ctrl)( struct neuron_device *nd, u32 pod_ctrl, enum neuron_ultraserver_mode mode, u32 timeout, u32 *pod_state);
	ssize_t (*npe_class_node_id_show_data)(char *buf, u32 sz);
	ssize_t (*npe_class_node_cnt_show_data)(char *buf);
	ssize_t (*npe_class_server_id_show_data)(char *buf, u32 sz);
	ssize_t (*npe_class_ultraserver_mode_show_data)(char *buf);
	u32 (*npe_neighbor_eng_ids)[2];
};

struct ndhal_tpb {
    int pe_xbus_count;
    int pe_row_grp_count;
    int pe_col_grp_count;
    u64 pe_perf_reg_grp_size;
    u64 *pe_mm_cntr_offsets;
    u64 *pe_wl_cntr_offsets;
    u64 *pe_fast_wl_cntr_offsets;
    u64 *pe_idle_cntr_offsets;
    u64 (*pe_get_row_grp_activity_counter_offset)(u64 base, int row_grp_id);
    int (*pe_get_counter_val)(void __iomem *bar0, u64 lsb_offset, u64 *val);
    int (*pe_get_fast_wl_cycle_cnt)(struct neuron_device *nd, int nc_id, int row_grp_id, u64 *val);
    int (*pe_get_aggregated_wl_cycle_cnt)(struct neuron_device *nd, int nc_id, int row_grp_id, u64 *val);
    int (*pe_format_activity_stats)(struct neuron_device *nd, int nc_id, char buffer[], unsigned int bufflen);
};

struct ndhal_perf {
    int current_performance_profile;
    int (*perf_set_profile) (struct neuron_device *nd, uint32_t profile);
    int (*perf_get_profile) (struct neuron_device *nd, uint32_t *profile);
    int (*perf_get_supported_profiles) (struct neuron_device *nd, u16 feature, u8 *num_profiles, u8 out_bitmap[32]);
    void (*perf_update_hbm_7200_supported) (struct neuron_device *nd);
};

struct neuron_dhal {
    unsigned int pci_device_id;

    struct ndhal_arch ndhal_arch;
    struct ndhal_address_map ndhal_address_map;
    struct ndhal_reset ndhal_reset;
    struct ndhal_topsp ndhal_topsp;
    struct ndhal_nc ndhal_nc;
    struct ndhal_nq ndhal_nq;
    struct ndhal_mpset ndhal_mpset;
    struct ndhal_ndmar ndhal_ndmar;
    struct ndhal_fw_io ndhal_fw_io;
    struct ndhal_mmap ndhal_mmap;
    struct ndhal_sysfs_metrics ndhal_sysfs_metrics;
    struct ndhal_pci ndhal_pci;
    struct ndhal_cdev ndhal_cdev;
    struct ndhal_udma ndhal_udma;
    struct ndhal_ndma ndhal_ndma;
	struct ndhal_npe ndhal_npe;
    struct ndhal_tpb ndhal_tpb;
    struct ndhal_perf ndhal_perf;
    void (*ndhal_ext_cleanup) (void);
};

extern struct neuron_dhal *ndhal;       // ndhal is a global structure shared by all available neuron devices


/**
 * @brief Initialize the global ndhal for all available neuron devices
 *          - The initialization must be done only once
 *          - Mem allocation must be wrapped by the ndhal_init_lock
 * 
 * @param pci_device_id: the PCI DEVICE ID
 * 
 * @return int 0 for success, negative for failures
 */
int neuron_dhal_init(unsigned int pci_device_id);

/**
 * @brief Cleanup any ndhal related resources prior to 
 *
 */
void neuron_dhal_cleanup(void);

/**
 * @brief Clean up ndhal
 * The caller is to ensure that the ndhal is freed only once.
 * 
 */
void neuron_dhal_free(void);

/**
 * ndhal_register_funcs() - Register functions based on hardward arch to the ndhal
 * 
 * @return int 0 on success, negative for failures
 */
int ndhal_register_funcs_vc(void);
int ndhal_register_funcs_v2(void);
int ndhal_register_funcs_v3(void);
int ndhal_register_funcs_v4(void);

#endif // #ifndef NEURON_DHAL_H
