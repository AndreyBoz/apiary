#include "screen.h"
#include "board.h"
#include "node_config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned char frame[1025];

static const unsigned char font[91][5] = {
    ['0'] = {62, 81, 73, 69, 62},
    ['1'] = {0, 66, 127, 64, 0},
    ['2'] = {66, 97, 81, 73, 70},
    ['3'] = {65, 73, 73, 73, 54},
    ['4'] = {24, 20, 18, 127, 16},
    ['5'] = {79, 73, 73, 73, 49},
    ['6'] = {62, 73, 73, 73, 48},
    ['7'] = {1, 113, 9, 5, 3},
    ['8'] = {54, 73, 73, 73, 54},
    ['9'] = {6, 73, 73, 73, 62},
    ['A'] = {126, 9, 9, 9, 126},
    ['B'] = {127, 73, 73, 73, 54},
    ['C'] = {62, 65, 65, 65, 34},
    ['D'] = {127, 65, 65, 65, 62},
    ['E'] = {127, 73, 73, 73, 65},
    ['F'] = {127, 9, 9, 9, 1},
    ['G'] = {62, 65, 73, 73, 122},
    ['H'] = {127, 8, 8, 8, 127},
    ['I'] = {0, 65, 127, 65, 0},
    ['J'] = {48, 64, 65, 63, 1},
    ['K'] = {127, 8, 20, 34, 65},
    ['L'] = {127, 64, 64, 64, 64},
    ['M'] = {127, 2, 12, 2, 127},
    ['N'] = {127, 2, 4, 8, 127},
    ['O'] = {62, 65, 65, 65, 62},
    ['P'] = {127, 9, 9, 9, 6},
    ['Q'] = {62, 65, 81, 33, 94},
    ['R'] = {127, 9, 25, 41, 70},
    ['S'] = {70, 73, 73, 73, 49},
    ['T'] = {1, 1, 127, 1, 1},
    ['U'] = {63, 64, 64, 64, 63},
    ['V'] = {31, 32, 64, 32, 31},
    ['W'] = {127, 32, 24, 32, 127},
    ['X'] = {99, 20, 8, 20, 99},
    ['Y'] = {3, 4, 120, 4, 3},
    ['Z'] = {97, 81, 73, 69, 67},
    ['%'] = {49, 8, 4, 2, 33},
    ['-'] = {8, 8, 8, 8, 8},
    ['.'] = {0, 96, 96, 0, 0},
    ['>'] = {65, 34, 20, 8, 0},
    [' '] = {0, 0, 0, 0, 0},
};

static void text(unsigned page, const char *value)
{
    unsigned column = 0;
    while (*value && column + 5 <= 128) {
        unsigned char ch = (unsigned char)*value++;
        if (ch < sizeof(font) / sizeof(font[0])) {
            memcpy(&frame[1 + page * 128 + column], font[ch], 5);
        }
        column += 6;
    }
}

esp_err_t screen_init(void)
{
    static const unsigned char settings[] = {
        0x00, 0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00,
        0x40, 0x8d, 0x14, 0x20, 0x00, 0xa1, 0xc8,
        0xda, 0x12, 0x81, 0x7f, 0xd9, 0xf1, 0xdb, 0x40,
        0xa4, 0xa6, 0xaf,
    };
    return board_oled_write(settings, sizeof(settings));
}

esp_err_t screen_draw(const measurement_t *data, bool gateway, bool menu, bool cursor)
{
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x40;
    char line[32];
    if (menu) {
        text(0, "SELECT ROLE");
        text(2, cursor ? "> GATEWAY" : "  GATEWAY");
        text(4, cursor ? "  LEAF" : "> LEAF");
        text(7, "SHORT NEXT  LONG OK");
    } else {
        snprintf(line, sizeof(line), "%s %s %d%%", HIVE_ID, gateway ? "GATEWAY" : "LEAF",
                 data->charge);
        if (!data->battery_ok) {
            snprintf(line, sizeof(line), "%s %s --%%", HIVE_ID, gateway ? "GATEWAY" : "LEAF");
        }
        text(0, line);
        if (isfinite(data->grams)) {
            snprintf(line, sizeof(line), "MASS %.2f KG", data->grams / 1000.0);
            text(2, line);
        } else {
            text(2, "MASS -- KG");
        }
        if (isfinite(data->dbfs)) {
            snprintf(line, sizeof(line), "SOUND %.0f DBFS", (double)data->dbfs);
            text(4, line);
        } else {
            text(4, "SOUND -- DBFS");
        }
        if (data->battery_ok) {
            snprintf(line, sizeof(line), "ADDR %02X  %.2f V", (unsigned)NODE_ADDRESS, data->millivolts / 1000.0);
        } else {
            snprintf(line, sizeof(line), "ADDR %02X  -- V", (unsigned)NODE_ADDRESS);
        }
        text(6, line);
    }
    static const unsigned char address[] = {0x00, 0x21, 0, 127, 0x22, 0, 7};
    esp_err_t result = board_oled_write(address, sizeof(address));
    if (result != ESP_OK) {
        return result;
    }
    return board_oled_write(frame, sizeof(frame));
}
