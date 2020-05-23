/*--------------------------------------------------------------------
 * Kujira: 2D tile-based game
 *
 * Copyright 2020 Sean Tommasi
 *--------------------------------------------------------------------*/
#include <SDL2/SDL.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

#define TILESIZE 48
#define DISPLAY_PW 960
#define DISPLAY_PH 540
#define DISPLAY_TW (DISPLAY_PW / TILESIZE)
#define DISPLAY_TH (DISPLAY_PH / TILESIZE)
#define MAPLENGTH 2000
#define MAPWIDTH 2000
#define MAPHEIGHT 2000
#define SCROLL_TW (DISPLAY_TW - 5)
#define SCROLL_TH (DISPLAY_TH - 5)
#define SCROLL_PW (SCROLL_TW * TILESIZE)
#define SCROLL_PH (SCROLL_TH * TILESIZE)

typedef struct display {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int width, height;
    int strideX, strideY;
    unsigned char *buffer;
} Display;

typedef struct input {
    int key_up;
    int key_down;
    int key_left;
    int key_right;
    int key_z;
    int key_x;
    int key_q;
    int key_r;
} Input;

typedef struct tile {
    int x, y;
    int flatCoord;
} Tile;

typedef struct camera {
    int tileX, tileY;
    float pixelX, pixelY;
    int destTileX, destTileY;
    float accelX, accelY;
    float velocityX, velocityY;
} Camera;

/* Tell GCC to pack the struct tightly for correct overlay of data */
#pragma pack(push, 1)
typedef struct bitmapHeader {
    /* Header */
    short signature;
    int filesize;
    int reserved;
    int dataoffset;

    /* Info header */
    int infoheadersize;
    int width;
    int height;
    short planes;
    short bitsperpixel;
    int compression;
    int imagesize;
    int xpixelsperm;
    int ypixelsperm;
    int colorsused;
    int importantcolors;
} BitmapHeader;
#pragma pack(pop)

typedef struct windowsBMP {
    char *data;
    BitmapHeader header;
} WindowsBMP;

typedef struct bitmap {
    unsigned int *data;
    int width;
    int height;
} Bitmap;

typedef struct ripple {
    Bitmap bitmap;
    float radius;
    float alpha;
    int tileX, tileY;
    int active;
} Ripple;

Ripple rippleArray[5] = {};
int rippleIndex = 0;

typedef struct player {
    int x, y;
    int destX, destY;
    float pixelX, pixelY;
    float accelX, accelY;
    float velocityX, velocityY;
    Bitmap bitmap;
    int newDirection, oldDirection;
    float angle;
    float destAngle;
    float scale;
    float destScale;
} Player;

Camera cam;
Player player;
int running = 1;
float dtFrame;
WindowsBMP playerBitmap;
Bitmap bgBufferOld;
Bitmap bgBufferNew;

Tile tileArray[MAPLENGTH];

Display display;
Input newInput, oldInput;

/*--------------------------------------------------------------------
 * tileCompare
 *
 * For the bsearch routine to find a tile by its flattened coordinate
 * number. Returns 0 if equal.
 *--------------------------------------------------------------------*/
int tileCompare(const void *a, const void *b)
{
    return ((Tile *)a)->flatCoord - ((Tile *)b)->flatCoord;
}

/*--------------------------------------------------------------------
 * borderCollide
 *
 * Return 0 for "no collide" if there is a tile at the given x and y
 * coordinates. Return 1 for "collide" if there is no tile.
 *--------------------------------------------------------------------*/
int borderCollide(int x, int y)
{
    Tile tile;
    tile.x = x;
    tile.y = y;
    tile.flatCoord = (tile.y * MAPWIDTH) + tile.x;
    if (bsearch(&tile, tileArray, MAPLENGTH, sizeof(Tile), tileCompare)) {
        return 0;
    }
    return 1;
}

/*--------------------------------------------------------------------
 * applyColor
 *
 * Blend the src color with whatever color is already at *dest, in
 * accordance with the former's alpha channel.
 *--------------------------------------------------------------------*/
void applyColor(unsigned int src, unsigned int *dest)
{
    /* Linear blend:
     *
     * C = A + t(B - A)
     *   = A + tB - tA
     *   = A - tA + tB
     * C = (1 - t)A + tB
     *
     * The first ordering of the equation says that the resulting color
     * is some portion t, the alpha channel, of the distance between A
     * and B, i.e. the destination and source pixels. The final
     * reordering of this equation says that the resulting color is the
     * alpha portion of B, plus a portion of A equal to the sacrificed
     * portion of B. */
    float a = (src & 0xFF) / 255.0f;
    int r = ((1 - a) * (*dest >> 24 & 0xFF)) + (a * (src >> 24 & 0xFF));
    int g = ((1 - a) * (*dest >> 16 & 0xFF)) + (a * (src >> 16 & 0xFF));
    int b = ((1 - a) * (*dest >> 8 & 0xFF)) + (a * (src >> 8 & 0xFF));
    *dest = r << 24 | g << 16 | b << 8 | (src & 0xFF);
}

