#include <SPI.h>
#include "petsprite.h"
#include "ui_assets.h"

#define EPD_CS_PIN   PC4
#define EPD_DC_PIN   PC3
#define EPD_RST_PIN  PD0
#define EPD_BUSY_PIN PD2

#define DISP_W        250
#define DISP_H        128
#define DISP_STRIDE   (DISP_H / 8)
#define DISP_GATE_MAX (DISP_W - 1)

#define BULK_SPI         1

#define WINDOWED_REFRESH 1
#define ANIM_XA          100
#define ANIM_XB          237
#define FRAME_DELAY_MS   12
#define GHOST_EVERY      150
#define HOP_LEN          12
#define JUMP_PEAK        2

#define QR_SC   3
#define QR_X    8
#define QR_Y    42
#define TXT_X   3
#define TXT_Y   18
#define TITLE_X 123
#define TITLE_Y 1

#define CELL_LX 102
#define CELL_RX 183
#define CELL_TY 14
#define CELL_BY 69

static const int CELL_X[4]      = { CELL_LX, CELL_RX, CELL_LX, CELL_RX };
static const int CELL_BASE_Y[4] = { CELL_TY, CELL_TY, CELL_BY, CELL_BY };
static const uint8_t *g_cell_spr[4];
static int           g_cell_y[4];

static void epd_writeCommand(uint8_t command) {
	digitalWrite(EPD_DC_PIN, LOW);
	delayMicroseconds(1);
	digitalWrite(EPD_CS_PIN, LOW);
	SPI.transfer(command);
	digitalWrite(EPD_CS_PIN, HIGH);
}

static void epd_writeData(uint8_t data) {
	digitalWrite(EPD_DC_PIN, HIGH);
	delayMicroseconds(1);
	digitalWrite(EPD_CS_PIN, LOW);
	SPI.transfer(data);
	digitalWrite(EPD_CS_PIN, HIGH);
}

static void epd_waitUntilIdle(void) {
	delay(2);
	unsigned long start = millis();
	while (digitalRead(EPD_BUSY_PIN) != LOW) {
		if (millis() - start > 5000)
			break;
	}
}

static void epd_HWreset(void) {
	delay(50);
	digitalWrite(EPD_RST_PIN, LOW);
	delay(50);
	digitalWrite(EPD_RST_PIN, HIGH);
	delay(50);
}

static void epd_set_window(int xb_start, int xb_end, int g_start, int g_end) {
	epd_writeCommand(0x44);
	epd_writeData(xb_start & 0xFF);
	epd_writeData(xb_end & 0xFF);

	epd_writeCommand(0x45);
	epd_writeData(g_start & 0xFF);
	epd_writeData((g_start >> 8) & 0xFF);
	epd_writeData(g_end & 0xFF);
	epd_writeData((g_end >> 8) & 0xFF);
}

static void epd_set_cursor(int xb, int g) {
	epd_writeCommand(0x4E);
	epd_writeData(xb & 0xFF);

	epd_writeCommand(0x4F);
	epd_writeData(g & 0xFF);
	epd_writeData((g >> 8) & 0xFF);
}

static void epd_set_full_window(void) {
	epd_set_window(0x00, DISP_STRIDE - 1, DISP_GATE_MAX, 0x00);
}

static void epd_set_full_cursor(void) {
	epd_set_cursor(0x00, DISP_GATE_MAX);
}

static int sprite_bit(const uint8_t *spr, int col, int row) {
	return (spr[row * PET_STRIDE + (col >> 3)] >> (7 - (col & 7))) & 1;
}

static int blit_bit(const uint8_t *bits, int stride, int w, int h,
		    int lx, int ly, int sc, int x, int y) {
	int px = x - lx, py = y - ly;
	if (px < 0 || py < 0)
		return 0;
	int i = px / sc, j = py / sc;
	if (i >= w || j >= h)
		return 0;
	return (bits[j * stride + (i >> 3)] >> (7 - (i & 7))) & 1;
}

