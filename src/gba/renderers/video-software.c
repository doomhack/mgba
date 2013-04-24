#include "video-software.h"

#include "gba.h"
#include "gba-io.h"

#include <stdlib.h>
#include <string.h>

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer);
static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y);
static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer);

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer);
static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value);

static void _composite(struct GBAVideoSoftwareRenderer* renderer, int offset, int entry, struct PixelFlags flags);
static void _drawBackgroundMode0(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y);

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer);
static inline uint16_t _brighten(uint16_t color, int y);
static inline uint16_t _darken(uint16_t color, int y);

static void _sortBackgrounds(struct GBAVideoSoftwareRenderer* renderer);
static int _backgroundComparator(const void* a, const void* b);

void GBAVideoSoftwareRendererCreate(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->d.init = GBAVideoSoftwareRendererInit;
	renderer->d.deinit = GBAVideoSoftwareRendererDeinit;
	renderer->d.writeVideoRegister = GBAVideoSoftwareRendererWriteVideoRegister;
	renderer->d.drawScanline = GBAVideoSoftwareRendererDrawScanline;
	renderer->d.finishFrame = GBAVideoSoftwareRendererFinishFrame;

	renderer->d.turbo = 0;
	renderer->d.framesPending = 0;

	renderer->sortedBg[0] = &renderer->bg[0];
	renderer->sortedBg[1] = &renderer->bg[1];
	renderer->sortedBg[2] = &renderer->bg[2];
	renderer->sortedBg[3] = &renderer->bg[3];

	{
		pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
		renderer->mutex = mutex;
		pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
		renderer->cond = cond;
	}
}

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	int i;

	softwareRenderer->dispcnt.packed = 0x0080;

	softwareRenderer->target1Obj = 0;
	softwareRenderer->target1Bd = 0;
	softwareRenderer->target2Obj = 0;
	softwareRenderer->target2Bd = 0;
	softwareRenderer->blendEffect = BLEND_NONE;
	memset(softwareRenderer->variantPalette, 0, sizeof(softwareRenderer->variantPalette));

	softwareRenderer->bldy = 0;

	for (i = 0; i < 4; ++i) {
		struct GBAVideoSoftwareBackground* bg = &softwareRenderer->bg[i];
		bg->index = i;
		bg->enabled = 0;
		bg->priority = 0;
		bg->charBase = 0;
		bg->mosaic = 0;
		bg->multipalette = 0;
		bg->screenBase = 0;
		bg->overflow = 0;
		bg->size = 0;
		bg->target1 = 0;
		bg->target2 = 0;
		bg->x = 0;
		bg->y = 0;
		bg->refx = 0;
		bg->refy = 0;
		bg->dx = 256;
		bg->dmx = 0;
		bg->dy = 0;
		bg->dmy = 256;
		bg->sx = 0;
		bg->sy = 0;
	}

	pthread_mutex_init(&softwareRenderer->mutex, 0);
	pthread_cond_init(&softwareRenderer->cond, 0);
}

static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	pthread_mutex_destroy(&softwareRenderer->mutex);
	pthread_cond_destroy(&softwareRenderer->cond);
}

static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	switch (address) {
	case REG_DISPCNT:
		value &= 0xFFFB;
		softwareRenderer->dispcnt.packed = value;
		GBAVideoSoftwareRendererUpdateDISPCNT(softwareRenderer);
		break;
	case REG_BG0CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[0], value);
		break;
	case REG_BG1CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[1], value);
		break;
	case REG_BG2CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[2], value);
		break;
	case REG_BG3CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[3], value);
		break;
	case REG_BG0HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].x = value;
		break;
	case REG_BG0VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].y = value;
		break;
	case REG_BG1HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].x = value;
		break;
	case REG_BG1VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].y = value;
		break;
	case REG_BG2HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].x = value;
		break;
	case REG_BG2VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].y = value;
		break;
	case REG_BG3HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].x = value;
		break;
	case REG_BG3VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].y = value;
		break;
	case REG_BLDCNT:
		GBAVideoSoftwareRendererWriteBLDCNT(softwareRenderer, value);
		break;
	case REG_BLDY:
		softwareRenderer->bldy = value & 0x1F;
		if (softwareRenderer->bldy > 0x10) {
			softwareRenderer->bldy = 0x10;
		}
		_updatePalettes(softwareRenderer);
		break;
	default:
		GBALog(GBA_LOG_STUB, "Stub video register write: %03x", address);
	}
	return value;
}

static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	uint16_t* row = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * y];
	if (softwareRenderer->dispcnt.forcedBlank) {
		for (int x = 0; x < VIDEO_HORIZONTAL_PIXELS; ++x) {
			row[x] = 0x7FFF;
		}
		return;
	}

	memset(softwareRenderer->flags, 0, sizeof(softwareRenderer->flags));
	softwareRenderer->row = row;

	for (int i = 0; i < 4; ++i) {
		if (softwareRenderer->sortedBg[i]->enabled) {
			_drawBackgroundMode0(softwareRenderer, softwareRenderer->sortedBg[i], y);
		}
	}
}

static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	pthread_mutex_lock(&softwareRenderer->mutex);
	renderer->framesPending++;
	if (!renderer->turbo) {
		pthread_cond_wait(&softwareRenderer->cond, &softwareRenderer->mutex);
	}
	pthread_mutex_unlock(&softwareRenderer->mutex);
}

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->bg[0].enabled = renderer->dispcnt.bg0Enable;
	renderer->bg[1].enabled = renderer->dispcnt.bg1Enable;
	renderer->bg[2].enabled = renderer->dispcnt.bg2Enable;
	renderer->bg[3].enabled = renderer->dispcnt.bg3Enable;
}