/*--------------------------------------------------------------------
 * loadBitmap
 *
 * Open a Windows BMP file, extract its header information, read its
 * data, and finally load the data into another buffer, beginning with
 * the last row and moving upward in order to correct for the fact
 * that Windows BMPs are upside down.
 *--------------------------------------------------------------------*/
Bitmap loadBitmap(const char *filename)
{
    FILE *fp;
    WindowsBMP bmp;
    Bitmap bitmap;
    fp = fopen(filename, "r");
    fseek(fp, 0, SEEK_END);
    int n = ftell(fp);
    rewind(fp);
    bmp.data = (char *)malloc(n);
    fread(bmp.data, 1, n, fp);
    bmp.header = *(BitmapHeader *)bmp.data;
    bmp.data += bmp.header.dataoffset;
    int dataLen = bmp.header.width * bmp.header.height;
    bitmap.data = (unsigned int *)malloc(dataLen * sizeof(int));
    unsigned int *p = (unsigned int *)bmp.data + (bmp.header.width * (bmp.header.height - 1));
    bitmap.width = bmp.header.width;
    bitmap.height = bmp.header.height;
    for (int y = 0; y < bitmap.height; ++y) {
        for (int x = 0; x < bitmap.width; ++x) {
            unsigned int *dest = bitmap.data + (y * bitmap.width) + x;
            *dest = *(p - (y * bitmap.width) + x);
        }
    }
    return bitmap;
}

/*--------------------------------------------------------------------
 * vflipBitmap
 *
 * Return a vertically flipped bitmap.
 *--------------------------------------------------------------------*/
Bitmap vflipBitmap(Bitmap bitmap)
{
    int w = bitmap.width;
    int h = bitmap.height;
    Bitmap vflippedBitmap;
    vflippedBitmap.data = (unsigned int *)calloc(w * h, sizeof(int));
    vflippedBitmap.width = w;
    vflippedBitmap.height = h;
    unsigned int *dest = vflippedBitmap.data;
    dest += (w * h) - w;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            *dest++ = *(bitmap.data + (y * w) + x);
        }
        dest -= w * 2;
    }
    return vflippedBitmap;
}

/*--------------------------------------------------------------------
 * rotateBitmap
 *
 * Return a bitmap rotated to any angle expressed in radians.
 *--------------------------------------------------------------------*/
Bitmap rotateBitmap(Bitmap bitmap, float angle)
{
    int w = bitmap.width;
    int h = bitmap.height;
    Bitmap rotatedBitmap;
    rotatedBitmap.data = (unsigned int *)calloc(w * h, sizeof(int));
    rotatedBitmap.width = w;
    rotatedBitmap.height = h;
    float angleSin = sin(angle);
    float angleCos = cos(angle);
    float cx = w / 2;
    float cy = h / 2;
    unsigned int *dest = rotatedBitmap.data;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            /* Get the rotated coordinates */
            float rx = (((float)x - cx) * angleCos - ((float)y - cx) * angleSin) + cx;
            float ry = (((float)x - cx) * angleSin + ((float)y - cy) * angleCos) + cy;
            if (rx < 0 || ry < 0 || rx >= w - 1 || ry >= h - 1) {
                continue;
            }
            /* Bilinear blending to smooth out edges */
            unsigned int tl = *(bitmap.data + ((int)floor(ry) * w) + (int)floor(rx));
            unsigned int tr = *(bitmap.data + ((int)floor(ry) * w) + (int)ceil(rx));
            unsigned int bl = *(bitmap.data + ((int)ceil(ry) * w) + (int)floor(rx));
            unsigned int br = *(bitmap.data + ((int)ceil(ry) * w) + (int)ceil(rx));
            float dx = rx - floor(rx);
            float dy = ry - floor(ry);
            float topA = (1 - dx) * (tl >> 24 & 255) + dx * (tr >> 24 & 255);
            float topR = (1 - dx) * (tl >> 16 & 255) + dx * (tr >> 16 & 255);
            float topG = (1 - dx) * (tl >> 8 & 255) + dx * (tr >> 8 & 255);
            float topB = (1 - dx) * (tl >> 0 & 255) + dx * (tr >> 0 & 255);
            float botA = (1 - dx) * (bl >> 24 & 255) + dx * (br >> 24 & 255);
            float botR = (1 - dx) * (bl >> 16 & 255) + dx * (br >> 16 & 255);
            float botG = (1 - dx) * (bl >> 8 & 255) + dx * (br >> 8 & 255);
            float botB = (1 - dx) * (bl >> 0 & 255) + dx * (br >> 0 & 255);
            float a = (1 - dy) * topA + dy * botA;
            float r = (1 - dy) * topR + dy * botR;
            float g = (1 - dy) * topG + dy * botG;
            float b = (1 - dy) * topB + dy * botB;
            unsigned int color = 0;
            color |= (int)round(r) << 24;
            color |= (int)round(g) << 16;
            color |= (int)round(b) << 8;
            color |= (int)round(a) << 0;
            applyColor(color, dest + x);
        }
        dest += w;
    }
    return rotatedBitmap;
}

