// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

/**
 * Power utilization
 *
 * This module contains code designed to periodically sample power utilization, measured as a
 * percentage of max power with background power backed out, from firmware and to report
 * per-minute aggregate power stats to applications via a sysfs interface.
 */

#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>

#include "neuron_power.h"
#include "neuron_device.h"
#include "neuron_dhal.h"
#include "neuron_fw_io.h"

#define NEURON_FW_POWER_MIN_API_VERSION 3 // The minimum API version number in firmware that publishes power utilization
#define NEURON_FW_POWER_FIRMWARE_WARN_PERIOD 30 // How often to log about firmware that doesn't support power reads, in minutes.

static bool power_enabled_in_fw = false;

/**
 * npower_init_samples() - initializes the current set of power utilization samples for a device
 *
 * @nd - the neuron device whose stats we are initializing
 * @last_count - the counter value associated with the last power sample read from firmware for each die
 */
static void npower_init_samples(struct neuron_power_samples *samples, u16 last_count[])
{
    unsigned die;

	samples->num_data_points = 0;
	samples->total_power_util_bips = 0;
	samples->min_power_bips = NEURON_MAX_POWER_UTIL_BIPS;
	samples->max_power_bips = NEURON_MIN_POWER_UTIL_BIPS;
	for (die = 0; die < ndhal->ndhal_address_map.dice_per_device; die++) {
		samples->last_counter[die] = last_count[die];
	}
}

bool npower_enabled_in_fw(struct neuron_device *nd)
{
	int ret = 0;
	u32 api_version_num = 0;

	extern unsigned int nmetric_log_posts;

	// If metric log posting is disabled, don't do power reporting - we're likely on
	// bringup hardware or simulation
	if (!nmetric_log_posts) {
		return false;
	}

	// Just read the API version from firmware.  We could try to be smart here and cache
	// this, but we need to protect ourselves from rollbacks in the firmware version or
	// other changes.  Plus, this is just a simple MMIO read, so it's cheap.
	ret = fw_io_api_version_read(nd->npdev.bar0, &api_version_num);
	if (ret != 0) {
		pr_err("Failed to read firmware API version, err = %d\n", ret);
	}

	// Cache the enablement state so that we can grab it without needing to call into this
	// function for quick things like checking whether we need to log.
	power_enabled_in_fw = (ret == 0) && (api_version_num >= NEURON_FW_POWER_MIN_API_VERSION);

	return power_enabled_in_fw;
}

/**
 * npower_get_utilization() - gets the utilization portion of a power sample from firmware
 *
 * @power_sample: a sample gathered from firmware, which contains both power and a counter value
 *
 */
static inline u16 npower_get_utilization(u32 power_sample)
{
	return (u16)(power_sample & 0xFFFF);
}

/**
 * npower_get_sample_num() - gets the counter portion of a power sample from firmware
 *
 * @power_sample: a sample gathered from firmware, which contains both power and a counter value
 *
 */
static inline u16 npower_get_sample_num(u32 power_sample)
{
	return (u16)((power_sample >> 16) & 0xFFFF);
}

/**
 * npower_classify_die_observation() - figure out what the per-die cache should say after
 *                                     a single power read attempt
 *
 * @read_ret: the return value from the firmware read (0 on success, an error code
 *            otherwise)
 * @raw_sample: the raw power sample word from firmware.  Only meaningful when @read_ret
 *              is 0.
 * @entry: location to fill in with the cache entry we should store after this
 *         observation
 *
 * This function is pure - it doesn't touch any locks or any device state - so it's easy
 * to reason about and easy to unit test.
 *
 * The rules:
 *   - If the read failed, record a READ_ERROR.
 *   - If the read succeeded but firmware handed us a util value that's outside the legal
 *     range, record a BOGUS entry holding the offending value (we keep it around so the
 *     sysfs row can show it for debugging).
 *   - Otherwise, record a GOOD entry with the new util/counter pair.
 */
static void npower_classify_die_observation(int read_ret, u32 raw_sample,
                                            struct neuron_power_die_cache_entry *entry)
{
	u16 util;
	u16 cnt;

