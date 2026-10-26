/*
 * The touch screen of the Xiaomi 12 (cupid): an STMicroelectronics FTS controller
 * ("st,spi" in the vendor device tree) on the SPI bus of QUP0's serial engine 4, as a
 * source of touches for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg
 * touchscreen=1: every address below belongs to this phone.
 *
 * There is no clock, pin, regulator or SPI framework in this kernel, so the driver does
 * all of it itself, in the order the vendor's Linux driver (drivers/input/touchscreen/
 * fts_spi) does:
 *   power    the analog supply is LDO7 of the PM8350C (3.2 V), the I/O supply LDO2
 *            (1.8 V), switched through RPMh (requests written into the "always on"
 *            processor's command slots, driver 2)
 *   clocks   the serial engine's three clocks, by vote in the global clock controller
 *            (QUP0 SE4: vote bit 14, M_AHB bit 6, S_AHB bit 7; the RCG at 0x274e4)
 *   pins     four GPIOs to the serial engine, plus reset (GPIO 20) and interrupt
 *            (GPIO 21, low while the chip has reports)
 *   SPI      the serial engine ("GENI") in FIFO mode, polled; or, if the engine cannot
 *            be used, the same four pins driven by hand
 *   chip     reset, wait for the "controller ready" event, read the chip ID through the
 *            FTI register interface (0xFB), then read the event FIFO (0x87) when the
 *            interrupt line signals
 *
 * FTS 5.x SPI framing (fts_lib/ftsIO.c of the vendor driver):
 *   write:  cmd (0xFA), 32-bit big-endian address, data
 *   read:   cmd (0xFB), 32-bit big-endian address, then one dummy byte and the data
 *   FIFO:   cmd (0x87), then one dummy byte and eight bytes per event
 * No interrupt is used: a thread looks at the interrupt line 125 times a second
 * (boot-arg touch_poll_us=N: every N microseconds instead of 8000).
 *
 * Register layouts: Linux drivers/spi/spi-geni-qcom.c, drivers/soc/qcom/{qcom-geni-se,
 * rpmh-rsc,cmd-db}.c, drivers/clk/qcom/gcc-sm8450.c, drivers/pinctrl/qcom/pinctrl-sm8450.c,
 * drivers/input/touchscreen/fts_spi/, and the phone's own device tree.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern void hakios_tl(const char *format, ...) __printflike(1, 2);

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

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

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_UP                 3

/*
 * The vendor device tree's qupv3_se4_spi_active group: gpio16..19, function "qup4".
 * On SM8450 the four are MISO, MOSI, CLK, CS in that order (pinctrl-sm8450.c has
 * PINGROUP(16, qup4, ...) .. PINGROUP(19, qup4, ...)); "qup4" is the second entry of
 * each pin's function list, so the mux value is 1.
 */