/*--------------------------------------------------------------------
 * scaleBitmap
 *
 * Return a bitmap scaled to any real number.
 *--------------------------------------------------------------------*/
Bitmap scaleBitmap(Bitmap bitmap, float scale)
{
    /* Original dimensions */
    int w = bitmap.width;
    int h = bitmap.height;
    /* Scaled dimensions */
    float wScaled = (float)w * scale;
    float hScaled = (float)h * scale;
    /* Ratio by which the original bitmap is scaled up or down */
    float wRatio = (float)w / wScaled;
    float hRatio = (float)h / hScaled;
    Bitmap scaledBitmap;
    scaledBitmap.data = (unsigned int *)calloc((int)(wScaled * hScaled), sizeof(int));
    scaledBitmap.width = (int)(wScaled);
    scaledBitmap.height = (int)(hScaled);
    unsigned int *dest = scaledBitmap.data;
    /* Move pixel by pixel through the new scaled bitmap, but multiply
     * the movement by the ratios so that we either repeat or skip
     * pixels depending on if we're scaling up or down. */
    for (int y = 0; y < scaledBitmap.height; ++y) {
        int yStride = (int)(y * hRatio) * bitmap.width;
        for (int x = 0; x < scaledBitmap.width; ++x) {
            unsigned int *src = bitmap.data + yStride + (int)(x * wRatio);
            *(dest + x) = *src;
        }
        dest += scaledBitmap.width;
    }
    return scaledBitmap;
}

/*--------------------------------------------------------------------
 * drawBitmap
 *
 * Copy an RGBA bitmap, rotated and scaled as needed, to the game's
 * primary display buffer.
 * TODO: This function is particularly designed for the main player,
 * currently the only sprite in the game. It will need to be
 * generalized at some point.
 *--------------------------------------------------------------------*/
void drawBitmap(Bitmap bitmap, int x, int y, float angle, float scale)
{
    Bitmap scaledBitmap = scaleBitmap(bitmap, scale);
    int x1 = x + (bitmap.width - scaledBitmap.width) / 2;
    int y1 = y + (bitmap.height - scaledBitmap.height) / 2;
    int x2 = x1 + scaledBitmap.width;
    int y2 = y1 + scaledBitmap.height;
    int xoff = 0, yoff = 0;
    if (x1 < 0) {
        xoff = -x1;
        x1 = 0;
    }
    if (y1 < 0) {
        yoff = -y1;
        y1 = 0;
    }
    if (x2 > display.width) {
        x2 = display.width;
    }
    if (y2 > display.height) {
        y2 = display.height;
    }
    Bitmap rotatedBitmap = rotateBitmap(scaledBitmap, angle);
    if (fabs(angle - M_PI) < 0.1f) {
        rotatedBitmap = vflipBitmap(rotatedBitmap);
    }
    unsigned int *src = rotatedBitmap.data + (yoff * rotatedBitmap.width) + xoff;
    unsigned int *dest = (unsigned int *)display.buffer + (y1 * display.width);
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            unsigned int color = *(src + (x - x1));
            if (color != 0xffffffff && color != 0) {
                color = 0x000000ff;
            }
            applyColor(color, dest + x);
        }
        src += rotatedBitmap.width;
        dest += display.width;
    }
    free(rotatedBitmap.data);
    free(scaledBitmap.data);
}

/*--------------------------------------------------------------------
 * initRipple
 *
 * Initialize a ripple struct at a given location in an array.
 *--------------------------------------------------------------------*/
void initRipple(int x, int y)
{
    /* Wrap around the array and overwrite */
    if (rippleIndex > 4) {
        rippleIndex = 0;
    }
    Ripple *ripple = &rippleArray[rippleIndex];
    ripple->bitmap.width = 100;
    ripple->bitmap.height = 100;
    ripple->bitmap.data = (unsigned int *)calloc(ripple->bitmap.width * ripple->bitmap.height, sizeof(int));
    ripple->radius = 20.0f;
    ripple->alpha = 1.0f;
    ripple->tileX = x;
    ripple->tileY = y;
    ripple->active = 1;
    ++rippleIndex;
}

