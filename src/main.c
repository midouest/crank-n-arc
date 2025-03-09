#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "pd_api.h"

PlaydateAPI *pd_;

const unsigned int bayer[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21}};

void setPatternAlpha(LCDPattern *pattern, float alpha)
{
    const unsigned int threshold = (unsigned int)((1.0f - alpha) * 64.0f);
    for (int row = 0; row < 8; ++row)
    {
        for (int col = 0; col < 8; ++col)
        {
            if (bayer[row][col] >= threshold)
            {
                (*pattern)[row] |= (1 << col);
            }
            else
            {
                (*pattern)[row] &= ~(1 << col);
            }
            (*pattern)[8 + row] |= (1 << col);
        }
    }
}

const unsigned int black_pattern[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void setBlackPattern(LCDPattern *pattern) { memcpy(*pattern, black_pattern, sizeof(black_pattern)); }

LCDPattern level_patterns[16];

#define N_RINGS 4
#define N_LEDS 64
#define RING_RADIUS 65
const uint16_t ring_diameter = 2 * RING_RADIUS;
#define LED_PADDING 0.5l
#define LED_LENGTH 15

#define RING1_X 68
#define RING1_Y 172
#define RING2_X 156
#define RING2_Y 68
#define RING3_X 244
#define RING3_Y 172
#define RING4_X 332
#define RING4_Y 68

#define SELECT_LEVEL 2

int ring_led_quads[N_RINGS][N_LEDS][8];

typedef struct _ring
{
    uint16_t x;
    uint16_t y;
    uint16_t anchor_x;
    uint16_t anchor_y;
    bool selected;
    bool pressed;
    bool dirty;
    uint32_t dirty_leds_lo;
    uint32_t dirty_leds_hi;
    uint8_t leds[N_LEDS];
} ring_t;

ring_t rings[N_RINGS] = {
    {.x = RING1_X,
     .y = RING1_Y,
     .anchor_x = RING1_X - RING_RADIUS,
     .anchor_y = RING1_Y - RING_RADIUS,
     .dirty = true,
     .selected = true,
     .pressed = false,
     .dirty_leds_lo = 0xffffffff,
     .dirty_leds_hi = 0xffffffff},
    {.x = RING2_X,
     .y = RING2_Y,
     .anchor_x = RING2_X - RING_RADIUS,
     .anchor_y = RING2_Y - RING_RADIUS,
     .dirty = true,
     .selected = false,
     .pressed = false,
     .dirty_leds_lo = 0xffffffff,
     .dirty_leds_hi = 0xffffffff},
    {.x = RING3_X,
     .y = RING3_Y,
     .anchor_x = RING3_X - RING_RADIUS,
     .anchor_y = RING3_Y - RING_RADIUS,
     .dirty = true,
     .selected = false,
     .pressed = false,
     .dirty_leds_lo = 0xffffffff,
     .dirty_leds_hi = 0xffffffff},
    {.x = RING4_X,
     .y = RING4_Y,
     .anchor_x = RING4_X - RING_RADIUS,
     .anchor_y = RING4_Y - RING_RADIUS,
     .dirty = true,
     .selected = false,
     .pressed = false,
     .dirty_leds_lo = 0xffffffff,
     .dirty_leds_hi = 0xffffffff}};

bool multi_select = false;
uint8_t last_select = 0;
bool enable_arc_mod = false;

static void init(PlaydateAPI *pd);
static int update(void *userdata);
static void serial(const char *data);
static void sendModEnabled(PlaydateAPI *pd, bool enabled);
static void sendEncDelta(PlaydateAPI *pd, uint8_t n, float delta);
static void sendKeyPress(PlaydateAPI *pd, uint8_t n, bool s);

#ifdef _WINDLL
__declspec(dllexport)
#endif
int
eventHandler(PlaydateAPI *pd, PDSystemEvent event, uint32_t arg)
{
    (void)arg; // arg is currently only used for event = kEventKeyPressed

    if (event == kEventInit)
    {
        // Note: If you set an update callback in the kEventInit handler, the system assumes the game is pure C and doesn't run any Lua code in the game
        init(pd);
        sendModEnabled(pd, true);
    }
    else if (event == kEventTerminate)
    {
        sendModEnabled(pd, false);
    }
    else if (event == kEventLock)
    {
        sendModEnabled(pd, false);
    }
    else if (event == kEventUnlock)
    {
        sendModEnabled(pd, true);
    }

    return 0;
}

static void sendModEnabled(PlaydateAPI *pd, bool enabled)
{
    pd->system->logToConsole("arc: mod %d", enabled);
}

static void sendEncDelta(PlaydateAPI *pd, uint8_t n, float delta)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdouble-promotion"
    pd->system->logToConsole("arc: enc %d %f", n, delta);
#pragma GCC diagnostic pop
}

