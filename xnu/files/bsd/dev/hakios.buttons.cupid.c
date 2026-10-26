/*
 * The buttons of the Xiaomi 12 (cupid) -- power, volume up, volume down -- as a source of
 * button records for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg buttons=1:
 * every address below belongs to this phone.
 *
 * None of the three is a pin of the SoC:
 *   power        KPDPWR_N of the power-on block ("PON") of the PMK8350
 *   volume down  RESIN_N of the same block
 *   volume up    GPIO6 of the PM8350, pulled up; the key pulls it low
 * (the phone's device tree: pon_hlos@1300 and gpio_keys' vol_up, both on the first SPMI bus).
 * Both chips sit on the SPMI bus (slaves 0 and 1). The way to that bus is the SoC's "PMIC
 * arbiter" -- here version 7, whose layout differs from the older one in hakios.buttons.c:
 * the APID-to-peripheral map is at 0x2000 of the "core" block, and the observer channels are
 * 0x8000 apart per system, 0x20 per APID. Reads go through the "observer" channels, which
 * every system has for every peripheral, whoever owns it.
 *
 * THIS DRIVER ONLY READS, for the reason hakios.buttons.c gives: the PMICs make the phone's
 * supplies. What a press is, and the mapping to the iPhone's side and volume buttons, are as
 * there. A thread reads the two status registers 60 times a second, and a key has changed
 * when two looks in a row say so.
 *
 * Register layouts: Linux drivers/spmi/spmi-pmic-arb.c (version 7),
 * drivers/input/misc/pm8941-pwrkey.c, drivers/pinctrl/qcom/pinctrl-spmi-gpio.c, and the
 * phone's own device tree (qcom,spmi@c42d000, pon_hlos@1300, gpio_keys).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* Every line goes to the loader's log directly as well, as in hakios.buttons.c. */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- the PMIC arbiter, version 7 ---------------------------------------------------------------- */

#define ARB_CORE_PHYS           0x0c400000UL    /* the version, the APID counts, the APID map */
#define ARB_CORE_SIZE           0x3000UL
#define ARB_CNFG_PHYS           0x0c42d000UL    /* which system owns each APID (bus 0) */
#define ARB_CNFG_SIZE           0x4000UL
#define ARB_OBSERVER_PHYS       0x0c440000UL    /* the observer channels: 0x8000 per system */
#define ARB_OBSERVER_SIZE       0x80000UL
#define ARB_SYSTEM              0               /* the device tree's qcom,ee */

#define ARB_VERSION             0x000
#define ARB_VERSION_7           0x70000000u
#define ARB_VERSION_8           0x80000000u
#define ARB_FEATURES            0x004           /* [10:0] the APIDs of bus 0; the next word: bus 1 */
#define ARB_APID_MAP            0x2000          /* one word per APID: [19:8] the PPID it serves */
#define ARB_APID_MAX            1024

#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18            /* the bytes read, the first in the lowest byte */
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long"; [11:4] register, [2:0] bytes - 1 */
#define READ_TIMEOUT_US         2000

static vm_offset_t arb_core, arb_observer;
static LCK_GRP_DECLARE(cupid_spmi_lck_grp, "cupid-spmi");
static LCK_SPIN_DECLARE(cupid_spmi_lock, &cupid_spmi_lck_grp);

struct peripheral {
	const char *    name;
	uint16_t        ppid;           /* slave << 8 | peripheral */
	uint16_t        apid;
	bool            found;
};
static struct peripheral pon = { .name = "the PMK8350's power-on block", .ppid = 0x0013 };     /* registers 0x1300 of slave 0 */
static struct peripheral pin = { .name = "GPIO6 of the PM8350", .ppid = 0x018e };              /* registers 0x8e00 of slave 1 */

/* How the reads have gone: said once a minute. */
static struct {
	uint64_t reads, failed, total_ns, longest_ns, last_ns;
} bus;

/*
 * Which APID serves that peripheral. The map is one word per APID, for both buses; the first
 * that names the peripheral will do (an observer read is allowed to every system).
 */