/*--------------------------------------------------------------------
 * fillBitmap
 *
 * Fill a bitmap buffer with a given color.
 *--------------------------------------------------------------------*/
void fillBitmap(Bitmap *bitmap, int color)
{
    for (int y = 0; y < bitmap->height; ++y) {
        for (int x = 0; x < bitmap->width; ++x) {
            *(bitmap->data + (y * bitmap->width) + x) = color;
        }
    }
}

/*--------------------------------------------------------------------
 * animateRipple
 *
 * Draw a series of continuously expanding ripples, if they exist.
 *--------------------------------------------------------------------*/
void animateRipple()
{
    /* Iterate through the 4 ripples that can exist at a time */
    for (int i = 0; i < 5; ++i) {
        Ripple *ripple = &rippleArray[i];
        /* Ripples are marked inactive at a certain radial limit */
        if (!ripple->active) {
            continue;
        }
        /* Clear the whole bitmap with full alpha transparency */
        fillBitmap(&ripple->bitmap, 0);
        /* Center point of the ripple circle */
        float cx = ripple->bitmap.width / 2;
        float cy = ripple->bitmap.height / 2;
        /* Location of ripple on the screen */
        int screenX = (ripple->tileX - cam.tileX + (DISPLAY_TW / 2)) * TILESIZE;
        screenX -= cx;
        screenX += TILESIZE / 2;
        screenX -= cam.pixelX;
        int screenY = (ripple->tileY - cam.tileY + (DISPLAY_TH / 2)) * TILESIZE;
        screenY -= cy;
        screenY += TILESIZE / 2;
        screenY -= cam.pixelY;
        /* Expands each frame */
        ripple->radius += 1.0f;
        /* Fades each frame */
        ripple->alpha -= 0.03f;
        /* For the gradient within the ripple */
        float subAlpha = 1.0f;
        /* Each ripple consists of 4.0 * 2 circles */
        for (float rippleLine = 0.0f; rippleLine < 4.0f; rippleLine += 1.0f) {
            /* Gradient within ripple */
            unsigned int color = 0x6f6fbf << 8;
            color |= (int)((ripple->alpha * 255) * subAlpha);
            subAlpha -= 0.2f;
            /* Inner and outer circle for bidirectional gradient */
            for (float angle = 0.0f; angle < 2 * M_PI; angle += 0.01f) {
                unsigned int *pixel;
                float x, y;
                x = cx + ((ripple->radius + rippleLine) * cos(angle));
                y = cy + ((ripple->radius + rippleLine) * sin(angle));
                pixel = ripple->bitmap.data;
                pixel += ((int)y * ripple->bitmap.width) + (int)x;
                *pixel = color;
                x = cx + ((ripple->radius - rippleLine) * cos(angle));
                y = cy + ((ripple->radius - rippleLine) * sin(angle));
                pixel = ripple->bitmap.data;
                pixel += ((int)y * ripple->bitmap.width) + (int)x;
                *pixel = color;
            }
        }
        /* Draw the ripple bitmap onto the game's display */
        unsigned int *src = ripple->bitmap.data;
        unsigned int *dest = (unsigned int *)display.buffer;
        dest += (screenY * display.width) + screenX;
        for (int y = 0; y < ripple->bitmap.height; ++y) {
            for (int x = 0; x < ripple->bitmap.width; ++x) {
                if (*(dest + x) != 0xeb9b34ff && *(dest + x) != 0x000000ff) {
                    applyColor(*src, dest + x);
                }
                ++src;
            }
            dest += display.width;
        }
        /* Kill the ripple if it gets too big */
        if (ripple->radius >= (ripple->bitmap.width - 5) / 2) {
            ripple->active = 0;
            free(ripple->bitmap.data);
        }
    }
}

/*--------------------------------------------------------------------
 * initMap
 *
 * Initialize a tilemap with a random walk. Store the coordinates in
 * an array in the order they are determined by the walk. For each
 * coordinate pair, calculate a unique "flat" coordinate with (y *
 * MAPWIDTH) + x. At the end, use this number to sort the array with
 * the standard library's qsort.
 *--------------------------------------------------------------------*/