	if (read_ret != 0) {
		entry->populated = true;
		entry->state = NEURON_POWER_DIE_STATE_READ_ERROR;
		entry->util_bips = 0;
		entry->counter = 0;
		return;
	}

	util = npower_get_utilization(raw_sample);
	cnt = npower_get_sample_num(raw_sample);

	entry->populated = true;
	entry->state = (util > NEURON_MAX_POWER_UTIL_BIPS) ? NEURON_POWER_DIE_STATE_BOGUS
	                                                  : NEURON_POWER_DIE_STATE_GOOD;
	entry->util_bips = util;
	entry->counter = cnt;
}

/**
 * npower_store_utilization() - Store a sample of neuron power utilization data in the set of
 *                              samples for a device
 *
 * @nd: pointer to the neuron device whose power utilization is to be saved
 * @utilization: the new power utilization value to save
 * @current_counter: the sample counter from firmware associated with the current utilization
 *
 * @return: 0 on success, nonzero on failure
 */
static int npower_store_utilization(struct neuron_device *nd, u16 utilization, u16 current_counter[NEURON_POWER_MAX_DIE])
{
	static u64 last_logging_jiffies = 0;
	static unsigned missed_logging_messages = 0;
	unsigned die;

	if (utilization > NEURON_MAX_POWER_UTIL_BIPS) {
		// Bogus power values should never happen.  But if they do, don't spam the logs
		if (jiffies > last_logging_jiffies + 10 * HZ) {
			last_logging_jiffies = jiffies;
			pr_err("Invalid power utilization value: %u, skipped %u logging messages\n",
			       utilization, missed_logging_messages);
			missed_logging_messages = 0;
		} else {
			missed_logging_messages++;
		}
		return -1;
	}

	nd->power.current_samples.total_power_util_bips += utilization;
	nd->power.current_samples.num_data_points++;
	if (utilization < nd->power.current_samples.min_power_bips) {
		nd->power.current_samples.min_power_bips = utilization;
	}
	if (utilization > nd->power.current_samples.max_power_bips) {
		nd->power.current_samples.max_power_bips = utilization;
	}

    // Save the set of counters associated with
	for (die = 0; die < ndhal->ndhal_address_map.dice_per_device; die++) {
        nd->power.current_samples.last_counter[die] = current_counter[die];
	}


	return 0;
}

/**
 * npower_passed_minute_boundary() - determine if we've passed the start of a new minute since epoch since last calculating stats
 *
 * @nd: pointer to a neuron device structure
 *
 * returns true if the number of seconds since epoch indicates that a new minute has started in wall clock time since we last rolled up samples into stats
 *         false if not
 *
 * @note: this is checking for minute boudnaries in time since epoch so that we can produce a number at the top of each minute.  This was a
 *.       customer request.  It's a little odd and renders us slightly vulnerable to clock adjustments, but we sample often enough that it's unlikely to be
 *.       an issue in practical terms.  Worst case, we have a period with fewer or more data points in the sample, which is not a massive problem.
*/
static bool npower_passed_minute_boundary(struct neuron_device *nd, struct timespec64 *curr_time)
{
	long int curr_minute;
	long int last_minute;

	curr_minute = curr_time->tv_sec / 60;
	last_minute = nd->power.current_stats.time_of_sample.tv_sec / 60;
	if (curr_minute != last_minute) {
		return true;
	}
	return false;
}

static void npower_acquire_lock(struct neuron_device *nd)
{
	mutex_lock(&nd->power.stats_lock);
}

static void npower_release_lock(struct neuron_device *nd)
{
	mutex_unlock(&nd->power.stats_lock);
}

/**
 * npower_calculate_stats() - Given data about a set of power utilization samples, generate a new set of per-period stats
 *
 * @current_samples - data about the power values seen in the last period
 * @new_stats - A location to write the stats into
 * @curr_time - the current time since epoch
 */
