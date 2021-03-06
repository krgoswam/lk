/*
 * Copyright (c) 2013, Google Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <platform.h>
#include <platform/interrupts.h>
#include <platform/timer.h>
#include <trace.h>

#define LOCAL_TRACE 1

#define LTRACEF_LEVEL(level, x...) do { if (LOCAL_TRACE >= level) { TRACEF(x); } } while (0)

#include "fixed_point.h"

static platform_timer_callback t_callback;

struct fp_32_64 cntpct_per_ms;
struct fp_32_64 ms_per_cntpct;
struct fp_32_64 us_per_cntpct;

static uint64_t lk_time_to_cntpct(lk_time_t lk_time)
{
	return u64_mul_u32_fp32_64(lk_time, cntpct_per_ms);
}

static lk_time_t cntpct_to_lk_time(uint64_t cntpct)
{
	return u32_mul_u64_fp32_64(cntpct, ms_per_cntpct);
}

static lk_bigtime_t cntpct_to_lk_bigtime(uint64_t cntpct)
{
	return u64_mul_u64_fp32_64(cntpct, us_per_cntpct);
}

static uint32_t read_cntfrq(void)
{
	uint32_t cntfrq;

	__asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r" (cntfrq));
	LTRACEF("cntfrq: 0x%08x, %u\n", cntfrq, cntfrq);
	return cntfrq;
}

static uint32_t read_cntp_ctl(void)
{
	uint32_t cntp_ctl;

	__asm__ volatile("mrc p15, 0, %0, c14, c2, 1" : "=r" (cntp_ctl));
	return cntp_ctl;
}

static void write_cntp_ctl(uint32_t cntp_ctl)
{
	LTRACEF_LEVEL(3, "cntp_ctl: 0x%x\n", cntp_ctl);
	__asm__ volatile("mcr p15, 0, %0, c14, c2, 1" :: "r" (cntp_ctl));
}

static void write_cntp_cval(uint64_t cntp_cval)
{
	LTRACEF_LEVEL(3, "cntp_cval: 0x%016llx, %llu\n", cntp_cval, cntp_cval);
	__asm__ volatile("mcrr p15, 2, %0, %H0, c14" :: "r" (cntp_cval));
}

static void write_cntp_tval(int32_t cntp_tval)
{
	LTRACEF_LEVEL(3, "cntp_tval: 0x%08x, %d\n", cntp_tval, cntp_tval);
	__asm__ volatile("mcr p15, 0, %0, c14, c2, 0" :: "r" (cntp_tval));
}

static uint64_t read_cntpct(void)
{
	uint64_t cntpct;

	__asm__ volatile("mrrc p15, 0, %0, %H0, c14" : "=r" (cntpct));
	LTRACEF_LEVEL(3, "cntpct: 0x%016llx, %llu\n", cntpct, cntpct);
	return cntpct;
}

static enum handler_return platform_tick(void *arg)
{
	write_cntp_ctl(0);
	if (t_callback) {
		return t_callback(arg, current_time());
	} else {
		return INT_NO_RESCHEDULE;
	}
}

status_t platform_set_oneshot_timer(platform_timer_callback callback, void *arg, lk_time_t interval)
{
	uint64_t cntpct_interval = lk_time_to_cntpct(interval);

	ASSERT(arg == NULL);

	t_callback = callback;
	if (cntpct_interval <= INT_MAX)
		write_cntp_tval(cntpct_interval);
	else
		write_cntp_cval(read_cntpct() + cntpct_interval);
	write_cntp_ctl(1);
	return 0;
}

void platform_stop_timer(void)
{
	write_cntp_ctl(0);
}

lk_bigtime_t current_time_hires(void)
{
	return cntpct_to_lk_bigtime(read_cntpct());
}

lk_time_t current_time(void)
{
	return cntpct_to_lk_time(read_cntpct());
}

void arm_generic_timer_init_secondary_cpu(void)
{
}

static uint32_t abs_int32(int32_t a)
{
	return (a > 0) ? a : -a;
}

static uint64_t abs_int64(int64_t a)
{
	return (a > 0) ? a : -a;
}

static void test_time_conversion_check_result(uint64_t a, uint64_t b, uint64_t limit, bool is32)
{
	if (a != b) {
		uint64_t diff = is32 ? abs_int32(a - b) : abs_int64(a - b);
		if (diff <= limit)
			LTRACEF("ROUNDED by %llu (up to %llu allowed)\n", diff, limit);
		else
			TRACEF("FAIL, off by %llu\n", diff);
	}
}

static void test_lk_time_to_cntpct(uint32_t cntfrq, lk_time_t lk_time)
{
	uint64_t cntpct = lk_time_to_cntpct(lk_time);
	uint64_t expected_cntpct = ((uint64_t)cntfrq * lk_time + 500) / 1000;

	test_time_conversion_check_result(cntpct, expected_cntpct, 1, false);
	LTRACEF_LEVEL(2, "lk_time_to_cntpct(%lu): got %llu, expect %llu\n", lk_time, cntpct, expected_cntpct);
}

static void test_cntpct_to_lk_time(uint32_t cntfrq, lk_time_t expected_lk_time, uint32_t wrap_count)
{
	lk_time_t lk_time;
	uint64_t cntpct;

	cntpct = (uint64_t)cntfrq * expected_lk_time / 1000;
	if ((uint64_t)cntfrq * wrap_count > UINT_MAX)
		cntpct += (((uint64_t)cntfrq << 32) / 1000) * wrap_count;
	else
		cntpct += (((uint64_t)(cntfrq * wrap_count) << 32) / 1000);
	lk_time = cntpct_to_lk_time(cntpct);

	test_time_conversion_check_result(lk_time, expected_lk_time, (1000 + cntfrq - 1) / cntfrq, true);
	LTRACEF_LEVEL(2, "cntpct_to_lk_time(%llu): got %lu, expect %lu\n", cntpct, lk_time, expected_lk_time);
}

static void test_cntpct_to_lk_bigtime(uint32_t cntfrq, uint64_t expected_s)
{
	lk_bigtime_t expected_lk_bigtime = expected_s * 1000 * 1000;
	uint64_t cntpct = (uint64_t)cntfrq * expected_s;
	lk_bigtime_t lk_bigtime = cntpct_to_lk_bigtime(cntpct);

	test_time_conversion_check_result(lk_bigtime, expected_lk_bigtime, (1000 * 1000 + cntfrq - 1) / cntfrq, false);
	LTRACEF_LEVEL(2, "cntpct_to_lk_bigtime(%llu): got %llu, expect %llu\n", cntpct, lk_bigtime, expected_lk_bigtime);
}

static void test_time_conversions(uint32_t cntfrq)
{
	test_lk_time_to_cntpct(cntfrq, 0);
	test_lk_time_to_cntpct(cntfrq, 1);
	test_lk_time_to_cntpct(cntfrq, INT_MAX);
	test_lk_time_to_cntpct(cntfrq, INT_MAX + 1U);
	test_lk_time_to_cntpct(cntfrq, ~0);
	test_cntpct_to_lk_time(cntfrq, 0, 0);
	test_cntpct_to_lk_time(cntfrq, INT_MAX, 0);
	test_cntpct_to_lk_time(cntfrq, INT_MAX + 1U, 0);
	test_cntpct_to_lk_time(cntfrq, ~0, 0);
	test_cntpct_to_lk_time(cntfrq, 0, 1);
	test_cntpct_to_lk_time(cntfrq, 0, 7);
	test_cntpct_to_lk_time(cntfrq, 0, 70);
	test_cntpct_to_lk_time(cntfrq, 0, 700);
	test_cntpct_to_lk_bigtime(cntfrq, 0);
	test_cntpct_to_lk_bigtime(cntfrq, 1);
	test_cntpct_to_lk_bigtime(cntfrq, 60 * 60 * 24);
	test_cntpct_to_lk_bigtime(cntfrq, 60 * 60 * 24 * 365);
	test_cntpct_to_lk_bigtime(cntfrq, 60 * 60 * 24 * (365 * 10 + 2));
	test_cntpct_to_lk_bigtime(cntfrq, 60ULL * 60 * 24 * (365 * 100 + 2));
}

static void arm_generic_timer_init_conversion_factors(uint32_t cntfrq)
{
	fp_32_64_div_32_32(&cntpct_per_ms, cntfrq, 1000);
	fp_32_64_div_32_32(&ms_per_cntpct, 1000, cntfrq);
	fp_32_64_div_32_32(&us_per_cntpct, 1000 * 1000, cntfrq);
	LTRACEF("cntpct_per_ms: %08x.%08x%08x\n", cntpct_per_ms.l0, cntpct_per_ms.l32, cntpct_per_ms.l64);
	LTRACEF("ms_per_cntpct: %08x.%08x%08x\n", ms_per_cntpct.l0, ms_per_cntpct.l32, ms_per_cntpct.l64);
	LTRACEF("us_per_cntpct: %08x.%08x%08x\n", us_per_cntpct.l0, us_per_cntpct.l32, us_per_cntpct.l64);
}

void arm_generic_timer_init(int irq)
{
	uint32_t cntfrq = read_cntfrq();

	if (!cntfrq) {
		TRACEF("Failed to initialize timer, frequency is 0\n");
		return;
	}

#if LOCAL_TRACE
	LTRACEF("Test min cntfrq\n");
	arm_generic_timer_init_conversion_factors(1);
	test_time_conversions(1);
	LTRACEF("Test max cntfrq\n");
	arm_generic_timer_init_conversion_factors(~0);
	test_time_conversions(~0);
	LTRACEF("Set actual cntfrq\n");
#endif
	arm_generic_timer_init_conversion_factors(cntfrq);
	test_time_conversions(cntfrq);

	register_int_handler(irq, &platform_tick, NULL);
	unmask_interrupt(irq);
}