static void
find_apid(struct peripheral *p, uint32_t apids)
{
	unsigned matches = 0;

	for (uint32_t n = 0; n < apids; n++) {
		uint32_t map = rd(arb_core + ARB_APID_MAP + 4 * n);

		if (map == 0 || (uint32_t)(map >> 8 & 0xfff) != p->ppid) {
			continue;
		}
		printf("buttons: APID %u is %s (PPID %#x)\n", n, p->name, p->ppid);
		if (matches++ == 0) {
			p->found = true;
			p->apid = (uint16_t)n;
		}
	}
	if (!p->found) {
		printf("buttons: no APID serves %s (slave %u, peripheral %#x)\n", p->name, p->ppid >> 8, p->ppid & 0xff);
	}
}

/* Read `count` (1 to 8) registers of a peripheral, from register `reg` on. Returns NULL, or what went wrong. */
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x20UL * p->apid;
	uint64_t start = mach_absolute_time(), now, interval, taken;
	uint32_t status, data[2];
	const char *problem = NULL;

	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	lck_spin_lock(&cupid_spmi_lock);
	wr(channel + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(channel + OBSERVER_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	lck_spin_unlock(&cupid_spmi_lock);

	if (!(status & STATUS_DONE)) {
		problem = "no answer from the arbiter";
	} else if (status & STATUS_DENIED) {
		problem = "the arbiter refuses";
	} else if (status & STATUS_FAILURE) {
		problem = "the transfer failed (nobody at that address?)";
	} else if (status & STATUS_DROPPED) {
		problem = "the transfer was dropped";
	} else {
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	absolutetime_to_nanoseconds(now - start, &taken);
	bus.reads++;
	bus.total_ns += taken;
	bus.last_ns = taken;
	if (taken > bus.longest_ns) {
		bus.longest_ns = taken;
	}
	if (problem != NULL) {
		bus.failed++;
	}
	return problem;
}

/* ---- the two peripherals ------------------------------------------------------------------------ */

#define REG_TYPE                0x04
#define REG_SUBTYPE             0x05
#define REG_RT_STATUS           0x10            /* the inputs as they are now */

/* the power-on block, of the kind that is split in two ("generation 3"): its half for the operating system */
#define PON_TYPE                0x01
#define PON_SUBTYPE_HLOS        0x09
#define PON_KPDPWR              0x80
#define PON_RESIN               0x40

/* a GPIO of the low or medium voltage kind */
#define GPIO_TYPE               0x10
#define GPIO_SUBTYPE_LV         0x10
#define GPIO_SUBTYPE_MV_VIN3    0x13
#define GPIO_LEVEL              0x01
#define GPIO_REG_MODE           0x40
#define GPIO_REG_SUPPLY         0x41
#define GPIO_REG_PULL           0x42
#define GPIO_REG_ENABLE         0x46

#define POLL_MS                 16
#define MINUTE_TICKS            (60 * 1000 / POLL_MS)

static struct button {
	const char *    name;
	uint32_t        record;
	bool            usable;
	bool            live;
	bool            last;
	bool            down;
	uint64_t        since;
	uint32_t        presses;
} power = { .name = "power", .record = HAKIOS_BUTTON_LOCK },
    volume_up = { .name = "volume up", .record = HAKIOS_BUTTON_VOLUME_UP },
    volume_down = { .name = "volume down", .record = HAKIOS_BUTTON_VOLUME_DOWN };

static void
now_text(char *text, size_t size, uint64_t now)
{
	clock_sec_t seconds;
	clock_usec_t micro;
	uint64_t up;

	absolutetime_to_nanoseconds(now, &up);
	clock_get_calendar_microtime(&seconds, &micro);
	snprintf(text, size, "%llu.%03llu s (%02u:%02u:%02u.%03u UTC)", up / NSEC_PER_SEC, up / NSEC_PER_MSEC % 1000,
	    (unsigned)(seconds / 3600 % 24), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60), (unsigned)(micro / 1000));
}

static void
button_look(struct button *b, bool pressed)
{
	uint64_t now = mach_absolute_time(), held;
	bool steady = pressed == b->last;
	char when[64];

	b->last = pressed;
	if (!b->usable || !steady) {
		return;
	}
	if (!b->live) {
		if (!pressed) {
			b->live = true;
			printf("buttons: %s is released now: believed from here on\n", b->name);
		}
		return;
	}
	if (pressed == b->down) {
		return;
	}
	b->down = pressed;
	now_text(when, sizeof(when), now);
	if (pressed) {
		b->since = now;
		b->presses++;
		printf("buttons: %s PRESSED at %s\n", b->name, when);
	} else {
		absolutetime_to_nanoseconds(now - b->since, &held);
		printf("buttons: %s released at %s, after %llu ms\n", b->name, when, held / NSEC_PER_MSEC);
	}
	hakios_touch_post(pressed ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, b->record, 0, 0);
}

static void
buttons_poll(void)
{
	static unsigned said;
	const char *problem;
	uint8_t status;

	if (power.usable || volume_down.usable) {
		problem = spmi_read(&pon, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&power, status & PON_KPDPWR);
			button_look(&volume_down, status & PON_RESIN);
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pon.name, problem);
		}
	}
	if (volume_up.usable) {
		problem = spmi_read(&pin, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&volume_up, !(status & GPIO_LEVEL));
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pin.name, problem);
		}
	}
}