static int content_black(int x, int y) {
	for (int i = 0; i < 4; i++) {
		int dx = x - CELL_X[i], dy = y - g_cell_y[i];
		if (dx >= 0 && dx < PET_W && dy >= 0 && dy < PET_H &&
		    sprite_bit(g_cell_spr[i], dx, dy))
			return 1;
	}
	return blit_bit(qr_bits,  QR_STRIDE,  QR_N,  QR_N,  QR_X,    QR_Y,    QR_SC, x, y) ||
	       blit_bit(txt_bits, TXT_STRIDE, TXT_W, TXT_H, TXT_X,   TXT_Y,   1,     x, y) ||
	       blit_bit(tt_bits,  TT_STRIDE,  TT_W,  TT_H,  TITLE_X, TITLE_Y, 1,     x, y);
}

static int gate_active(int gate) {
	return (gate >= QR_X    && gate < QR_X    + QR_N * QR_SC) ||
	       (gate >= TXT_X    && gate < TXT_X   + TXT_W)        ||
	       (gate >= TITLE_X  && gate < TITLE_X + TT_W)         ||
	       (gate >= CELL_LX  && gate < CELL_LX + PET_W)        ||
	       (gate >= CELL_RX  && gate < CELL_RX + PET_W);
}

static void epd_stream_range(int xa, int xb) {
#if BULK_SPI
	digitalWrite(EPD_DC_PIN, HIGH);
	delayMicroseconds(1);
	digitalWrite(EPD_CS_PIN, LOW);
#endif
	for (int gate = xa; gate <= xb; gate++) {
		int active = gate_active(gate);
		for (int b = 0; b < DISP_STRIDE; b++) {
			uint8_t out = 0xFF;
			if (active)
				for (int k = 0; k < 8; k++)
					if (content_black(gate, b * 8 + k))
						out &= ~(0x80 >> k);
#if BULK_SPI
			SPI.transfer(out);
#else
			epd_writeData(out);
#endif
		}
	}
#if BULK_SPI
	digitalWrite(EPD_CS_PIN, HIGH);
#endif
}

static void epd_reg_init(void) {
	epd_waitUntilIdle();
	epd_writeCommand(0x12);
	epd_waitUntilIdle();

	epd_writeCommand(0x01);
	epd_writeData(0xF9);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x11);
	epd_writeData(0x01);

	epd_writeCommand(0x44);
	epd_writeData(0x00);
	epd_writeData(0x0F);

	epd_writeCommand(0x45);
	epd_writeData(0xF9);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x3C);
	epd_writeData(0x05);

	epd_writeCommand(0x21);
	epd_writeData(0x00);
	epd_writeData(0x80);

	epd_writeCommand(0x18);
	epd_writeData(0x80);

	epd_writeCommand(0x4E);
	epd_writeData(0x00);
	epd_writeCommand(0x4F);
	epd_writeData(0xF9);
	epd_writeData(0x00);

	epd_waitUntilIdle();
}

static void epd_write_base(void) {
	epd_set_full_window();

	epd_set_full_cursor();
	epd_writeCommand(0x24);
	epd_stream_range(0, DISP_W - 1);

	epd_set_full_cursor();
	epd_writeCommand(0x26);
	epd_stream_range(0, DISP_W - 1);

	epd_writeCommand(0x22);
	epd_writeData(0xF7);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();
}

static const uint8_t WF_PARTIAL[159] = {
	0x0, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x80, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x40, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x14, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
	0x22, 0x17, 0x41, 0x0, 0x32, 0x36,
};

static const int g_phase0_tp = 0x10;
static const int g_fr = 3;

static void epd_load_partial_lut(void) {
	uint8_t fr_byte = ((g_fr & 7) << 4) | (g_fr & 7);
	epd_writeCommand(0x32);
	for (int i = 0; i < 153; i++) {
		uint8_t b = WF_PARTIAL[i];
		if (i == 60)
			b = (uint8_t)g_phase0_tp;
		else if (i >= 144 && i <= 149)
			b = fr_byte;
		epd_writeData(b);
	}
	epd_waitUntilIdle();

	epd_writeCommand(0x3F);
	epd_writeData(WF_PARTIAL[153]);
	epd_writeCommand(0x03);
	epd_writeData(WF_PARTIAL[154]);
	epd_writeCommand(0x04);
	epd_writeData(WF_PARTIAL[155]);
	epd_writeData(WF_PARTIAL[156]);
	epd_writeData(WF_PARTIAL[157]);
	epd_writeCommand(0x2C);
	epd_writeData(WF_PARTIAL[158]);

	epd_writeCommand(0x37);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x40);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x3C);
	epd_writeData(0x80);
}