static void npower_calculate_stats(struct neuron_power_samples *current_samples,
                                   struct neuron_power_stats *new_stats,
                                   struct timespec64 *curr_time)
{
	// Make sure we have at least a minimum number of data points present so we know that
	// we have a valid set of stats to produce.
	if (current_samples->num_data_points >= NEURON_MIN_SAMPLES_PER_PERIOD) {
		new_stats->status = POWER_STATUS_VALID;
		new_stats->min_power_bips = current_samples->min_power_bips;
		new_stats->max_power_bips = current_samples->max_power_bips;
		new_stats->avg_power_bips =
			current_samples->total_power_util_bips / current_samples->num_data_points;
	} else {
		// If we never got a measurement, min power will have been initialized to 100%.
		// Sanitize that so that we don't print a misleading value in the logs.
		u16 min_power_to_log = current_samples->min_power_bips;
		if (min_power_to_log > current_samples->max_power_bips) {
			min_power_to_log = current_samples->max_power_bips;
		}
		if (power_enabled_in_fw) {
			pr_debug("Not enough data to aggregate stats.  Have %u data points, min of %u max of %u, total of %llu.",
				current_samples->num_data_points, min_power_to_log,
				current_samples->max_power_bips,
				current_samples->total_power_util_bips);
		}

		new_stats->status = POWER_STATUS_NO_DATA;
		new_stats->min_power_bips = NEURON_MIN_POWER_UTIL_BIPS;
		new_stats->max_power_bips = NEURON_MIN_POWER_UTIL_BIPS;
		new_stats->avg_power_bips = NEURON_MIN_POWER_UTIL_BIPS;
	}
	new_stats->time_of_sample.tv_sec = curr_time->tv_sec;
	new_stats->time_of_sample.tv_nsec = curr_time->tv_nsec;
}

/**
 * npower_copy_stats() - Copy the stats from one location to another.
 *
 * @src - the source stats
 * @dst - the destination stats
 */
static void npower_copy_stats(struct neuron_power_stats *src, struct neuron_power_stats *dst)
{
    *dst = *src;
}
/**
 * npower_aggregate_stats() - take the current set of samples for a period and use them to generate per-period stats
 *l
 * @nd: pointer to the neuron device whose power utilization is to be aggregated into stats
 * @current_fw_counter: the counter value associated with the last power sample read from firmware for each die
 *
 */
static void npower_aggregate_stats(struct neuron_device *nd, struct timespec64 *curr_time,
                                   u16 current_fw_counter[])
{
        // Calculate new stats based on the current set of samples.  Note that we do all calculation
        // before we take the lock around the stats in order to minimize contention.
        struct neuron_power_stats new_stats;
        npower_calculate_stats(&nd->power.current_samples, &new_stats, curr_time);

        // Clear the current set of samples so we're ready to start taking new ones
        npower_init_samples(&nd->power.current_samples, current_fw_counter);

        // Get the lock to protect against somebody trying to read the stats while we're writing them.
        npower_acquire_lock(nd);

        // Write the new stats into the set that's stored in the neuron_device, then free the lock
        // and be done.
        npower_copy_stats(&new_stats, &nd->power.current_stats);
        npower_release_lock(nd);
}

/*
 * npower_in_simulated_env() - tells if we are running on silicon or various flavors of
 *                             simulation or emulation
 *
 * @return: true if we are running on simulation or emulation, false if on real silicon
 */
static bool npower_in_simulated_env(void)
{
        return narch_is_qemu() || narch_is_emu();
}

/**
 * npower_update_die_cache() - update the per-die cache entry for a single die based on a
 *                             freshly-read power sample from firmware
 *
 * @nd: pointer to the neuron device whose cache we are updating
 * @die: which die's cache entry to update
 * @read_ret: the return value from the firmware read for this die
 * @raw_sample: the raw power sample word read from firmware.  Only meaningful when
 *              @read_ret is 0.  Low 16 bits are util_bips, high 16 bits are the sample
 *              counter.
 *
 * The caller is responsible for performing the firmware read before calling this - we
 * never issue MMIO ourselves.  We classify the new observation outside the lock and then
 * take stats_lock just long enough to store the result, so we never hold stats_lock
 * across firmware I/O and a concurrent sysfs reader can't observe a torn cache entry.
 *
 * Note that this overwrites the cache entry on every call.  In particular, we don't
 * suppress duplicate counters here the way npower_select_power() does for the aggregate
 * stats - the aggregator gate exists to keep the per-minute stats from being skewed by
 * stale firmware samples, but this cache just needs to mirror what the previous live
 * sysfs read path used to return, including transient READ_ERROR and BOGUS states.
 * Don't try to unify the two.
 */