/* Is the peripheral what the registers above are those of? */
static bool
identify(const struct peripheral *p, uint8_t type, uint8_t subtype_from, uint8_t subtype_to)
{
	const char *problem;
	uint8_t is[2];

	if (!p->found) {
		return false;
	}
	problem = spmi_read(p, REG_TYPE, is, 2);
	if (problem != NULL) {
		printf("buttons: %s: %s\n", p->name, problem);
		return false;
	}
	printf("buttons: %s says type %#x, subtype %#x (the read took %llu us)\n", p->name, is[0], is[1], bus.last_ns / 1000);
	if (is[0] != type || is[1] < subtype_from || is[1] > subtype_to) {
		printf("buttons: ... which is not what this driver knows (type %#x, subtype %#x to %#x): left alone\n", type,
		    subtype_from, subtype_to);
		return false;
	}
	return true;
}

/* Nothing to check for the power-on block but what it says now. */
static void
pon_inputs(void)
{
	const char *problem;
	uint8_t status;

	problem = spmi_read(&pon, REG_RT_STATUS, &status, 1);
	if (problem != NULL) {
		printf("buttons: %s: %s\n", pon.name, problem);
		return;
	}
	printf("buttons: power-on block: status %#x (power %s, volume down %s)\n", status,
	    (status & PON_KPDPWR) ? "HELD" : "released", (status & PON_RESIN) ? "HELD" : "released");
	power.usable = volume_down.usable = true;
	power.last = status & PON_KPDPWR;
	volume_down.last = status & PON_RESIN;
}

/* The pin is volume up only if it is an input with a pull-up, and switched on. */
static void
pin_input(void)
{
	const char *problem;
	uint8_t c[8], status = 0;
	unsigned mode, pull;

	problem = spmi_read(&pin, GPIO_REG_MODE, c, 8);
	if (problem == NULL) {
		problem = spmi_read(&pin, REG_RT_STATUS, &status, 1);
	}
	if (problem != NULL) {
		printf("buttons: %s: %s\n", pin.name, problem);
		return;
	}
	mode = c[0] & 3;
	pull = c[GPIO_REG_PULL - GPIO_REG_MODE] & 7;
	printf("buttons: volume up's pin: mode %u (%s), supply %u, pull %u (%s), %s; level %s\n", mode,
	    mode == 0 ? "input" : mode == 1 ? "output" : mode == 2 ? "input and output" : "analog",
	    c[GPIO_REG_SUPPLY - GPIO_REG_MODE] & 7, pull, pull < 4 ? "up" : pull == 4 ? "down" : "none",
	    (c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80) ? "enabled" : "DISABLED",
	    (status & GPIO_LEVEL) ? "high (released)" : "LOW (held)");
	if (mode != 0 || pull >= 4 || !(c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80)) {
		printf("buttons: volume up's pin is not an enabled input with a pull-up. Making it one is a write to the PMIC, "
		    "which this driver does not do: volume up is left alone\n");
		return;
	}
	volume_up.usable = true;
	volume_up.last = !(status & GPIO_LEVEL);
}

