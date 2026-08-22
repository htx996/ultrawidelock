/**
 * @file test_ultrawidelock_port.c — the OSAL, flash and kv contracts, on the host backend.
 *
 * Files under test: tests/host/port/osal_host.c, flash_host.c, kv_host.c and
 * dgram_host.c — the
 * backend every converted module's host suite runs on, so its semantics ARE
 * the portability contract: schedule is a no-op while pending, reschedule
 * restarts the delay, a timed sem take walks the virtual clock, flash refuses
 * unaligned writes. A drift here would let module tests pass against
 * behaviour no target exhibits.
 */
#include <string.h>

#include "test.h"

#include "ultrawidelock_dgram.h"
#include "ultrawidelock_flash.h"
#include "ultrawidelock_kv.h"
#include "ultrawidelock_osal.h"

#include "port/dgram_host.h"

/* ---- recorders -------------------------------------------------------------- */

static int g_runs[8];
static int g_order[8];
static int g_order_n;

static void run_count(struct ultrawidelock_work *w)
{
	(void)w;
	g_runs[0]++;
}

static struct ultrawidelock_work g_self;
static int g_self_runs;
static void run_resubmit(struct ultrawidelock_work *w)
{
	g_self_runs++;
	if (g_self_runs == 1) {
		ultrawidelock_work_submit(w);
	}
}

static void dwork_a(struct ultrawidelock_dwork *w)
{
	(void)w;
	g_runs[1]++;
	g_order[g_order_n++] = 1;
}
static void dwork_b(struct ultrawidelock_dwork *w)
{
	(void)w;
	g_runs[2]++;
	g_order[g_order_n++] = 2;
}

static struct ultrawidelock_dwork g_rearm;
static int g_rearm_runs;
static void dwork_rearm(struct ultrawidelock_dwork *w)
{
	g_rearm_runs++;
	if (g_rearm_runs == 1) {
		ultrawidelock_dwork_schedule(w, 10);
	}
}

static ultrawidelock_sem_t g_sem;
static void dwork_give(struct ultrawidelock_dwork *w)
{
	(void)w;
	ultrawidelock_sem_give(&g_sem);
}

static void thread_entry(void *arg)
{
	*(int *)arg = 1;
}

static int g_hook_runs;
static int hook_fn(void)
{
	g_hook_runs++;
	return 0;
}
ULTRAWIDELOCK_INIT_APPLICATION(hook_fn);

/* ---- the suite --------------------------------------------------------------- */

static void test_work(void)
{
	struct ultrawidelock_work w;

	t_group("osal: immediate work");
	ultrawidelock_osal_host_reset();
	memset(g_runs, 0, sizeof(g_runs));

	ultrawidelock_work_init(&w, run_count);
	T_EQ("nothing queued runs nothing", (long)ultrawidelock_osal_host_flush(), 0L);
	ultrawidelock_work_submit(&w);
	T_EQ("submit twice while pending queues once", ultrawidelock_work_submit(&w), 0L);
	T_EQ("flush runs it once", (long)ultrawidelock_osal_host_flush(), 1L);
	T_EQ("the handler ran once", (long)g_runs[0], 1L);
	T_EQ("the queue is drained", (long)ultrawidelock_osal_host_flush(), 0L);

	g_self_runs = 0;
	ultrawidelock_work_init(&g_self, run_resubmit);
	ultrawidelock_work_submit(&g_self);
	T_EQ("a self-resubmitting handler runs once per flush", (long)ultrawidelock_osal_host_flush(), 1L);
	T_EQ("its resubmission waits for the next flush", (long)ultrawidelock_osal_host_flush(), 1L);
	T_EQ("then the queue is quiet", (long)ultrawidelock_osal_host_flush(), 0L);
	T_EQ("two runs total", (long)g_self_runs, 2L);
}