static void __maybe_unused npower_update_die_cache(struct neuron_device *nd, unsigned die,
                                                   int read_ret, u32 raw_sample)
{
        struct neuron_power_die_cache_entry next;

        npower_classify_die_observation(read_ret, raw_sample, &next);

        npower_acquire_lock(nd);
        nd->power.per_die_cache[die] = next;
        npower_release_lock(nd);
}

static void npower_select_power(struct neuron_device *nd, u32 power_samples[NEURON_POWER_MAX_DIE])
{
        bool duplicate_read = false;
        u16 largest_power = 0;
        u16 current_counters[NEURON_POWER_MAX_DIE] = { 0 };
        u16 current_power = 0;
        unsigned die;

	    for (die = 0; die < ndhal->ndhal_address_map.dice_per_device; die++) {
		        current_power = npower_get_utilization(power_samples[die]);
		        current_counters[die] = npower_get_sample_num(power_samples[die]);

		        if (current_counters[die] == nd->power.current_samples.last_counter[die]) {
			             duplicate_read = true;
		        } else if (current_power > largest_power) {
			            largest_power = current_power;
		        }
	    }

	     // If we've seen a new reading for all dice, store the largest power value we've seen.
	    if (!duplicate_read) {
		    npower_store_utilization(nd, largest_power, current_counters);
	    }
}

/**
 * npower_sample_utilization() - read power utilization from firmware and, if there is new
 *                                      data found, store it.
 *
 * @dev: pointer to the neuron device whose power utilization is to be sampled
 *
  * @return: 0 on success, nonzero on failure
 */
int npower_sample_utilization(void *dev)
{
        uint32_t power_samples[NEURON_POWER_MAX_DIE] = { 0 };

        static unsigned long long last_log_time = 0ULL;
        static unsigned skipped_log_msgs = 0;
        struct timespec64 curr_time = { 0 };
        int ret[NEURON_POWER_MAX_DIE] = { -1 };
        unsigned die;
        bool failed_read = false;

        struct neuron_device *nd = (struct neuron_device *)dev;

        // If we're in the middle of reset and the device isn't ready, just quit.  We don't know if we can talk
        // to the firmware at this point, even just to read the firmware API version.  So there's not much that we
        // can do.  Similarly, just quit if we're running in emulation/simulation rather than on real silicon.
        if ((nd->device_state != NEURON_DEVICE_STATE_READY) || npower_in_simulated_env()) {
            return -1;
        }

        // Only attempt to read power utilization if we have a firmware version that supports it.
        // We don't just exit in this case because we want to still handle the per-minute processing
        // that comes later in this function.
        if (npower_enabled_in_fw(nd)) {
                // Read power utilization from MISC RAM.  If we succeed, break it up into its
                // component parts so we can do calculations on the power.
                for (die = 0; die <  ndhal->ndhal_address_map.dice_per_device; die++) {
                    ret[die] = fw_io_device_power_read(nd->npdev.bar0, &power_samples[die], die);

                    // Mirror this observation into the per-die cache that backs the raw
                    // sysfs read path.  We do this whether or not the read succeeded,
                    // independently of npower_select_power's duplicate-tick gate below.
                    // See npower_update_die_cache for why these two paths intentionally
                    // don't share the same gating rule.
                    npower_update_die_cache(nd, die, ret[die], power_samples[die]);

                    if (ret[die]) {
                            failed_read = true; // Note for later when we determine whether we have data to store

                            // Log an error here, but don't spam if it's a continuous condition
                            unsigned long long deltat = jiffies - last_log_time;
                            if (deltat >= 60 * HZ) {
                                    pr_err("sysfs failed to read power utilization data from FWIO, skipped %u log messages",
                                           skipped_log_msgs);
                                    last_log_time = jiffies;
                                    skipped_log_msgs = 0;
                            } else {
                                    skipped_log_msgs++;
                            }
                    }
                }

        } else {
                static u64 last_log_jiffies = 0;
                // Log at most once per <n> minutes if we have firmware that doesn't support power measurement
                if (jiffies > last_log_jiffies + NEURON_FW_POWER_FIRMWARE_WARN_PERIOD * 60 * HZ) {
                        last_log_jiffies = jiffies;
                        pr_info("Power utilization not supported in firmware, cannot read power at this time.\n");
                }
        }

        // If we were able to read the power data from all dice on the neuron device, select the current power
        // utilization value from the values we read and store it
        if (!failed_read) {
                npower_select_power(nd, power_samples);
        }

        // If we've passed over a minute boundary since we last aggregated stats, do so now.
        // Note that we do this whether or not there was an error reading stats - we need to update
        // the stats to indicate that there was an error.
        ktime_get_real_ts64(&curr_time);
        if (npower_passed_minute_boundary(nd, &curr_time)) {
                npower_aggregate_stats(nd, &curr_time, nd->power.current_samples.last_counter);
        }

        return (failed_read == true);
}