static void sendKeyPress(PlaydateAPI *pd, uint8_t n, bool s)
{
    pd->system->logToConsole("arc: key %d %d", n, s);
}

static void init(PlaydateAPI *pd)
{
    pd->system->setUpdateCallback(update, pd);
    pd->system->setSerialMessageCallback(serial);
    pd->display->setRefreshRate(50);

    for (int i = 0; i < 16; i++)
    {
        setBlackPattern(&level_patterns[i]);
        setPatternAlpha(&level_patterns[i], 1.0f - i / 15.0f);
    }

    const double led_angle = 2 * M_PI / N_LEDS;
    const double led_half_angle = led_angle / 2;
    const double led_padding_radius = LED_PADDING / sin(led_half_angle);
    const double led_triangle_height = RING_RADIUS - led_padding_radius;
    const double led_triangle_half_base = led_triangle_height * tan(led_half_angle);
    const double led_inner_triangle_height = led_triangle_height - LED_LENGTH;
    const double led_inner_triangle_half_base = led_inner_triangle_height * tan(led_half_angle);

    for (int i = 0; i < N_RINGS; i++)
    {
        const ring_t *ring = &rings[i];
        const uint16_t x0 = ring->x;
        const uint16_t y0 = ring->y;

        double angle = 0.0l;
        for (int j = 0; j < N_LEDS; j++)
        {
            const double a = cos(angle);
            const double b = sin(angle);

            rings[i].leds[j] = 0;

            ring_led_quads[i][j][0] = round(-led_triangle_half_base * a - (-led_padding_radius - led_triangle_height) * b + x0);
            ring_led_quads[i][j][1] = round(-led_triangle_half_base * b + (-led_padding_radius - led_triangle_height) * a + y0);
            ring_led_quads[i][j][2] = round(led_triangle_half_base * a - (-led_padding_radius - led_triangle_height) * b + x0);
            ring_led_quads[i][j][3] = round(led_triangle_half_base * b + (-led_padding_radius - led_triangle_height) * a + y0);
            ring_led_quads[i][j][4] = round(led_inner_triangle_half_base * a - (-led_padding_radius - led_inner_triangle_height) * b + x0);
            ring_led_quads[i][j][5] = round(led_inner_triangle_half_base * b + (-led_padding_radius - led_inner_triangle_height) * a + y0);
            ring_led_quads[i][j][6] = round(-led_inner_triangle_half_base * a - (-led_padding_radius - led_inner_triangle_height) * b + x0);
            ring_led_quads[i][j][7] = round(-led_inner_triangle_half_base * b + (-led_padding_radius - led_inner_triangle_height) * a + y0);

            angle += led_angle;
        }
    }
}

/**
 * Examples:
 * !msg arc: map 0 0000000000000000000000000000000000000000000000000000000000000000
 * !msg arc: map 0 0123456789:;<=>?0123456789:;<=>?0123456789:;<=>?0123456789:;<=>?
 * !msg arc: map 0 ????????????????????????????????????????????????????????????????
 */
void handleMapMessage(const char *data)
{
    int index = (data[9] - 48) & 0x3;
    ring_t *ring = &rings[index];
    for (int i = 0, j = 11; i < N_LEDS; i++, j++)
    {
        uint8_t old_level = ring->leds[i];
        uint8_t new_level = (data[j] - 48) & 0xf;
        if (new_level != old_level)
        {
            ring->leds[i] = new_level;
            if (i < 32)
            {
                ring->dirty_leds_lo |= (1 << i);
            }
            else
            {
                ring->dirty_leds_hi |= (1 << (i - 32));
            }
        }
    }
}

static void serial(const char *data)
{
    if (strncmp("arc: map ", data, 9) == 0)
    {
        handleMapMessage(data);
    }
    else if (strncmp("arc: mod?", data, 9) == 0)
    {
        enable_arc_mod = true;
    }
}

void selectRing(PlaydateAPI *pd, uint8_t index, bool a_button_down)
{
    for (int i = 0; i < N_RINGS; i++)
    {
        ring_t *ring = &rings[i];
        if (i == index)
        {
            if (!ring->selected)
            {
                ring->selected = true;
                ring->dirty = true;
                if (a_button_down && !ring->pressed)
                {
                    ring->pressed = true;
                    sendKeyPress(pd, i, true);
                }
            }
        }
        else if (!multi_select && ring->selected)
        {
            ring->selected = false;
            ring->dirty = true;
            if (ring->pressed)
            {
                ring->pressed = false;
                sendKeyPress(pd, i, false);
            }
        }
    }
    last_select = index;
}