static void test_dwork(void)
{
	struct ultrawidelock_dwork a, b;

	t_group("osal: delayable work");
	ultrawidelock_osal_host_reset();
	memset(g_runs, 0, sizeof(g_runs));
	g_order_n = 0;

	ultrawidelock_dwork_init(&a, dwork_a);
	ultrawidelock_dwork_init(&b, dwork_b);

	/* The k_work_delayable distinction the rxdiag windowing depends on:
	 * schedule keeps the earlier deadline, reschedule restarts it. */
	ultrawidelock_dwork_schedule(&a, 50);
	ultrawidelock_osal_host_advance_ms(10);
	ultrawidelock_dwork_schedule(&a, 100);
	T_EQ("schedule while armed is a no-op", (long)ultrawidelock_osal_host_advance_ms(39), 0L);
	T_EQ("the original deadline stands", (long)ultrawidelock_osal_host_advance_ms(1), 1L);
	T_EQ("ran exactly once", (long)g_runs[1], 1L);

	ultrawidelock_dwork_schedule(&a, 50);
	ultrawidelock_osal_host_advance_ms(10);
	ultrawidelock_dwork_reschedule(&a, 50);
	T_EQ("reschedule restarted the delay", (long)ultrawidelock_osal_host_advance_ms(49), 0L);
	T_EQ("new deadline fires", (long)ultrawidelock_osal_host_advance_ms(1), 1L);

	ultrawidelock_dwork_reschedule(&a, 0);
	T_EQ("delay 0 is due immediately", (long)ultrawidelock_osal_host_advance_ms(0), 1L);

	ultrawidelock_dwork_schedule(&a, 30);
	ultrawidelock_dwork_cancel(&a);
	T_EQ("cancelled work never fires", (long)ultrawidelock_osal_host_advance_ms(100), 0L);
	T_EQ("cancel of idle work is harmless", ultrawidelock_dwork_cancel(&a), 0L);

	g_order_n = 0;
	ultrawidelock_dwork_schedule(&b, 20);
	ultrawidelock_dwork_schedule(&a, 10);
	T_EQ("both fire in one window", (long)ultrawidelock_osal_host_advance_ms(25), 2L);
	T_OK("deadline order, not submit order", g_order[0] == 1 && g_order[1] == 2);

	g_rearm_runs = 0;
	ultrawidelock_dwork_init(&g_rearm, dwork_rearm);
	ultrawidelock_dwork_schedule(&g_rearm, 5);
	T_EQ("a re-arming handler fires again next window",
	     (long)ultrawidelock_osal_host_advance_ms(15), 2L);
	T_EQ("re-armed twice total", (long)g_rearm_runs, 2L);
}

static void test_sem(void)
{
	struct ultrawidelock_dwork give;

	t_group("osal: semaphore");
	ultrawidelock_osal_host_reset();

	ultrawidelock_sem_init(&g_sem, 0, 1);
	T_EQ("empty take with no wait fails", ultrawidelock_sem_take(&g_sem, 0), -1L);
	ultrawidelock_sem_give(&g_sem);
	T_EQ("give then take succeeds", ultrawidelock_sem_take(&g_sem, 0), 0L);
	ultrawidelock_sem_give(&g_sem);
	ultrawidelock_sem_give(&g_sem);
	T_EQ("gives saturate at the limit", ultrawidelock_sem_take(&g_sem, 0), 0L);
	T_EQ("only one was kept", ultrawidelock_sem_take(&g_sem, 0), -1L);
	ultrawidelock_sem_give(&g_sem);
	ultrawidelock_sem_reset(&g_sem);
	T_EQ("reset drops the count", ultrawidelock_sem_take(&g_sem, 0), -1L);

	/* The single-thread contract: a timed take advances the clock and a
	 * delayable give lands inside the window. */
	ultrawidelock_dwork_init(&give, dwork_give);
	ultrawidelock_dwork_schedule(&give, 30);
	T_EQ("a give due at 30ms satisfies a 100ms take", ultrawidelock_sem_take(&g_sem, 100), 0L);
	T_EQ("the take consumed the full window", (long)ultrawidelock_osal_host_now_ms(), 100L);

	ultrawidelock_dwork_schedule(&give, 50);
	T_EQ("a give due past the window times out", ultrawidelock_sem_take(&g_sem, 20), -1L);
	T_EQ("the timeout still elapsed", (long)ultrawidelock_osal_host_now_ms(), 120L);
	T_EQ("the late give then satisfies a forever take",
	     ultrawidelock_sem_take(&g_sem, ULTRAWIDELOCK_WAIT_FOREVER), 0L);
}