/**
 * npower_get_stats()
 *
 * @nd - expected to point to a struct neuron_device
 * @stats - location to write stats into
 *
 * Doesn't have a return value.  Failures will be indicated by the neuron_power_stats_status enum in
 * the returned stats
 */
static void npower_get_stats(void *dev, struct neuron_power_stats *stats)
{
        struct neuron_device *nd = (struct neuron_device *)dev;

        npower_acquire_lock(nd);
        npower_copy_stats(&nd->power.current_stats, stats);
        npower_release_lock(nd);
}

/*
 ** Stringify a power status.
 */
static const char *neuron_power_status_string[] = { FOREACH_POWER_STATUS(GENERATE_STRING) };

int npower_format_stats(void *dev, char buffer[], unsigned bufflen)
{
        unsigned bytes_formatted;
        struct neuron_power_stats stats;
        enum neuron_power_stats_status status;

        if (!dev) {
                struct timespec64 currtime;
                jiffies_to_timespec64(jiffies, &currtime);
                snprintf(buffer, bufflen, "%.32s,%lld,0.00,0.00,0.00",
                         neuron_power_status_string[POWER_STATUS_NO_DATA], currtime.tv_sec);
                return -1;
        }

        npower_get_stats(dev, &stats);

        // Safety check the status before passing it to be stringified.
        status = stats.status;
        if (status > POWER_STATUS_MAX) {
                status = POWER_STATUS_MAX;
        }

        bytes_formatted = snprintf(buffer, bufflen, "%.32s,%lld,%u.%02u,%u.%02u,%u.%02u",
                                   neuron_power_status_string[status], stats.time_of_sample.tv_sec,
                                   stats.min_power_bips / 100, stats.min_power_bips % 100,
                                   stats.max_power_bips / 100, stats.max_power_bips % 100,
                                   stats.avg_power_bips / 100, stats.avg_power_bips % 100);

        return ((bytes_formatted > bufflen) ||
                ((bytes_formatted == bufflen) && buffer[bufflen - 1] != '\0'));
}

/**
 * npower_emit_die_row() - format a single per-die row of the raw sysfs output from a
 *                         cache snapshot
 *
 * @die: the die index to print at the start of the row
 * @snapshot: pointer to a cache entry that the caller has already copied out of
 *            per_die_cache[] under stats_lock.  We operate on the caller's snapshot, so
 *            we don't need to take any locks ourselves.
 * @buf: where to write the row
 * @bufflen: how much room is left in @buf
 *
 * Returns the number of bytes written, with the same scnprintf truncation semantics that
 * the rest of this file uses, so the caller can just add this to its running offset.
 *
 * The output is one of these four forms, picked based on what the snapshot says:
 *   <die>,unavailable          - we haven't gotten a sample for this die yet, or the
 *                                snapshot is otherwise unpopulated
 *   <die>,<util_bips>,<counter> - normal good reading
 *   <die>,read_error           - the most recent firmware read for this die failed
 *   <die>,bogus(<util>),<counter> - firmware returned an out-of-range value; we keep the
 *                                   offending value visible to make debugging easier
 */