static void epd_partial_regs(void) {
	epd_writeCommand(0x01);
	epd_writeData(0xF9);
	epd_writeData(0x00);
	epd_writeData(0x00);

	epd_writeCommand(0x11);
	epd_writeData(0x01);

	epd_writeCommand(0x21);
	epd_writeData(0x00);
	epd_writeData(0x80);

	epd_writeCommand(0x18);
	epd_writeData(0x80);
}

#define DISPLAY_PART_KEEP_ON 0x0C

static void epd_partial_begin(void) {
	digitalWrite(EPD_RST_PIN, LOW);
	delay(2);
	digitalWrite(EPD_RST_PIN, HIGH);
	delay(2);

	epd_partial_regs();
	epd_load_partial_lut();

	epd_writeCommand(0x22);
	epd_writeData(0xC0);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();

	epd_set_full_window();
}

static void epd_set_anim_window(void) {
	int ya = DISP_GATE_MAX - ANIM_XA;
	int yb = DISP_GATE_MAX - ANIM_XB;
	epd_set_window(0x00, DISP_STRIDE - 1, ya, yb);
	epd_set_cursor(0x00, ya);
}

static void epd_partial_frame(void) {
#if WINDOWED_REFRESH
	epd_set_anim_window();
	epd_writeCommand(0x24);
	epd_stream_range(ANIM_XA, ANIM_XB);
#else
	epd_set_full_cursor();
	epd_writeCommand(0x24);
	epd_stream_range(0, DISP_W - 1);
#endif

	epd_writeCommand(0x22);
	epd_writeData(DISPLAY_PART_KEEP_ON);
	epd_writeCommand(0x20);
	epd_waitUntilIdle();

#if WINDOWED_REFRESH
	epd_set_full_window();
#endif
}

static int jump_dy(int p, int span) {
	if (span < 1)
		return 0;
	return -(JUMP_PEAK * 4 * p * (span - p)) / (span * span);
}

static void update_cells(int frame) {
	int hp = frame % HOP_LEN;
	int tp = frame % 12;

	if (hp == 0 || hp == 8) {
		g_cell_spr[0] = pet_crouch; g_cell_y[0] = CELL_BASE_Y[0];
	} else if (hp < 8) {
		g_cell_spr[0] = pet_jump;   g_cell_y[0] = CELL_BASE_Y[0] + jump_dy(hp - 1, 7);
	} else {
		g_cell_spr[0] = pet_idle;   g_cell_y[0] = CELL_BASE_Y[0];
	}

	g_cell_spr[1] = (frame % 12 < 8) ? pet_happy : pet_idle;
	g_cell_y[1]   = CELL_BASE_Y[1];

	if (tp < 2)      g_cell_spr[2] = pet_look_half;
	else if (tp < 6) g_cell_spr[2] = pet_look;
	else if (tp < 8) g_cell_spr[2] = pet_look_half;
	else             g_cell_spr[2] = pet_idle;
	g_cell_y[2]   = CELL_BASE_Y[2];

	g_cell_spr[3] = (frame % 4 < 2) ? pet_eat : pet_idle;
	g_cell_y[3]   = CELL_BASE_Y[3];
}

static void epd_init(void) {
	epd_HWreset();
	delay(1000);
	epd_reg_init();
}

void setup(void) {
	pinMode(EPD_CS_PIN, OUTPUT);
	pinMode(EPD_DC_PIN, OUTPUT);
	pinMode(EPD_RST_PIN, OUTPUT);
	pinMode(EPD_BUSY_PIN, INPUT);
	digitalWrite(EPD_CS_PIN, HIGH);
	digitalWrite(EPD_RST_PIN, LOW);

	SPI.begin();
	SPI.beginTransaction(SPISettings(5000000, MSBFIRST, SPI_MODE0));

	epd_init();

	update_cells(0);
	epd_write_base();
	delay(500);
	epd_partial_begin();
}

void loop(void) {
	static int frame = 0;

	frame++;
	update_cells(frame);
	epd_partial_frame();

	if (frame % GHOST_EVERY == 0) {
		epd_HWreset();
		epd_reg_init();
		epd_write_base();
		epd_partial_begin();
	}

	delay(FRAME_DELAY_MS);
}