static int update(void *userdata)
{
    PlaydateAPI *pd = userdata;
    if (enable_arc_mod)
    {
        sendModEnabled(pd, true);
        enable_arc_mod = false;
    }

    const struct playdate_graphics *gfx = pd->graphics;

    PDButtons current, pushed, released;
    pd->system->getButtonState(&current, &pushed, &released);

    bool a_button_down = (pushed & kButtonA) || (current & kButtonA);

    if (pushed & kButtonLeft)
    {
        selectRing(pd, 0, a_button_down);
    }
    else if (pushed & kButtonUp)
    {
        selectRing(pd, 1, a_button_down);
    }
    else if (pushed & kButtonDown)
    {
        selectRing(pd, 2, a_button_down);
    }
    else if (pushed & kButtonRight)
    {
        selectRing(pd, 3, a_button_down);
    }

    if (pushed & kButtonA)
    {
        for (int i = 0; i < N_RINGS; i++)
        {
            ring_t *ring = &rings[i];
            if (ring->selected)
            {
                ring->pressed = true;
                ring->dirty = true;
                sendKeyPress(pd, i, true);
            }
        }
    }
    else if (released & kButtonA)
    {
        for (int i = 0; i < N_RINGS; i++)
        {
            ring_t *ring = &rings[i];
            if (ring->selected && ring->pressed)
            {
                ring->pressed = false;
                ring->dirty = true;
                sendKeyPress(pd, i, false);
            }
        }
    }

    if (pushed & kButtonB)
    {
        multi_select = true;
        for (int i = 0; i < N_RINGS; i++)
        {
            ring_t *ring = &rings[i];
            if (i != last_select)
            {
                if (ring->selected)
                {
                    ring->selected = false;
                    ring->dirty = true;
                }
            }
        }
    }
    else if (released & kButtonB)
    {
        multi_select = false;
    }

    float delta = pd->system->getCrankChange();
    if (delta != 0.0f)
    {
        for (int i = 0; i < N_RINGS; i++)
        {
            ring_t *ring = &rings[i];
            if (ring->selected)
            {
                sendEncDelta(pd, i, delta);
            }
        }
    }

    for (int i = 0; i < N_RINGS; i++)
    {
        ring_t *ring = &rings[i];
        if (ring->dirty_leds_lo != 0 || ring->dirty_leds_hi != 0)
        {
            const uint8_t *leds = ring->leds;
            for (int j = 0; j < N_LEDS; j++)
            {
                bool dirty = j < 32 ? (ring->dirty_leds_lo & (1 << j)) : (ring->dirty_leds_hi & (1 << (j - 32)));
                if (dirty)
                {
                    const uint8_t level = leds[j];
                    gfx->fillPolygon(
                        4,
                        ring_led_quads[i][j],
                        (LCDColor)&level_patterns[level],
                        kPolygonFillNonZero);
                }
            }
            ring->dirty_leds_lo = 0;
            ring->dirty_leds_hi = 0;
        }

        if (ring->dirty)
        {
            LCDColor key_color = kColorWhite;
            if (ring->selected)
            {
                if (ring->pressed)
                {
                    key_color = kColorBlack;
                }
                else
                {
                    key_color = (LCDColor)&level_patterns[SELECT_LEVEL];
                }
            }
            gfx->fillEllipse(
                ring->anchor_x + LED_LENGTH + LED_PADDING + 2,
                ring->anchor_y + LED_LENGTH + LED_PADDING + 2,
                2 * (RING_RADIUS - LED_LENGTH - LED_PADDING - 2) + 1, // looks slightly better when wider
                2 * (RING_RADIUS - LED_LENGTH - LED_PADDING - 2),
                0.0f,
                360.0f,
                key_color);
            gfx->drawEllipse(
                ring->anchor_x + LED_LENGTH + LED_PADDING + 1,
                ring->anchor_y + LED_LENGTH + LED_PADDING + 1,
                2 * (RING_RADIUS - LED_LENGTH - LED_PADDING - 1) + 1, // looks slightly better when wider
                2 * (RING_RADIUS - LED_LENGTH - LED_PADDING - 1),
                1,
                0.0f,
                360.0f,
                kColorBlack);
            gfx->drawEllipse(
                ring->anchor_x - 3,
                ring->anchor_y - 3,
                2 * (RING_RADIUS + 3),
                2 * (RING_RADIUS + 3),
                1,
                0.0f,
                360.0f,
                kColorBlack);
        }

        ring->dirty = false;
    }

    // pd->system->drawFPS(0, 0);
    return 1;
}