static unsigned __maybe_unused
npower_emit_die_row(unsigned die,
                    const struct neuron_power_die_cache_entry *snapshot,
                    char *buf, unsigned bufflen)
{
        if (!snapshot->populated) {
                return scnprintf(buf, bufflen, "%u,unavailable\n", die);
        }

        switch (snapshot->state) {
        case NEURON_POWER_DIE_STATE_READ_ERROR:
                return scnprintf(buf, bufflen, "%u,read_error\n", die);
        case NEURON_POWER_DIE_STATE_BOGUS:
                return scnprintf(buf, bufflen, "%u,bogus(%u),%u\n",
                                 die, snapshot->util_bips, snapshot->counter);
        case NEURON_POWER_DIE_STATE_GOOD:
        default:
                return scnprintf(buf, bufflen, "%u,%u,%u\n",
                                 die, snapshot->util_bips, snapshot->counter);
        }
}

int npower_format_raw(void *dev, char buffer[], unsigned bufflen)
{
        struct neuron_device *nd = (struct neuron_device *)dev;
        struct neuron_power_die_cache_entry snapshot[NEURON_POWER_MAX_DIE];
        unsigned offset = 0;
        unsigned die;
        unsigned dice;

        // Always emit the header first.
        offset += scnprintf(buffer + offset, bufflen - offset, "die,util_bips,counter\n");

        // Top-level unavailability: if the device isn't ready, we're in simulation, or
        // firmware doesn't support power readings, just emit "unavailable" for every die.
        // We don't take stats_lock or look at per_die_cache[] in that case - there's
        // nothing useful in there anyway.
        if (!nd || nd->device_state != NEURON_DEVICE_STATE_READY ||
            npower_in_simulated_env() || !npower_enabled_in_fw(nd)) {
                for (die = 0; die < ndhal->ndhal_address_map.dice_per_device; die++) {
                        offset += scnprintf(buffer + offset, bufflen - offset,
                                            "%u,unavailable\n", die);
                }
                return 0;
        }

        // Snapshot the cache under stats_lock and do all the formatting outside the lock.
        // Holding the lock just across a memcpy keeps it cheap, ensures no scnprintf runs
        // under it, and guarantees that each formatted row reflects a single, complete
        // cache write rather than a torn update.  We don't issue MMIO from here at all -
        // the periodic sampler is the sole producer of per-die state.
        npower_acquire_lock(nd);
        memcpy(snapshot, nd->power.per_die_cache, sizeof(snapshot));
        npower_release_lock(nd);

        dice = ndhal->ndhal_address_map.dice_per_device;
        for (die = 0; die < dice; die++) {
                offset += npower_emit_die_row(die, &snapshot[die],
                                              buffer + offset, bufflen - offset);
        }

        return 0;
}

int npower_init_stats(struct neuron_device *nd)
{
        u16 zero_counter_array[NEURON_POWER_MAX_DIE] = { 0 };

        if (!nd) {
                pr_err("npower_init_power_stats: nd is NULL\n");
                return -1;
        }

        // Initialize the samples to base value, then set the stats' status to "no data".  This will prevent
        // us from serving up any stats if an application reads from sysfs before we have gotten
        // enough data points to aggregate some.
        npower_init_samples(&nd->power.current_samples, zero_counter_array);

        npower_acquire_lock(nd);
        memset(&nd->power.current_stats, 0, sizeof(struct neuron_power_stats));
        nd->power.current_stats.status = POWER_STATUS_NO_DATA;

        // Reset the per-die cache.  Zeroing it leaves every entry with populated=false,
        // which is the only thing the sysfs reader cares about until the sampler fills
        // entries in - the other fields are just along for the ride at this point.
        memset(&nd->power.per_die_cache, 0, sizeof(nd->power.per_die_cache));
        npower_release_lock(nd);

        return 0;
}