void initMap()
{
    int mapMinX = -(MAPWIDTH / 2);
    int mapMaxX = MAPWIDTH / 2;
    int mapMinY = -(MAPHEIGHT / 2);
    int mapMaxY = MAPHEIGHT / 2;
    int x = 0;
    int y = 0;
    int r1 = 0;
    int r2 = 0;
    Tile *tile = tileArray;
    int i = 0;
    while (tile - tileArray < MAPLENGTH) {
        int repeat = 0;
        /* Check if the current tile proposal is a repeat */
        for (Tile *p = tileArray; p < tile; ++p) {
            if (p->x == x && p->y == y) {
                repeat = 1;
                break;
            }
        }
        /* If not, push it onto the array and store a flattened
         * version of it for a sorted index */
        if (!repeat) {
            tile->x = x;
            tile->y = y;
            tile->flatCoord = (y * MAPWIDTH) + x;
            ++tile;
        }
        /* Change the directional bias every 20 moves */
        if (i++ % 20 == 0) {
            r2 = rand() % 4;
        }
        /* r1 can be one of 4 directions, or a 5th number that hands
         * the move to the bias r2 */
        r1 = rand() % 5;
        if (r1 == 0 || (r1 == 4 && r2 == 0)) {
            ++x;
        } else if (r1 == 1 || (r1 == 4 && r2 == 1)) {
            --x;
        } else if (r1 == 2 || (r1 == 4 && r2 == 2)) {
            ++y;
        } else if (r1 == 3 || (r1 == 4 && r2 == 3)) {
            --y;
        }
        /* Don't walk past the boundaries */
        if (x > mapMaxX) x = mapMaxX;
        if (y > mapMaxY) y = mapMaxY;
        if (x < mapMinX) x = mapMaxX;
        if (y < mapMinY) y = mapMaxY;
    }
    /* Sort the array by flatCoord for faster access with bsearch */
    qsort(tileArray, MAPLENGTH, sizeof(Tile), tileCompare);
}

/*--------------------------------------------------------------------
 * blitDisplay
 *
 * Call SDL functions to blit the display buffer onto the screen.
 *--------------------------------------------------------------------*/
void blitDisplay()
{
    SDL_RenderClear(display.renderer);
    SDL_UpdateTexture(
        display.texture,
        NULL,
        display.buffer,
        display.width * sizeof(int));
    SDL_RenderCopy(
        display.renderer,
        display.texture,
        NULL, NULL);
    SDL_RenderPresent(display.renderer);
}

/*--------------------------------------------------------------------
 * initDisplay
 *
 * Initialize SDL and display buffer.
 *--------------------------------------------------------------------*/
void initDisplay()
{
    display.width = DISPLAY_PW;
    display.height = DISPLAY_PH;
    display.strideX = 4;
    display.strideY = display.width * display.strideX;
    SDL_Init(SDL_INIT_VIDEO);
    display.window = SDL_CreateWindow(
        "Kujira",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        display.width, display.height,
        SDL_WINDOW_RESIZABLE);
    display.renderer = SDL_CreateRenderer(
        display.window, -1, SDL_RENDERER_ACCELERATED);
    display.texture = SDL_CreateTexture(display.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        display.width, display.height);
    SDL_RenderSetLogicalSize(
        display.renderer,
        display.width, display.height);
    display.buffer = malloc(display.strideY * display.height);
}

/*--------------------------------------------------------------------
 * drawRect
 *
 * Draw a rectangle to the game's display buffer.
 *--------------------------------------------------------------------*/
void drawRect(Bitmap buffer, int x, int y, int w, int h, unsigned int color)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w >= DISPLAY_PW) w = DISPLAY_PW - x;
    if (y + h >= DISPLAY_PH) h = DISPLAY_PH - y;
    unsigned int *pixel = buffer.data;
    pixel += y * buffer.width;
    pixel += x;
    for (int rectY = 0; rectY < h; ++rectY) {
        for (int rectX = 0; rectX < w; ++rectX) {
            applyColor(color, pixel + rectX);
        }
        pixel += buffer.width;
    }
}

/*--------------------------------------------------------------------
 * drawMap
 *
 * Draw a new portion of the map onto the newer of two buffers
 * whenever the camera scrolls. The actual displaying of the buffer is
 * accomplished by just copying the contents of these two buffers to
 * the game's display buffer, so that the expensive calculations in
 * this function don't need to be performed during most frames.
 *--------------------------------------------------------------------*/
void drawMap()
{
    /* We want to scroll into the next portion of the map when the
     * player steps into it, but we also don't want to recalculate the
     * visible map on each frame, so we need to keep two buffers: the
     * current (old) and the one to be scrolled into (new). The new
     * becomes the old whenever this function is called, which is only
     * when it's time to scroll. */
    memcpy(
        bgBufferOld.data,
        bgBufferNew.data,
        bgBufferNew.width * bgBufferNew.height * sizeof(int));
    bgBufferOld.width = bgBufferNew.width;
    bgBufferOld.height = bgBufferNew.height;
    fillBitmap(&bgBufferNew, 0xeb9b34ff); // gold
    int pixelX = 0;
    int pixelY = 0;
    int centerX = DISPLAY_TW / 2;
    int centerY = DISPLAY_TH / 2;
    /* Draw tiles from the map with the camera as center point */
    for (int y = cam.destTileY - centerY; y < cam.destTileY + centerY + 2; ++y) {
        for (int x = cam.destTileX - centerX; x < cam.destTileX + centerX + 1; ++x) {
            Tile tile;
            tile.x = x;
            tile.y = y;
            tile.flatCoord = (tile.y * MAPWIDTH) + tile.x;
            if (bsearch(&tile, tileArray, MAPLENGTH, sizeof(Tile), tileCompare)) {
                unsigned int color = 0x4f4f9fff; // blue
                /* Tile's shadow */
                drawRect(
                    bgBufferNew,
                    pixelX,
                    pixelY,
                    TILESIZE,
                    TILESIZE,
                    0x000000ff);
                /* Actual tile */
                drawRect(
                    bgBufferNew,
                    pixelX - 2,
                    pixelY - 2,
                    TILESIZE - 2,
                    TILESIZE - 2,
                    color);
            }
            pixelX += TILESIZE;
        }
        pixelX = 0;
        pixelY += TILESIZE;
    }
}

