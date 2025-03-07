#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "pd_api.h"

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
    // alpha goes from 0 (invisible) to 1 (fully opaque)
    const unsigned int threshold = (unsigned int)((1.f - alpha) * 64.f);
    for (int row = 0; row < 8; ++row)
    {
        for (int col = 0; col < 8; ++col)
        {
            if (bayer[row][col] >= threshold)
            {
                (*pattern)[8 + row] |= (1 << col); // set
            }
            else
            {
                (*pattern)[8 + row] &= ~(1 << col); // clear
            }
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

int ring_led_quads[N_RINGS][N_LEDS][8];

typedef struct _ring
{
    uint16_t x;
    uint16_t y;
    uint16_t anchor_x;
    uint16_t anchor_y;
    bool dirty;
    bool selected;
    bool pressed;
    uint8_t levels[N_LEDS];
} ring_t;

ring_t rings[N_RINGS] = {
    {.x = RING1_X,
     .y = RING1_Y,
     .anchor_x = RING1_X - RING_RADIUS,
     .anchor_y = RING1_Y - RING_RADIUS,
     .dirty = true,
     .selected = true,
     .pressed = false},
    {.x = RING2_X,
     .y = RING2_Y,
     .anchor_x = RING2_X - RING_RADIUS,
     .anchor_y = RING2_Y - RING_RADIUS,
     .dirty = true,
     .selected = false,
     .pressed = false},
    {.x = RING3_X,
     .y = RING3_Y,
     .anchor_x = RING3_X - RING_RADIUS,
     .anchor_y = RING3_Y - RING_RADIUS,
     .dirty = true,
     .selected = false,
     .pressed = false},
    {.x = RING4_X,
     .y = RING4_Y,
     .anchor_x = RING4_X - RING_RADIUS,
     .anchor_y = RING4_Y - RING_RADIUS,
     .dirty = true,
     .selected = false,
     .pressed = false}};

bool multi_select = false;
uint8_t last_select = 0;

static void init(PlaydateAPI *pd);
static int update(void *userdata);
static void serial(const char *data);
static void sendModEnabled(PlaydateAPI *pd, bool enabled);
static void sendEncDelta(PlaydateAPI *pd, uint8_t n, double delta);
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
    pd->system->logToConsole("~arc: mod %d", enabled);
}

static void sendEncDelta(PlaydateAPI *pd, uint8_t n, double delta)
{
    pd->system->logToConsole("~arc: enc %d %f", n, delta);
}

static void sendKeyPress(PlaydateAPI *pd, uint8_t n, bool s)
{
    pd->system->logToConsole("~arc: mod %d %d", n, s);
}

PlaydateAPI *pd_;

static void init(PlaydateAPI *pd)
{
    pd_ = pd;

    pd->system->setUpdateCallback(update, pd);
    pd->system->setSerialMessageCallback(serial);
    pd->display->setRefreshRate(50);

    for (int i = 0; i < 16; i++)
    {
        setBlackPattern(&level_patterns[i]);
        setPatternAlpha(&level_patterns[i], i / 15.0f);
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

            rings[i].levels[j] = 0;

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

void handleMapMessage(const char *data)
{
    int index = (data[10] - 48) & 0x3;
    ring_t *ring = &rings[index];
    for (int i = 12; i < 76; i++)
    {
        ring->levels[i - 12] = (data[i] - 48) & 0xf;
    }
    ring->dirty = true;
}

static void serial(const char *data)
{
    if (strncmp("~arc: map ", data, 10) == 0)
    {
        handleMapMessage(data);
    }
    else if (strncmp("~arc: mod?", data, 10) == 0)
    {
        sendModEnabled(pd_, true);
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
    bool enc = delta != 0.0f;

    for (int i = 0; i < N_RINGS; i++)
    {
        ring_t *ring = &rings[i];
        if (enc && ring->selected)
        {
            sendEncDelta(pd, i, delta);
        }

        if (!ring->dirty)
        {
            continue;
        }
        gfx->fillEllipse(
            ring->anchor_x - 2,
            ring->anchor_y - 2,
            ring_diameter + 4,
            ring_diameter + 4,
            0.0f,
            360.0f,
            kColorWhite);
    }

    for (int i = 0; i < N_RINGS; i++)
    {
        ring_t *ring = &rings[i];
        if (!ring->dirty)
        {
            continue;
        }

        const uint8_t *levels = ring->levels;
        for (int j = 0; j < N_LEDS; j++)
        {
            const uint8_t level = levels[j];
            gfx->fillPolygon(
                4,
                ring_led_quads[i][j],
                (LCDColor)&level_patterns[level],
                kPolygonFillNonZero);
        }

        gfx->drawEllipse(
            ring->anchor_x - 3,
            ring->anchor_y - 3,
            2 * (RING_RADIUS + 3),
            2 * (RING_RADIUS + 3),
            1,
            0.0f,
            360.0f,
            kColorBlack);

        if (ring->selected)
        {
            LCDColor key_color;
            if (ring->pressed)
            {
                key_color = kColorBlack;
            }
            else
            {
                key_color = (LCDColor)&level_patterns[4];
            }
            gfx->fillEllipse(
                ring->anchor_x + LED_LENGTH + LED_PADDING + 2,
                ring->anchor_y + LED_LENGTH + LED_PADDING + 2,
                2 * (RING_RADIUS - LED_LENGTH - LED_PADDING - 2) + 1, // looks slightly better when wider
                2 * (RING_RADIUS - LED_LENGTH - LED_PADDING - 2),
                0.0f,
                360.0f,
                key_color);
        }
        gfx->drawEllipse(
            ring->anchor_x + LED_LENGTH + LED_PADDING + 1,
            ring->anchor_y + LED_LENGTH + LED_PADDING + 1,
            2 * (RING_RADIUS - LED_LENGTH - LED_PADDING - 1) + 1, // looks slightly better when wider
            2 * (RING_RADIUS - LED_LENGTH - LED_PADDING - 1),
            1,
            0.0f,
            360.0f,
            kColorBlack);

        ring->dirty = false;
    }

    // pd->system->drawFPS(0, 0);
    return 1;
}
