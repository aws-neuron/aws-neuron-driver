// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025, Amazon.com, Inc. or its affiliates. All Rights Reserved
 */

#ifndef _NEURON_POWER_H
#define _NEURON_POWER_H

struct neuron_device;

// The maximum number of dice that we will read power utilization from per neuron device
#define NEURON_POWER_MAX_DIE 2

/*
 * A struct that holds aggregated data about power utilization since the last read
 *
 * @num_data_points - the number of power utilization samples included in the data
 * @total_power_util_bips - the sum of all power utilization samples included
 * @min_power_bips - the minimum power utilization seen during the sampling period
 * @max_power_bips - the maximum power utilization seen during the sampling period
 * @last_counter - the counter value associated with the most recent power util sample read from
 *                 firmware.  Used to detect if we read samples faster than firmware provides them.
 *                 We keep an array here to support per-die power utilization data from firmware.
 *
 * @note The power data is expressed in basis points so that we can stick to integer math while
 * preserving resolution.
 */
struct neuron_power_samples {
	u32 num_data_points;
	u64 total_power_util_bips;
	u16 min_power_bips;
	u16 max_power_bips;
	u16 last_counter[NEURON_POWER_MAX_DIE];
};

/**
 * An enum that describes the status of our power utilization stats.  Defined in this way
 * so that we can easily iterate over the possible values, and make a stringifier that's
 * easy to use and validated at the preprocessor.
 */
#define FOREACH_POWER_STATUS(POWER_STATUS)                                                         \
	POWER_STATUS(POWER_STATUS_VALID)                                                               \
	POWER_STATUS(POWER_STATUS_NO_DATA)                                                             \
	POWER_STATUS(POWER_STATUS_INVALID)                                                             \
	POWER_STATUS(POWER_STATUS_MAX)

#define GENERATE_ENUM(ENUM) ENUM,
#define GENERATE_STRING(STRING) #STRING,

enum neuron_power_stats_status { FOREACH_POWER_STATUS(GENERATE_ENUM) };

/**
 * Min and max values for power utilization, expressed in basis points.
 */
#define NEURON_MAX_POWER_UTIL_BIPS (100 * 100)
#define NEURON_MIN_POWER_UTIL_BIPS 0

/**
 * The minimum number of power utilization samples needed in a minute for stats to be considered valid
 */
#define NEURON_MIN_SAMPLES_PER_PERIOD 60

/*
 * A struct that holds cached data about power utilization, sampled minutely.
 *
 * @status - an enum indicting whether we have valid data or not
 * @time_of_sample - the time of day at which the sample was taken.  Note that this is a real time
 *                   and can be affected by timezone changes and NTP updates
 * @avg_power_bips - The average power utilization during the period
 * @min_power_bips - The minimum power utilization during the period
 * @max_power_bips - The maximum power utilization during the period
 *
 * @note We express the power data here in basis points (1/100ths of 1% so that we can stick to integer math
 * while preserving resolution.
 */
struct neuron_power_stats {
        enum neuron_power_stats_status status;
        struct timespec64 time_of_sample;
        u32 avg_power_bips;
        u16 min_power_bips;
        u16 max_power_bips;
};

/**
 * Per-die state recorded by the sampler each tick.
 *
 *   GOOD       - read succeeded and util_bips <= NEURON_MAX_POWER_UTIL_BIPS.
 *   READ_ERROR - the firmware read returned a nonzero error.
 *   BOGUS      - the read succeeded but util_bips was out of range.
 */
enum neuron_power_die_state {
        NEURON_POWER_DIE_STATE_GOOD = 0,
        NEURON_POWER_DIE_STATE_READ_ERROR,
        NEURON_POWER_DIE_STATE_BOGUS,
};

/**
 * One per-die cache entry. Written by the sampler-side cache update helper,
 * read by npower_format_raw(). Access is serialized by stats_lock.
 *
 * @populated: False until the sampler has read power data for this die since 
 *             the last npower_init_stats(); while false, no other field is 
 *             meaningful and the formatter emits "<die>,unavailable\n".
 * @state:     last per-die state. Only read when populated.
 * @util_bips: if state == GOOD: cached in-range util reading.
 *             if state == BOGUS: the offending out-of-range util value (so the formatter
 *                    can emit "bogus(<util_bips>)").
 *             if state == READ_ERROR: undefined.
 * @counter:   if state == GOOD or BOGUS: firmware sample counter associated with util_bips.
 *             if state == READ_ERROR: undefined.
 */
struct neuron_power_die_cache_entry {
        bool populated;
        enum neuron_power_die_state state;
        u16 util_bips;
        u16 counter;
};

struct neuron_power {
        struct neuron_power_samples current_samples; // Unaggregated raw data for the current period
        struct neuron_power_stats current_stats; // Aggregated stats from the last completed period
        // Per-die cache backing the raw power sysfs interface. Updated by
        // the periodic sampler, read by npower_format_raw(). Serialized by
        // stats_lock.
        struct neuron_power_die_cache_entry per_die_cache[NEURON_POWER_MAX_DIE];
        struct mutex stats_lock;
};

/**
 * npower_sample_utilization() - read power utilization from firmware and, if there is new
 *                                      data found, store it.
 *
 * @nd: pointer to the neuron device whose power utilization is to be sampled
 *
  * @return: 0 on success, nonzero on failure
 */
int npower_sample_utilization(void *dev);

/**
  * npower_format_power_stats() - Formats a neuron_power_stats struct into a string
  *
  * @nd - expected to point to a struct neuron_device whose power stats we want to format
  * @stats - the stats to format
  * @buffer - location to write the formatted string into
  * @bufflen - the length of the buffer
  *
  * Returns 0 if stats could be retrieved and fit in buffer, nonzero if not.  Note that cases in
  * which the stats are retrievable but not OK (for example because neuron doesn't have enough data
  * points for the past minute) are handled via the status field in the formatted output rather than
  * the return value here.
  *
  * @note This function is not thread safe and should only be called with a local copy of the stats
  *
  * @note This function will return a truncated string if the buffer is not large enough to hold the formatted string,
  *       and will return a formatted string if it receives invalid parameters (e.g. a null nd pointer)
  */
int npower_format_stats(void *nd, char buffer[], unsigned int bufflen);

/**
 * npower_format_raw() - Read live per-die power utilization from firmware and
 *                       format it as CSV.  Unlike npower_format_stats, this
 *                       does not consult the per-minute aggregate; it issues a
 *                       fresh MMIO read on every call so callers can poll at
 *                       the firmware refresh rate.
 *
 * @nd: expected to point to a struct neuron_device
 * @buffer / @bufflen: destination
 *
 * Returns 0 on success.
 */
int npower_format_raw(void *nd, char buffer[], unsigned int bufflen);

/**
 * npower_init_power_stats() - Initializes power stats for the specified neuron device upon boot
 *
 * @nd: expected to point to a struct neuron_device whose power stats we want to init
 *
 * Returns 0 on success, < 0 error code on failure
 *
 */
int npower_init_stats(struct neuron_device *nd);

/**
 * npower_power_enabled_in_fw() - tells whether the firmware is collecting and publishing power utilization
 *
 * @nd: the neuron device whose firmware we are checking
 *
 * @return: true if the firmware is collecting and publishing power utilization, false otherwise
 */
bool npower_enabled_in_fw(struct neuron_device *nd);

#endif // _NEURON_POWER_H