static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	union GBARegisterBGCNT reg = { .packed = value };
	bg->priority = reg.priority;
	bg->charBase = reg.charBase << 14;
	bg->mosaic = reg.mosaic;
	bg->multipalette = reg.multipalette;
	bg->screenBase = reg.screenBase << 11;
	bg->overflow = reg.overflow;
	bg->size = reg.size;

	_sortBackgrounds(renderer);
}

static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value) {
	union {
		struct {
			unsigned target1Bg0 : 1;
			unsigned target1Bg1 : 1;
			unsigned target1Bg2 : 1;
			unsigned target1Bg3 : 1;
			unsigned target1Obj : 1;
			unsigned target1Bd : 1;
			enum BlendEffect effect : 2;
			unsigned target2Bg0 : 1;
			unsigned target2Bg1 : 1;
			unsigned target2Bg2 : 1;
			unsigned target2Bg3 : 1;
			unsigned target2Obj : 1;
			unsigned target2Bd : 1;
		};
		uint16_t packed;
	} bldcnt = { .packed = value };

	enum BlendEffect oldEffect = renderer->blendEffect;

	renderer->bg[0].target1 = bldcnt.target1Bg0;
	renderer->bg[1].target1 = bldcnt.target1Bg1;
	renderer->bg[2].target1 = bldcnt.target1Bg2;
	renderer->bg[3].target1 = bldcnt.target1Bg3;
	renderer->bg[0].target2 = bldcnt.target2Bg0;
	renderer->bg[1].target2 = bldcnt.target2Bg1;
	renderer->bg[2].target2 = bldcnt.target2Bg2;
	renderer->bg[3].target2 = bldcnt.target2Bg3;

	renderer->blendEffect = bldcnt.effect;
	renderer->target1Obj = bldcnt.target1Obj;
	renderer->target1Bd = bldcnt.target1Bd;
	renderer->target2Obj = bldcnt.target2Obj;
	renderer->target2Bd = bldcnt.target2Bd;

	if (oldEffect != renderer->blendEffect) {
		_updatePalettes(renderer);
	}
}

static void _composite(struct GBAVideoSoftwareRenderer* renderer, int offset, int entry, struct PixelFlags flags) {
	if (renderer->blendEffect == BLEND_NONE || !flags.target1) {
		renderer->row[offset] = renderer->d.palette[entry];
	} else if (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN) {
		renderer->row[offset] = renderer->variantPalette[entry];
	}
	renderer->flags[offset].finalized = 1;
}

static void _drawBackgroundMode0(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y) {
	int start = 0;
	int end = VIDEO_HORIZONTAL_PIXELS;
	int inX = start + background->x;
	int inY = y + background->y;
	union GBATextMapData mapData;

	unsigned yBase = inY & 0xF8;
	if (background->size & 2) {
		yBase += inY & 0x100;
	} else if (background->size == 3) {
		yBase += (inY & 0x100) << 1;
	}

	unsigned xBase;

	uint32_t screenBase;
	uint32_t charBase;

	for (int outX = start; outX < end; ++outX) {
		if (renderer->flags[outX].finalized) {
			continue;
		}
		xBase = (outX + inX) & 0xF8;
		if (background->size & 1) {
			xBase += ((outX + inX) & 0x100) << 5;
		}
		screenBase = (background->screenBase >> 1) + (xBase >> 3) + (yBase << 2);
		mapData.packed = renderer->d.vram[screenBase];
		charBase = ((background->charBase + (mapData.tile << 5)) >> 1) + ((inY & 0x7) << 1) + (((outX + inX) >> 2) & 1);
		uint16_t tileData = renderer->d.vram[charBase];
		tileData >>= ((outX + inX) & 0x3) << 2;
		if (tileData & 0xF) {
			struct PixelFlags flags = {
				.finalized = 1,
				.target1 = background->target1,
				.target2 = background->target2
			};
			_composite(renderer, outX, (tileData & 0xF) | (mapData.palette << 4), flags);
		}
	}
}

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer) {
	if (renderer->blendEffect == BLEND_BRIGHTEN) {
		for (int i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _brighten(renderer->d.palette[i], renderer->bldy);
		}
	} else if (renderer->blendEffect == BLEND_DARKEN) {
		for (int i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _darken(renderer->d.palette[i], renderer->bldy);
		}
	}
}

static inline uint16_t _brighten(uint16_t c, int y) {
	union GBAColor color = { .packed = c };
	color.r = color.r + ((31 - color.r) * y) / 16;
	color.g = color.g + ((31 - color.g) * y) / 16;
	color.b = color.b + ((31 - color.b) * y) / 16;
	return color.packed;
}

static inline uint16_t _darken(uint16_t c, int y) {
	union GBAColor color = { .packed = c };
	color.r = color.r - (color.r * y) / 16;
	color.g = color.g - (color.g * y) / 16;
	color.b = color.b - (color.b * y) / 16;
	return color.packed;
}

static void _sortBackgrounds(struct GBAVideoSoftwareRenderer* renderer) {
	qsort(renderer->sortedBg, 4, sizeof(struct GBAVideoSoftwareBackground*), _backgroundComparator);
}

static int _backgroundComparator(const void* a, const void* b) {
	const struct GBAVideoSoftwareBackground* bgA = *(const struct GBAVideoSoftwareBackground**) a;
	const struct GBAVideoSoftwareBackground* bgB = *(const struct GBAVideoSoftwareBackground**) b;
	if (bgA->priority != bgB->priority) {
		return bgA->priority - bgB->priority;
	} else {
		return bgA->index - bgB->index;
	}
}