/*--------------------------------------------------------------------
 * drawBackground
 *
 * This function, which, unlike drawMap, is called every frame,
 * actually composes the displayed map by copying the background
 * buffers to the display buffer. If the camera is in the middle of
 * scrolling, then a composite of the two buffers is put together.
 * Otherwise, the latest contents of the camera's updated view are
 * just memcpy'd.
 *--------------------------------------------------------------------*/
void drawBackground()
{
    int minX = (int)cam.pixelX;
    int maxX = minX + DISPLAY_PW;
    int minY = (int)cam.pixelY;
    int maxY = minY + DISPLAY_PH;
    /* If we're in the middle of a scroll */
    if (cam.tileX != cam.destTileX || cam.tileY != cam.destTileY) {
        for (int y = minY; y < maxY; ++y) {
            for (int x = minX; x < maxX; ++x) {
                unsigned int *src;
                int newX = x;
                int newY = y;
                /* If x or y are negative, or if they're greater than
                 * the pixel width of the screen, then they should
                 * actually refer to a position on the new buffer.
                 * Otherwise, this portion of the screen should still
                 * be showing the old buffer. */
                if (x < 0) {
                    src = bgBufferNew.data;
                    newX = x + SCROLL_PW;
                } else if (x >= DISPLAY_PW) {
                    src = bgBufferNew.data;
                    newX = x - SCROLL_PW;
                } else if (y < 0) {
                    src = bgBufferNew.data;
                    newY = y + SCROLL_PH;
                } else if (y >= DISPLAY_PH) {
                    src = bgBufferNew.data;
                    newY = y - SCROLL_PH;
                } else {
                    src = bgBufferOld.data;
                }
                src += (newY * DISPLAY_PW) + newX;
                unsigned int *dest = (unsigned int *)display.buffer;
                dest += ((y - minY) * DISPLAY_PW) + (x - minX);
                *dest = *src;
            }
        }
    /* No scrolling happening, so just copy the latest buffer */
    } else {
        memcpy(
            display.buffer,
            bgBufferNew.data,
            DISPLAY_PW * DISPLAY_PH * sizeof(int));
    }
}

/*--------------------------------------------------------------------
 * getInput
 *
 * Call SDL functions to get the state of the keyboard and set
 * appropriate input flags.
 *--------------------------------------------------------------------*/
void getInput()
{
    SDL_PumpEvents();
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    newInput.key_up = state[SDL_SCANCODE_UP];
    newInput.key_down = state[SDL_SCANCODE_DOWN];
    newInput.key_left = state[SDL_SCANCODE_LEFT];
    newInput.key_right = state[SDL_SCANCODE_RIGHT];
    newInput.key_z = state[SDL_SCANCODE_Z];
    newInput.key_x = state[SDL_SCANCODE_X];
    newInput.key_q = state[SDL_SCANCODE_Q];
    newInput.key_r = state[SDL_SCANCODE_R];
}

/*--------------------------------------------------------------------
 * updateCamera
 *
 * When the player comes close to the edge of the screen, scroll the
 * camera's position in the direction corresponding to the edge. Only
 * allow this to happen if the camera is not already scrolling.
 * TODO: I'm still unsure about this movement. It offers a certain
 * aesthetic, but there's the problem of the player disappearing off
 * the screen if they try to go off screen in a different direction
 * during a scroll.
 *--------------------------------------------------------------------*/