static void test_thread_and_init(void)
{
	ULTRAWIDELOCK_THREAD_STACK_DEFINE(stack, 512);
	ultrawidelock_thread_t t;
	int flag = 0;

	t_group("osal: thread + init hooks");
	T_EQ("create records and reports success",
	     ultrawidelock_thread_create(&t, stack, ULTRAWIDELOCK_THREAD_STACK_SIZEOF(stack),
					 thread_entry, &flag, ULTRAWIDELOCK_THREAD_PRIO_NORM, "t"),
	     0L);
	T_OK("host threads do not run by themselves", flag == 0 && t.created == 1);
	t.entry(t.arg);
	T_EQ("the suite drives the entry", (long)flag, 1L);

	T_EQ("init hooks run on demand", ultrawidelock_osal_init_all(), 0L);
	T_EQ("the registered hook ran", (long)g_hook_runs, 1L);
	ultrawidelock_osal_init_all();
	T_EQ("init_all is idempotent", (long)g_hook_runs, 1L);
}

static void test_flash(void)
{
	const struct ultrawidelock_flash_area *fa;
	struct ultrawidelock_flash_host_area *host;
	static const uint8_t word[4] = { 0xde, 0xad, 0xca, 0xfe };
	uint8_t back[4];

	t_group("flash: host partitions");
	ultrawidelock_flash_host_reset();

	T_EQ("staging opens", ultrawidelock_flash_open(ULTRAWIDELOCK_FLASH_AREA_STAGING, &fa), 0L);
	T_EQ("staging geometry matches pm_static.yml",
	     (long)ultrawidelock_flash_size(fa), (long)ULTRAWIDELOCK_FLASH_HOST_STAGING_SIZE);
	host = ultrawidelock_flash_host_area(ULTRAWIDELOCK_FLASH_AREA_STAGING);

	T_EQ("aligned write lands", ultrawidelock_flash_write(fa, 8, word, sizeof(word)), 0L);
	T_EQ("the bytes are there", (long)host->buf[8], 0xdeL);
	T_EQ("read round-trips", ultrawidelock_flash_read(fa, 8, back, sizeof(back)), 0L);
	T_OK("read matches write", memcmp(back, word, sizeof(word)) == 0);

	T_EQ("unaligned write offset refused", ultrawidelock_flash_write(fa, 2, word, 4), -1L);
	T_EQ("unaligned write length refused", ultrawidelock_flash_write(fa, 0, word, 3), -1L);
	T_EQ("unaligned erase refused", ultrawidelock_flash_erase(fa, 100, 4096), -1L);
	T_EQ("erase off the end refused",
	     ultrawidelock_flash_erase(fa, 0, ULTRAWIDELOCK_FLASH_HOST_STAGING_SIZE + 4096), -1L);
	T_EQ("write off the end refused",
	     ultrawidelock_flash_write(fa, ULTRAWIDELOCK_FLASH_HOST_STAGING_SIZE - 2, word, 4), -1L);

	T_EQ("page erase succeeds", ultrawidelock_flash_erase(fa, 0, 4096), 0L);
	T_EQ("erase restored 0xff", (long)host->buf[8], 0xffL);

	host->write_fail_in = 1;
	T_EQ("fail_in lets N calls through", ultrawidelock_flash_write(fa, 0, word, 4), 0L);
	T_EQ("then fails every call", ultrawidelock_flash_write(fa, 0, word, 4), -1L);
	T_EQ("still failing", ultrawidelock_flash_write(fa, 0, word, 4), -1L);

	ultrawidelock_flash_close(fa);
	T_EQ("calls were recorded", (long)host->close_calls, 1L);

	host->fail_open = 1;
	T_EQ("fail_open refuses", ultrawidelock_flash_open(ULTRAWIDELOCK_FLASH_AREA_STAGING, &fa), -1L);

	T_EQ("no reboot yet", (long)ultrawidelock_flash_host_reboots(), 0L);
	ultrawidelock_reboot();
	T_EQ("reboot recorded, not taken", (long)ultrawidelock_flash_host_reboots(), 1L);

	ultrawidelock_flash_host_reset();
	host = ultrawidelock_flash_host_area(ULTRAWIDELOCK_FLASH_AREA_STAGING);
	T_OK("reset restores knobs and recorders",
	     host->fail_open == 0 && host->write_fail_in == -1 && host->write_calls == 0);
	T_EQ("primary opens after reset",
	     ultrawidelock_flash_open(ULTRAWIDELOCK_FLASH_AREA_PRIMARY, &fa), 0L);
	T_EQ("primary geometry matches pm_static.yml",
	     (long)ultrawidelock_flash_size(fa), (long)ULTRAWIDELOCK_FLASH_HOST_PRIMARY_SIZE);
}