#define PIN_MISO                16
#define PIN_MOSI                17
#define PIN_CLK                 18
#define PIN_CS                  19
#define PIN_RESET               20      /* low = in reset */
#define PIN_INT                 21      /* low = the chip has reports */
#define PIN_FUNCTION_SPI        1       /* "qup4" */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the supplies (RPMh) ------------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x17a20000UL    /* the application processor's "DRV 2" (rsc@17a00000) */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("fts: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("fts: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("fts: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/* One regulator: its voltage in millivolts, on, in high-power mode. */
static bool
regulator_on(const char *name, uint32_t millivolts)
{
	uint32_t base = rpmh_address(name);
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { millivolts, 1, VRM_MODE_LDO_HPM };

	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("fts: RPMh: no regulator \"%s\" (address %#x)\n", name, base);
		return false;
	}
	printf("fts: RPMh: %s is at %#x\n", name, base);
	return rpmh_write(address, data, 3);
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x80000UL
#define GCC_VOTE                (gcc + 0x62008)         /* the application processor's votes */
#define VOTE_SE4                (1u << 14)
#define VOTE_M_AHB              (1u << 6)
#define VOTE_S_AHB              (1u << 7)
#define GCC_M_AHB_CBCR          (gcc + 0x27004)         /* bit 31: the clock is off */
#define GCC_S_AHB_CBCR          (gcc + 0x27008)
#define GCC_SE4_CBCR            (gcc + 0x274dc)
#define GCC_SE4_CMD             (gcc + 0x274e4)         /* bit 0: take the new configuration */
#define GCC_SE4_CFG             (gcc + 0x274e8)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_SE4_DFSR            (gcc + 0x274f8)         /* bit 0: the engine chooses among preset rates itself */
#define GCC_SE4_PERF_CFG(l)     (gcc + 0x27500 + 4 * (l))
#define GCC_SE4_PERF_M(l)       (gcc + 0x27540 + 4 * (l))
#define GCC_SE4_PERF_N(l)       (gcc + 0x27580 + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(uint32_t bus_hz)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_M_AHB | VOTE_S_AHB);
	if (rd(GCC_SE4_DFSR) & 1) {
		/*
		 * Preset rates. Pick the one that gives the fastest bus that stays within what
		 * the chip accepts (24 MHz / 2 = 12 MHz here).
		 */
		uint32_t best = 0;

		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_SE4_PERF_CFG(level)), rd(GCC_SE4_PERF_M(level)),
			    rd(GCC_SE4_PERF_N(level)));
			uint32_t divider = (hz + bus_hz - 1) / bus_hz;
			uint32_t bus = hz / (divider ? divider : 1);

			if (bus <= bus_hz && bus > best) {
				best = bus;
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("fts: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_SE4_CFG, (rd(GCC_SE4_CFG) & ~0x371fu) | 1);
		wr(GCC_SE4_CMD, rd(GCC_SE4_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_SE4_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_SE4_CMD) & 1) {
			printf("fts: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_SE4_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_SE4);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_M_AHB_CBCR) | rd(GCC_S_AHB_CBCR) | rd(GCC_SE4_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("fts: clocks stay off: %#x %#x %#x\n", rd(GCC_M_AHB_CBCR), rd(GCC_S_AHB_CBCR), rd(GCC_SE4_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_PHYS                 0x00990000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

/* 0 = ready; otherwise why the engine cannot be used */
static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, most significant first, four bytes in a FIFO word */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out (zeros without tx) and as many in. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("fts: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* ---- SPI by hand, on the same pins ----------------------------------------------------------------- */

static unsigned bitbang_in = PIN_MISO, bitbang_out = PIN_MOSI;

static void
bitbang_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(bitbang_out, false, PULL_NONE, 6);
	gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length);

/* ---- the FTS chip --------------------------------------------------------------------------------- */

#define FTS_CMD_HW_REG_R        0xfb    /* FTI register read, SPI */
#define FTS_CMD_HW_REG_W        0xfa    /* FTI register write, SPI */
#define FTS_CMD_FIFO_READ       0x87    /* read all events in the FIFO, SPI */
#define FTS_DUMMY               1       /* one dummy byte precedes every read */

#define FTS_ADDR_CHIP_ID        0x20000000UL
#define FTS_ADDR_ICR            0x2000002dUL
#define FTS_ADDR_GPIO_DIRECTION 0x20000032UL
#define FTS_ADDR_GPIO_PULLUP    0x20000034UL
#define FTS_ADDR_GPIO_CFG0      0x2000003dUL
#define FTS_ADDR_GPIO_CFG2      0x2000003fUL

#define FTS_CHIP_ID_0           0x36
#define FTS_EVENT_SIZE          8
#define FTS_FIFO_DEPTH          32
#define FTS_MAX_TOUCHES         10
#define FTS_MAX_READ            (FTS_EVENT_SIZE * FTS_FIFO_DEPTH)

#define FTS_EVT_CONTROLLER_READY 0x03
#define FTS_EVT_ENTER           0x13
#define FTS_EVT_MOTION          0x23
#define FTS_EVT_LEAVE           0x33
#define FTS_EVT_STATUS          0x43
#define FTS_EVT_USER            0x53
#define FTS_EVT_ERROR           0xf3

#define FTS_TOUCH_FINGER        0x01
#define FTS_TOUCH_GLOVE         0x02
#define FTS_TOUCH_PALM          0x04

#define FTS_TIMEOUT_MS          100     /* GENERAL_TIMEOUT in the vendor driver */
#define FTS_RESET_RETRIES       3

/* The panel's extent, from the device tree (fts,x-max / fts,y-max). */
static uint32_t max_x = 10800, max_y = 24000;
static bool super_resolution = true;

static uint8_t spi_tx[8 + FTS_MAX_READ], spi_rx[8 + FTS_MAX_READ];

/* cmd, four address bytes, then the data: one transfer, the chip selected throughout. */
static bool
fts_hw_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	uint32_t total = 5 + length;

	if (total > sizeof(spi_tx)) {
		return false;
	}
	spi_tx[0] = FTS_CMD_HW_REG_W;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	memcpy(spi_tx + 5, data, length);
	return spi_transfer(spi_tx, NULL, total);
}

/*
 * A read is two transfers with the chip unselected in between, as the vendor's fts_writeRead()
 * does (two spi_transfers, SPI_DELAY_CS between them): the command, then the answer, the first
 * byte of which is a dummy.
 */
static bool
fts_read_phases(const uint8_t *command, uint32_t command_length, uint8_t *data, uint32_t length)
{
	uint32_t total = FTS_DUMMY + length;

	if (total > sizeof(spi_rx)) {
		return false;
	}
	if (!spi_transfer(command, NULL, command_length)) {
		return false;
	}
	sleep_us(10);           /* SPI_DELAY_CS */
	if (!spi_transfer(NULL, spi_rx, total)) {
		return false;
	}
	memcpy(data, spi_rx + FTS_DUMMY, length);
	return true;
}

/* cmd, four address bytes, then the dummy byte and the data. */
static bool
fts_hw_read(uint32_t address, uint8_t *data, uint32_t length)
{
	uint8_t command[5];

	command[0] = FTS_CMD_HW_REG_R;
	command[1] = (uint8_t)(address >> 24);
	command[2] = (uint8_t)(address >> 16);
	command[3] = (uint8_t)(address >> 8);
	command[4] = (uint8_t)address;
	return fts_read_phases(command, sizeof(command), data, length);
}

/* The FIFO: cmd, then the dummy byte and `length` bytes of events. */
static bool
fts_fifo_read(uint8_t *data, uint32_t length)
{
	uint8_t command[1] = { FTS_CMD_FIFO_READ };

	return fts_read_phases(command, sizeof(command), data, length);
}

static bool
fts_chip_id(uint8_t *high, uint8_t *low)
{
	uint8_t id[2];

	if (!fts_hw_read(FTS_ADDR_CHIP_ID, id, sizeof(id))) {
		return false;
	}
	*high = id[0];
	*low = id[1];
	return id[0] == FTS_CHIP_ID_0;
}

static void
touch_reset(void)
{
	gpio_set(PIN_RESET, false);
	sleep_ms(10);
	gpio_set(PIN_RESET, true);
	sleep_ms(10);
}

/* Read one event from the FIFO; 0x00 when there is none. */
static bool
fts_wait_ready(void)
{
	uint8_t event[FTS_EVENT_SIZE];

	for (int i = 0; i < FTS_TIMEOUT_MS / 2; i++) {
		if (!fts_fifo_read(event, sizeof(event))) {
			return false;
		}
		if ((event[0] >> 4) == 0 && event[0] == FTS_EVT_CONTROLLER_READY) {
			return true;
		}
		if (event[0] != 0) {
			printf("fts: event %02x %02x %02x %02x %02x %02x %02x %02x while waiting for ready\n",
			    event[0], event[1], event[2], event[3], event[4], event[5], event[6], event[7]);
		}
		sleep_ms(2);
	}
	return false;
}

/* The chip defaults to the 3-wire protocol while no firmware runs; this asks for 4 wires. */
static bool
fts_spi4_mode(void)
{
	uint8_t value;

	value = 0x10;
	if (!fts_hw_write(FTS_ADDR_GPIO_DIRECTION, &value, 1)) {
		return false;
	}
	value = 0x02;
	if (!fts_hw_write(FTS_ADDR_GPIO_PULLUP, &value, 1)) {
		return false;
	}
	value = 0x07;
	if (!fts_hw_write(FTS_ADDR_GPIO_CFG2, &value, 1)) {
		return false;
	}
	value = 0x30;
	if (!fts_hw_write(FTS_ADDR_GPIO_CFG0, &value, 1)) {
		return false;
	}
	value = 0x02;
	return fts_hw_write(FTS_ADDR_ICR, &value, 1);
}

/* Is the chip there and running its firmware? */
static bool
fts_probe(bool verbose)
{
	uint8_t high = 0, low = 0;

	for (int attempt = 0; attempt < FTS_RESET_RETRIES; attempt++) {
		touch_reset();
		if (fts_wait_ready()) {
			printf("fts: controller ready after reset %d\n", attempt);
			break;
		}
		printf("fts: no controller-ready event after reset %d\n", attempt);
	}

	if (fts_chip_id(&high, &low)) {
		printf("fts: chip ID %02x %02x\n", high, low);
		return true;
	}
	if (verbose) {
		printf("fts: no chip ID (read %02x %02x); asking for 4-wire mode\n", high, low);
	}
	if (fts_spi4_mode() && fts_chip_id(&high, &low)) {
		printf("fts: chip ID %02x %02x after 4-wire mode\n", high, low);
		return true;
	}
	if (verbose) {
		printf("fts: no chip ID (read %02x %02x)\n", high, low);
	}
	return false;
}

/* What has been seen so far: said once a minute, so that the end of the log always tells. */
static struct {
	uint32_t polls, reports, events, touches, bad, last_x, last_y;
} seen;

/* Which fingers were down in the last report, and where. */
static struct {
	bool down;
	uint32_t x, y;
} contact[FTS_MAX_TOUCHES];
static uint32_t flip;           /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */

static uint32_t
event_x(const uint8_t *event)
{
	if (super_resolution) {
		return event[3] << 8 | event[2];
	}
	return (event[3] & 0x0f) << 8 | event[2];
}

static uint32_t
event_y(const uint8_t *event)
{
	if (super_resolution) {
		return event[5] << 8 | event[4];
	}
	return event[4] << 4 | (event[3] & 0xf0) >> 4;
}

static void
fts_touch_event(const uint8_t *event)
{
	static unsigned shown;
	uint32_t kind = event[0] >> 4, id = event[1] >> 4, type = event[1] & 0xf;

	if (id >= FTS_MAX_TOUCHES) {
		return;
	}
	if (kind == FTS_EVT_LEAVE >> 4) {
		if (contact[id].down) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
		return;
	}
	if (kind != FTS_EVT_ENTER >> 4 && kind != FTS_EVT_MOTION >> 4) {
		return;
	}
	if (type != FTS_TOUCH_FINGER && type != FTS_TOUCH_GLOVE && type != FTS_TOUCH_PALM) {
		return;
	}
	uint32_t x = event_x(event), y = event_y(event);

	if (x > max_x) {
		x = max_x;
	}
	if (y > max_y) {
		y = max_y;
	}
	x = (uint32_t)((uint64_t)x * 65535 / max_x);
	y = (uint32_t)((uint64_t)y * 65535 / max_y);
	if (flip & 1) {
		x = 65535 - x;
	}
	if (flip & 2) {
		y = 65535 - y;
	}
	seen.touches++;
	seen.last_x = x;
	seen.last_y = y;
	if (shown < 40) {
		shown++;
		printf("fts: finger %u at %u, %u (of %u x %u)\n", id, x, y, max_x, max_y);
	}
	if (!contact[id].down) {
		contact[id].down = true;
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
	} else if (contact[id].x != x || contact[id].y != y) {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
	}
	contact[id].x = x;
	contact[id].y = y;
}

static void
fts_event(const uint8_t *event)
{
	static unsigned shown;

	if (event[0] == 0) {
		return;
	}
	seen.events++;
	switch (event[0] >> 4) {
	case FTS_EVT_ENTER >> 4:
	case FTS_EVT_MOTION >> 4:
	case FTS_EVT_LEAVE >> 4:
		fts_touch_event(event);
		break;
	case FTS_EVT_CONTROLLER_READY >> 4:
		printf("fts: controller ready\n");
		break;
	case FTS_EVT_STATUS >> 4:
		if (event[1] != 0x01 && shown++ < 8) {           /* 0x01 is the echo of our own command */
			printf("fts: status event %02x %02x %02x %02x %02x %02x %02x %02x\n", event[0], event[1], event[2],
			    event[3], event[4], event[5], event[6], event[7]);
		}
		break;
	case FTS_EVT_USER >> 4:
		if (shown++ < 8) {
			printf("fts: user event %02x %02x %02x %02x %02x %02x %02x %02x\n", event[0], event[1], event[2],
			    event[3], event[4], event[5], event[6], event[7]);
		}
		break;
	case FTS_EVT_ERROR >> 4:
		seen.bad++;
		if (shown++ < 8) {
			printf("fts: error event %02x %02x %02x %02x %02x %02x %02x %02x\n", event[0], event[1], event[2],
			    event[3], event[4], event[5], event[6], event[7]);
		}
		break;
	default:
		break;
	}
}

/* Read all the events waiting in the FIFO and hand them on. */
static void
fts_poll(void)
{
	static uint8_t events[FTS_MAX_READ];
	uint32_t count;

	seen.polls++;
	if (!fts_fifo_read(events, FTS_EVENT_SIZE)) {
		return;
	}
	seen.reports++;
	count = events[7] & 0x1f;
	if (count > FTS_FIFO_DEPTH - 1) {
		count = FTS_FIFO_DEPTH - 1;
	}
	if (count > 0 && !fts_fifo_read(events + FTS_EVENT_SIZE, count * FTS_EVENT_SIZE)) {
		return;
	}
	for (uint32_t i = 0; i <= count; i++) {
		fts_event(events + i * FTS_EVENT_SIZE);
	}
}

/* ---- bring-up ------------------------------------------------------------------------------------- */

static void
use_engine_pins(void)
{
	gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	/* the vendor's mi_ts_cs_active: 16 mA, pulled up */
	gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_UP, 16, false);
}

static void
power_on(void)
{
	gpio_set(PIN_RESET, false);
	gpio_configure(PIN_RESET, 0, PULL_UP, 8, true);
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);          /* the vendor's mi_ts_active */
	if (!regulator_on("ldoc2", 1800)) {             /* the I/O supply, "vdd" */
		printf("fts: no I/O supply\n");
	}
	sleep_ms(1);
	if (!regulator_on("ldoc7", 3200)) {             /* the analog supply, "avdd" */
		printf("fts: no analog supply\n");
	}
	sleep_ms(5);
	gpio_set(PIN_RESET, true);
	sleep_ms(10);
}

static bool
bring_up(void)
{
	const char *problem = "its clocks cannot be turned on";
	uint32_t spi_hz = 12000000;

	power_on();
	/* boot-arg touch_spi_hz=N: the bus at N Hz (the device tree gives the chip 12 MHz) */
	PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
	if (spi_hz < 100000 || spi_hz > 12000000) {
		spi_hz = 12000000;
	}
	if (clocks_on(spi_hz)) {
		printf("fts: serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
		    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
		printf("fts: the bus at %u Hz\n", spi_hz);
		problem = geni_spi_init(spi_hz);
	}
	if (problem == NULL) {
		use_engine_pins();
		spi_transfer = geni_transfer;
		if (fts_probe(true)) {
			return true;
		}
		printf("fts: no answer through the serial engine; trying the pins by hand\n");
	} else {
		printf("fts: %s; driving the pins by hand\n", problem);
	}

	/* by hand; which of the two data pins is the input is not written down anywhere, so try both */
	spi_transfer = bitbang_transfer;
	for (int attempt = 0; attempt < 2; attempt++) {
		bitbang_in = attempt == 0 ? PIN_MISO : PIN_MOSI;
		bitbang_out = attempt == 0 ? PIN_MOSI : PIN_MISO;
		bitbang_init();
		if (fts_probe(true)) {
			printf("fts: answers on pins driven by hand (data in on gpio%u)\n", bitbang_in);
			return true;
		}
	}
	return false;
}

static uint64_t
micros_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
fts_thread(__unused void *parameter, __unused wait_result_t result)
{
	unsigned idle = 0, idle_looks;
	uint32_t poll_us = 8000, latency = 0, reads = 0;
	uint64_t before = 0, reading = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_lat", &latency, sizeof(latency));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	idle_looks = 80000 / poll_us;   /* a report read every 80 ms even with the line high, in case it is missed */

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */

	if (!bring_up()) {
		printf("fts: no touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("fts: touch screen ready; the interrupt line is looked at every %u us\n", poll_us);
	for (unsigned tick = 1;; tick++) {
		uint64_t looked = micros_now(), read_at;

		/* the interrupt line is low while a report waits; look anyway every 80 ms in case it is missed */
		if (!gpio_get(PIN_INT)) {
			idle = 0;
			fts_poll();
			read_at = micros_now();
			reads++;
			reading += read_at - looked;
			if (latency) {
				hakios_tl("tl: c before %llu seen %llu read %llu\n", before, looked, read_at);
			}
		} else if (++idle >= idle_looks) {
			idle = 0;
			fts_poll();
			reads++;
			reading += micros_now() - looked;
		}
		before = looked;
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("fts: so far %u reports in %u looks (%u bad), %u events, %u finger positions, the last at %u, %u; "
			    "interrupt line %s; a report takes %llu us to read; the line looked at every %u us\n",
			    seen.reports, seen.polls, seen.bad, seen.events, seen.touches, seen.last_x, seen.last_y,
			    gpio_get(PIN_INT) ? "high" : "low", reads ? reading / reads : 0, poll_us);
		}
		sleep_us(poll_us);
	}
}

void
hakios_fts_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (kernel_thread_start(fts_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