void updateCamera()
{
    if (cam.destTileX == cam.tileX && cam.destTileY == cam.tileY) {
        int horizEdge = (DISPLAY_TW / 2) - 2;
        int vertEdge = (DISPLAY_TH / 2) - 2;
        if (player.x - cam.tileX < -horizEdge) {
            cam.accelX = -((2 * SCROLL_PW) / pow(0.75, 2));
            cam.destTileX = cam.tileX - SCROLL_TW;
            drawMap();
        } else if (player.x - cam.tileX >= horizEdge) {
            cam.accelX = (2 * SCROLL_PW) / pow(0.75, 2);
            cam.destTileX = cam.tileX + SCROLL_TW;
            drawMap();
        } else if (player.y - cam.tileY < -vertEdge) {
            cam.accelY = -((2 * SCROLL_PH) / pow(0.75, 2));
            cam.destTileY = cam.tileY - SCROLL_TH;
            drawMap();
        } else if (player.y - cam.tileY >= vertEdge) {
            cam.accelY = (2 * SCROLL_PH) / pow(0.75, 2);
            cam.destTileY = cam.tileY + SCROLL_TH;
            drawMap();
        }
    }
    if (cam.destTileX != cam.tileX) {
        if (cam.pixelX > SCROLL_PW / 2
            || cam.pixelX < -SCROLL_PW / 2) {
            cam.velocityX -= cam.accelX * dtFrame;
        } else {
            cam.velocityX += cam.accelX * dtFrame;
        }
        cam.pixelX += cam.velocityX * dtFrame;
        if (cam.pixelX >= SCROLL_PW) {
            cam.velocityX = 0;
            cam.pixelX = 0;
            cam.tileX += SCROLL_TW;
        } else if (cam.pixelX < -SCROLL_PW) {
            cam.velocityX = 0;
            cam.pixelX = 0;
            cam.tileX -= SCROLL_TW;
        }
    } else if (cam.destTileY != cam.tileY) {
        if (cam.pixelY > (SCROLL_TH * TILESIZE) / 2
            || cam.pixelY < -SCROLL_PH / 2) {
            cam.velocityY -= cam.accelY * dtFrame;
        } else {
            cam.velocityY += cam.accelY * dtFrame;
        }
        cam.pixelY += cam.velocityY * dtFrame;
        if (cam.pixelY >= SCROLL_PH) {
            cam.velocityY = 0;
            cam.pixelY = 0;
            cam.tileY += SCROLL_TH;
        } else if (cam.pixelY < -SCROLL_PH) {
            cam.velocityY = 0;
            cam.pixelY = 0;
            cam.tileY -= SCROLL_TH;
        }
    }
}

/*--------------------------------------------------------------------
 * updatePlayer
 *
 * Handle the movement, rotation, and scaling of the player. Also
 * initialize water ripples.
 *--------------------------------------------------------------------*/
void updatePlayer()
{
    /* Calculate the accleration for a distance of one tile in 0.2
     * seconds, so that the player always moves one tile at a time. */
    float accel = (2 * TILESIZE) / pow(0.2, 2);
    /* Only check for input if no movement is currently underway. */
    if (player.destX == player.x && player.destY == player.y) {
        if (newInput.key_left) {
            player.accelX = -accel;
            player.destX = player.x - 1;
            player.destAngle = M_PI;
        }
        if (newInput.key_right) {
            player.accelX = accel;
            player.destX = player.x + 1;
            player.destAngle = 0.0f;
        }
        if (newInput.key_up) {
            player.accelY = -accel;
            player.destY = player.y - 1;
            player.destAngle = M_PI / 2.0f;
        }
        if (newInput.key_down) {
            player.accelY = accel;
            player.destY = player.y + 1;
            player.destAngle = (3.0f * M_PI) / 2.0f;
        }
/* Test scaling */
#if 1
        if (newInput.key_z) {
            player.scale -= 0.1f;
        }
        if (newInput.key_x) {
            player.scale += 0.1f;
        }
#endif
    }
    /* If a rotation is incomplete, adjust the player's actual angle
     * toward its destination. */
    if (abs((int)player.destAngle - (int)player.angle) > 0) {
        if (player.destAngle - player.angle >= 0) {
            player.angle += 0.4f;
        } else {
            player.angle -= 0.4f;
        }
        if (player.angle > 2 * M_PI) {
            player.angle = 0.0f;
        } else if (player.angle < 0.0f) {
            player.angle = 2 * M_PI;
        }
    } else {
        player.angle = player.destAngle;
    }
    /* Collision detection against the borders of the tile paths */
    if (player.destX != player.x &&
        borderCollide(player.destX, player.y)) {
        player.destX = player.x;
    }
    if (player.destY != player.y &&
        borderCollide(player.x, player.destY)) {
        player.destY = player.y;
    }
    /* Move the player toward the destination set by the input */
    if (player.destX != player.x) {
        player.velocityX += player.accelX * dtFrame;
        player.pixelX += player.velocityX * dtFrame;
        /* Make the player appear to hop as it moves */
        player.scale += (fabs(player.velocityX) * 0.005) * dtFrame;
        /* When the tile move is complete, reset and adjust */
        if (player.pixelX >= TILESIZE) {
            player.pixelX = 0;
            ++player.x;
            player.velocityX = 0;
            player.scale = 1.0f;
            initRipple(player.x, player.y);
        } else if (player.pixelX < -TILESIZE) {
            player.pixelX = 0;
            --player.x;
            player.velocityX = 0;
            player.scale = 1.0f;
            initRipple(player.x, player.y);
        }
    /* TODO: Same as above, abstract this stuff */
    } else if (player.destY != player.y) {
        player.velocityY += player.accelY * dtFrame;
        player.pixelY += player.velocityY * dtFrame;
        player.scale += (fabs(player.velocityY) * 0.005) * dtFrame;
        if (player.pixelY >= TILESIZE) {
            player.pixelY = 0;
            ++player.y;
            player.accelX = 0;
            player.velocityY = 0;
            player.scale = 1.0f;
            initRipple(player.x, player.y);
        } else if (player.pixelY < -TILESIZE) {
            player.pixelY = 0;
            --player.y;
            player.accelX = 0;
            player.velocityY = 0;
            player.scale = 1.0f;
            initRipple(player.x, player.y);
        }
    }
}