/* ---- kv ---------------------------------------------------------------------- */

static void test_kv(void)
{
	uint8_t big[ULTRAWIDELOCK_KV_VALUE_MAX];
	uint8_t out[64];
	size_t len;

	t_group("kv: get, set, delete");
	T_EQ("erase_all mounts", ultrawidelock_kv_erase_all(), (long)ULTRAWIDELOCK_KV_OK);
	T_EQ("init is idempotent", ultrawidelock_kv_init(), (long)ULTRAWIDELOCK_KV_OK);
	len = sizeof(out);
	T_EQ("absent key is NOT_FOUND", ultrawidelock_kv_get(0x4000u, out, &len),
	     (long)ULTRAWIDELOCK_KV_NOT_FOUND);
	T_EQ("set ok", ultrawidelock_kv_set(0x4000u, "abcd", 4u), (long)ULTRAWIDELOCK_KV_OK);
	len = sizeof(out);
	T_EQ("get ok", ultrawidelock_kv_get(0x4000u, out, &len), (long)ULTRAWIDELOCK_KV_OK);
	T_EQ("length is the stored length", (long)len, 4L);
	T_OK("value round-trips", memcmp(out, "abcd", 4) == 0);
	T_EQ("set replaces", ultrawidelock_kv_set(0x4000u, "zy", 2u), (long)ULTRAWIDELOCK_KV_OK);
	len = sizeof(out);
	(void)ultrawidelock_kv_get(0x4000u, out, &len);
	T_EQ("replacement shortened the record", (long)len, 2L);
	T_OK("replacement value", memcmp(out, "zy", 2) == 0);

	/* The truncation trap this seam exists to close: a caller whose buffer is
	 * too small must be refused and told the real length, never handed a
	 * short value it would use as a key. */
	t_group("kv: undersized read is refused, not truncated");
	T_EQ("set 40 bytes", ultrawidelock_kv_set(0x4001u, big, 40u), (long)ULTRAWIDELOCK_KV_OK);
	len = 8u;
	memset(out, 0xee, sizeof(out));
	T_EQ("short buffer -> INVALID", ultrawidelock_kv_get(0x4001u, out, &len),
	     (long)ULTRAWIDELOCK_KV_INVALID);
	T_EQ("stored length handed back", (long)len, 40L);
	T_OK("buffer untouched", out[0] == 0xee);
	len = 0u;
	T_EQ("NULL asks length only", ultrawidelock_kv_get(0x4001u, NULL, &len),
	     (long)ULTRAWIDELOCK_KV_OK);
	T_EQ("length answered", (long)len, 40L);

	t_group("kv: guards");
	T_EQ("KEY_NONE is not a key",
	     ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_NONE, "x", 1u),
	     (long)ULTRAWIDELOCK_KV_INVALID);
	T_EQ("oversize value refused",
	     ultrawidelock_kv_set(0x4002u, big, ULTRAWIDELOCK_KV_VALUE_MAX + 1u),
	     (long)ULTRAWIDELOCK_KV_INVALID);
	T_EQ("max-size value accepted",
	     ultrawidelock_kv_set(0x4002u, big, ULTRAWIDELOCK_KV_VALUE_MAX),
	     (long)ULTRAWIDELOCK_KV_OK);

	t_group("kv: delete and erase_all");
	T_EQ("delete ok", ultrawidelock_kv_delete(0x4000u), (long)ULTRAWIDELOCK_KV_OK);
	len = sizeof(out);
	T_EQ("deleted key is gone", ultrawidelock_kv_get(0x4000u, out, &len),
	     (long)ULTRAWIDELOCK_KV_NOT_FOUND);
	T_EQ("second delete is NOT_FOUND", ultrawidelock_kv_delete(0x4000u),
	     (long)ULTRAWIDELOCK_KV_NOT_FOUND);
	T_EQ("erase_all ok", ultrawidelock_kv_erase_all(), (long)ULTRAWIDELOCK_KV_OK);
	len = sizeof(out);
	T_EQ("erase_all took the survivors too", ultrawidelock_kv_get(0x4001u, out, &len),
	     (long)ULTRAWIDELOCK_KV_NOT_FOUND);
}