static void
say_summary(void)
{
	printf("buttons: so far %u presses of power, %u of volume up, %u of volume down; now%s%s%s%s; %llu reads of the bus, "
	    "%llu us each (the longest %llu), %llu failed\n", power.presses, volume_up.presses, volume_down.presses,
	    power.down ? " power" : "", volume_up.down ? " volume-up" : "", volume_down.down ? " volume-down" : "",
	    (power.down || volume_up.down || volume_down.down) ? " held" : " none held", bus.reads,
	    bus.reads ? bus.total_ns / bus.reads / 1000 : 0, bus.longest_ns / 1000, bus.failed);
}

static void
buttons_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter, version, apids, apids1;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	apids = rd(arb_core + ARB_FEATURES) & 0x7ff;
	apids1 = rd(arb_core + ARB_FEATURES + 4) & 0x7ff;
	printf("buttons: PMIC arbiter version %#x, %u APIDs on bus 0 and %u on bus 1\n", version, apids, apids1);
	if (version < ARB_VERSION_7 || version >= ARB_VERSION_8) {
		printf("buttons: this driver knows version 7 of the arbiter only: no buttons\n");
		goto out;
	}
	apids += apids1;
	if (apids == 0 || apids > ARB_APID_MAX) {
		apids = ARB_APID_MAX;
	}
	if (steps < 2) {
		goto out;
	}

	find_apid(&pon, apids);
	find_apid(&pin, apids);
	if (steps < 3) {
		goto out;
	}

	if (identify(&pon, PON_TYPE, PON_SUBTYPE_HLOS, PON_SUBTYPE_HLOS)) {
		pon_inputs();
	}
	if (identify(&pin, GPIO_TYPE, GPIO_SUBTYPE_LV, GPIO_SUBTYPE_MV_VIN3)) {
		pin_input();
	}
	if (!power.usable && !volume_up.usable) {
		printf("buttons: nothing that can be read: no buttons\n");
		goto out;
	}
	if (power.last || volume_down.last || volume_up.last) {
		printf("buttons: a key reads as held before anything was watched:%s%s%s. It is not reported until it has been "
		    "seen released\n", power.last ? " power" : "", volume_up.last ? " volume-up" : "",
		    volume_down.last ? " volume-down" : "");
	}
	power.live = power.usable && !power.last;
	volume_down.live = volume_down.usable && !volume_down.last;
	volume_up.live = volume_up.usable && !volume_up.last;
	if (steps < 4) {
		goto out;
	}

	printf("buttons: watching%s%s%s, every %u ms: power is the side button\n", power.usable ? " power" : "",
	    volume_up.usable ? " volume-up" : "", volume_down.usable ? " volume-down" : "", POLL_MS);
	for (unsigned tick = 1;; tick++) {
		buttons_poll();
		if (tick % MINUTE_TICKS == 0 || tick == 10 * 1000 / POLL_MS) {
			say_summary();
		}
		if (bus.failed >= 1000 && bus.failed * 2 > bus.reads) {
			printf("buttons: more than half of %llu reads have failed: giving up\n", bus.reads);
			break;
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
out:
	if (steps < 4) {
		printf("buttons: stopping here (buttons_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
hakios_buttons_cupid_init(void)
{
	uint32_t enabled = 0, steps = 4;
	thread_t thread;

	if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
		uint32_t touch_screen = 0;

		enabled = PE_parse_boot_argn("touchscreen", &touch_screen, sizeof(touch_screen)) && touch_screen != 0;
	}
	if (!enabled) {
		return;
	}
	PE_parse_boot_argn("buttons_steps", &steps, sizeof(steps));
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(buttons_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