/*--------------------------------------------------------------------
 * processInput
 *
 * Miscellaneous input handling.
 *--------------------------------------------------------------------*/
void processInput()
{
    if (newInput.key_q) {
        running = 0;
    }
    if (newInput.key_r && !oldInput.key_r) {
        initRipple(10, 10);
    }
}

/*--------------------------------------------------------------------
 * drawPlayer
 *
 * Adjust the player's coordinates so that it is drawn relative to the
 * camera.
 *--------------------------------------------------------------------*/
void drawPlayer()
{
    int centerX = DISPLAY_TW / 2;
    int centerY = DISPLAY_TH / 2;
    int x = (player.x - cam.tileX + centerX) * TILESIZE;
    int y = (player.y - cam.tileY + centerY) * TILESIZE;
    int offsetX = player.pixelX - cam.pixelX;
    int offsetY = player.pixelY - cam.pixelY;
    drawBitmap(player.bitmap, x + offsetX, y + offsetY, player.angle, player.scale);
}

/*--------------------------------------------------------------------
 * main
 *
 * Initialization of objects, main loop, and frame timer.
 *--------------------------------------------------------------------*/
int main()
{
    srand(time(NULL));
    dtFrame = 1.0f / 60.0f;
    cam.tileX = 0;
    cam.tileY = 0;
    cam.destTileX = 0;
    cam.destTileY = 0;
    cam.pixelX = 0;
    cam.pixelY = 0;
    cam.velocityX = 0;
    cam.velocityY = 0;
    player.x = 0;
    player.y = 0;
    player.destX = 0;
    player.destY = 0;
    player.velocityX = 0;
    player.velocityY = 0;
    player.accelX = 0;
    player.accelY = 0;
    player.angle = 0.0f;
    player.oldDirection = 1;
    player.newDirection = 1;
    player.bitmap = loadBitmap("assets/whale.bmp");
    player.scale = 1.0f;
    player.destScale = 1.0f;
    initMap();
    initDisplay();
    bgBufferOld.width = display.width;
    bgBufferOld.height = display.height;
    int dataLen = bgBufferOld.width * bgBufferOld.height;
    bgBufferOld.data = (unsigned int *)calloc(dataLen, sizeof(int));
    bgBufferNew.width = display.width;
    bgBufferNew.height = display.height;
    bgBufferNew.data = (unsigned int *)calloc(dataLen, sizeof(int));
    drawMap();
    struct timespec starttime, endtime;
    const int oneBillion = 1000000000;
    int targettime = dtFrame * oneBillion; /* nanoseconds */
    clock_gettime(CLOCK_REALTIME, &starttime);
    while (running) {
        getInput();
        processInput();
        updatePlayer();
        updateCamera();
        drawBackground();
        animateRipple();
        drawPlayer();
        blitDisplay();
        oldInput = newInput;
        clock_gettime(CLOCK_REALTIME, &endtime);
        int difftime = endtime.tv_nsec - starttime.tv_nsec;
        if (difftime < 0) {
            difftime += oneBillion; /* add bias */
        }
        if (difftime < targettime) {
            usleep((targettime - difftime) / 1000); /* microseconds */
        }
#if 0
        printf("\nBEFORE SLEEP: %f\n", difftime / (float)oneBillion);
        clock_gettime(CLOCK_REALTIME, &endtime);
        difftime = endtime.tv_nsec - starttime.tv_nsec;
        if (difftime < 0) {
            difftime += 1000000000;
        }
        printf("AFTER SLEEP:  %f\n", difftime / (float)oneBillion);
#endif
        clock_gettime(CLOCK_REALTIME, &starttime);
    }
    return 0;
}