/* ---- dgram ------------------------------------------------------------------ */

static uint8_t g_rx[ULTRAWIDELOCK_DGRAM_MAX];
static size_t g_rx_len;
static int g_rx_calls;
static void *g_rx_ctx;

static void rx_record(void *ctx, const uint8_t *data, size_t len)
{
	g_rx_ctx = ctx;
	g_rx_len = len;
	g_rx_calls++;
	memcpy(g_rx, data, len);
}

static void test_dgram(void)
{
	uint8_t out[ULTRAWIDELOCK_DGRAM_MAX];
	uint8_t big[ULTRAWIDELOCK_DGRAM_MAX + 1];
	int marker = 0;

	memset(big, 0xA5, sizeof(big));
	dgram_host_reset();
	g_rx_calls = 0;

	T_EQ("closed link is not ready", (long)ultrawidelock_dgram_ready(), 0L);
	/* The failure that matters most: a link nobody opened must refuse to
	 * send, not swallow. Both consumers fail closed on silence, so a fake
	 * that accepted this would let a missing open() pass every test. */
	T_EQ("send before open -> CLOSED", ultrawidelock_dgram_send("x", 1u),
	     (long)ULTRAWIDELOCK_DGRAM_CLOSED);
	T_EQ("delivery to a closed link runs no callback",
	     (long)dgram_host_deliver("x", 1u), 0L);
	T_EQ("and the callback did not run", (long)g_rx_calls, 0L);

	T_EQ("NULL callback refused", ultrawidelock_dgram_open(1234u, NULL, NULL),
	     (long)ULTRAWIDELOCK_DGRAM_INVALID);
	T_EQ("open ok", ultrawidelock_dgram_open(1234u, rx_record, &marker),
	     (long)ULTRAWIDELOCK_DGRAM_OK);
	T_EQ("open is idempotent", ultrawidelock_dgram_open(1234u, rx_record, &marker),
	     (long)ULTRAWIDELOCK_DGRAM_OK);
	T_EQ("ready after open", (long)ultrawidelock_dgram_ready(), 1L);
	T_EQ("bound to the port it was given", (long)dgram_host_port(), 1234L);

	T_EQ("send ok", ultrawidelock_dgram_send("abcd", 4u), (long)ULTRAWIDELOCK_DGRAM_OK);
	T_EQ("one datagram sent", (long)dgram_host_sent_count(), 1L);
	T_EQ("captured whole", (long)dgram_host_sent(0u, out, sizeof(out)), 4L);
	T_EQ("captured verbatim", (long)memcmp(out, "abcd", 4u), 0L);
	/* A group-addressed link does hear itself on a real network, but the
	 * fake must not echo on its own: a hidden loopback would put a datagram
	 * through every consumer's parse in cases that only meant to send. */
	T_EQ("send did not loop back", (long)g_rx_calls, 0L);

	T_EQ("zero length refused", ultrawidelock_dgram_send("x", 0u),
	     (long)ULTRAWIDELOCK_DGRAM_INVALID);
	T_EQ("oversize refused", ultrawidelock_dgram_send(big, sizeof(big)),
	     (long)ULTRAWIDELOCK_DGRAM_INVALID);
	T_EQ("max size accepted", ultrawidelock_dgram_send(big, ULTRAWIDELOCK_DGRAM_MAX),
	     (long)ULTRAWIDELOCK_DGRAM_OK);
	T_EQ("refusals were not counted as sends", (long)dgram_host_sent_count(), 2L);

	T_EQ("delivery runs the callback", (long)dgram_host_deliver("hello", 5u), 1L);
	T_EQ("callback ran once", (long)g_rx_calls, 1L);
	T_EQ("length delivered", (long)g_rx_len, 5L);
	T_EQ("bytes delivered", (long)memcmp(g_rx, "hello", 5u), 0L);
	T_EQ("context handed back", (long)(g_rx_ctx == &marker), 1L);

	/* The backend drops these before the consumer sees them; so must the
	 * fake, or the host suite is testing a link that does not exist. */
	T_EQ("empty datagram dropped", (long)dgram_host_deliver("x", 0u), 0L);
	T_EQ("oversize datagram dropped",
	     (long)dgram_host_deliver(big, sizeof(big)), 0L);
	T_EQ("neither reached the callback", (long)g_rx_calls, 1L);

	(void)ultrawidelock_dgram_close();
	dgram_host_open_rc = ULTRAWIDELOCK_DGRAM_IO;
	T_EQ("injected open failure surfaces",
	     ultrawidelock_dgram_open(1234u, rx_record, NULL),
	     (long)ULTRAWIDELOCK_DGRAM_IO);
	T_EQ("and it was one-shot", ultrawidelock_dgram_open(1234u, rx_record, &marker),
	     (long)ULTRAWIDELOCK_DGRAM_OK);
	dgram_host_send_rc = ULTRAWIDELOCK_DGRAM_IO;
	T_EQ("injected send failure surfaces", ultrawidelock_dgram_send("q", 1u),
	     (long)ULTRAWIDELOCK_DGRAM_IO);
	T_EQ("and it was one-shot too", ultrawidelock_dgram_send("q", 1u),
	     (long)ULTRAWIDELOCK_DGRAM_OK);

	T_EQ("close ok", ultrawidelock_dgram_close(), (long)ULTRAWIDELOCK_DGRAM_OK);
	T_EQ("not ready after close", (long)ultrawidelock_dgram_ready(), 0L);
	T_EQ("close is safe twice", ultrawidelock_dgram_close(),
	     (long)ULTRAWIDELOCK_DGRAM_OK);
	T_EQ("a closed link sends nothing", ultrawidelock_dgram_send("x", 1u),
	     (long)ULTRAWIDELOCK_DGRAM_CLOSED);
}

void test_ultrawidelock_port(void)
{
	test_work();
	test_dwork();
	test_sem();
	test_thread_and_init();
	test_flash();
	test_kv();
	test_dgram();
}
