// Phase 3c engine module — APK zip reader, bplist00 parser, multi-texture
// upload through the game's own Java loader (cocojava.loadTexture), and
// sprite-atlas rendering in the game's GL context.
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <android/log.h>
#include <GLES/gl.h>
#include <zlib.h>
#include "engine.h"

#define LOG_TAG "HamboEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// ------------------------------------------------------------------- zip ---
static char gApk[512] = {0};

void engine_set_apk_path(JNIEnv* env, jstring path) {
    const char* p = (*env)->GetStringUTFChars(env, path, NULL);
    if (p) {
        strncpy(gApk, p, sizeof(gApk) - 1);
        (*env)->ReleaseStringUTFChars(env, path, p);
    }
    LOGI("APK path: %s", gApk);
}

static int zip_read(const char* name, unsigned char** out, int* outLen) {
    *out = NULL; *outLen = 0;
    if (!gApk[0]) return 0;
    FILE* f = fopen(gApk, "rb");
    if (!f) { LOGI("apk open failed: %s", gApk); return 0; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    long scan = fsize > 66000 ? 66000 : fsize;
    unsigned char* buf = malloc((size_t)scan);
    fseek(f, fsize - scan, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)scan, f);
    long eocd = -1;
    for (long i = (long)got - 22; i >= 0; i--)
        if (buf[i] == 0x50 && buf[i+1] == 0x4B && buf[i+2] == 0x05 && buf[i+3] == 0x06) {
            eocd = i;                 // buffer-relative index
            break;
        }
    if (eocd < 0) { LOGI("EOCD not found"); free(buf); fclose(f); return 0; }
    unsigned cdCount = buf[eocd+10] | buf[eocd+11]<<8;
    long cdSize = buf[eocd+12] | buf[eocd+13]<<8 | buf[eocd+14]<<16 | (long)buf[eocd+15]<<24;
    long cdOff  = buf[eocd+16] | buf[eocd+17]<<8 | buf[eocd+18]<<16 | (long)buf[eocd+19]<<24;
    LOGI("zip: EOCD ok count=%u cdSize=%ld cdOff=%ld fsize=%ld", cdCount, cdSize, cdOff, fsize);
    free(buf);

    size_t nlen = strlen(name);
    unsigned char* cd = malloc((size_t)cdSize);
    fseek(f, cdOff, SEEK_SET);
    if (fread(cd, 1, (size_t)cdSize, f) != (size_t)cdSize) {
        LOGI("cd read fail"); free(cd); fclose(f); return 0;
    }
    long p = 0; int found = 0;
    for (unsigned i = 0; i < cdCount && p + 46 <= cdSize; i++) {
        if (cd[p] != 0x50 || cd[p+1] != 0x4B || cd[p+2] != 0x01 || cd[p+3] != 0x02) break;
        unsigned method     = cd[p+10] | cd[p+11]<<8;
        unsigned compSize   = cd[p+20] | cd[p+21]<<8 | cd[p+22]<<16 | (unsigned)cd[p+23]<<24;
        unsigned fnLen      = cd[p+28] | cd[p+29]<<8;
        unsigned xLen       = cd[p+30] | cd[p+31]<<8;
        unsigned commentLen = cd[p+32] | cd[p+33]<<8;
        long lho = cd[p+42] | cd[p+43]<<8 | cd[p+44]<<16 | (long)cd[p+45]<<24;
        if (p + 46 + fnLen > cdSize) { LOGI("cd entry %u overruns buffer (p=%ld fnLen=%u cdSize=%ld)", i, p, fnLen, cdSize); break; }
        if (fnLen == nlen && !memcmp(cd + p + 46, name, nlen)) {
            found = 1;
            if (method != 0 && method != 8) { LOGI("zip: unsupported method %u", method); break; }
            unsigned char hdr[30];
            fseek(f, lho, SEEK_SET); fread(hdr, 1, 30, f);
            unsigned lfn = hdr[26] | hdr[27]<<8, lx = hdr[28] | hdr[29]<<8;
            fseek(f, lho + 30 + lfn + lx, SEEK_SET);
            unsigned char* c = malloc((size_t)compSize + 16);
            fread(c, 1, (size_t)compSize, f);
            if (method == 0) {
                *out = c; *outLen = (int)compSize;
                LOGI("zip: %s stored %u bytes", name, compSize);
                free(cd); fclose(f); return 1;
            }
            uLongf dlen = (uLongf)compSize * 8 + 64;
            unsigned char* data = malloc((size_t)dlen);
            z_stream zs; memset(&zs, 0, sizeof zs);
            inflateInit2(&zs, -15);
            zs.next_in = c; zs.avail_in = compSize;
            zs.next_out = data; zs.avail_out = dlen;
            int zr = inflate(&zs, Z_FINISH);
            *outLen = (int)(dlen - zs.avail_out);
            inflateEnd(&zs);
            free(c);
            if (zr == Z_STREAM_END) {
                *out = data;
                LOGI("zip: %s inflated %d bytes", name, *outLen);
            } else {
                LOGI("zip inflate failed zr=%d", zr);
                free(data);
            }
            break;
        }
        p += 46 + fnLen + xLen + commentLen;
    }
    free(cd); fclose(f);
    if (!found) LOGI("zip: %s not in central dir", name);
    return found && *out != NULL;
}

// -------------------------------------------------------------- bplist00 ---
typedef struct { const unsigned char* d; size_t len; unsigned oidSize, intSize;
                 unsigned long numObjects, topIndex, tableOff; } Bp;

static unsigned long be(const unsigned char* b, int n) {
    unsigned long v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | b[i];
    return v;
}
static int bp_init(Bp* b, const unsigned char* d, size_t len) {
    if (len < 40 || memcmp(d, "bplist00", 8)) {
        LOGW("bplist00 init failed (len=%d head=%02x %02x %02x %02x %02x %02x %02x %02x)",
             len, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        return 0;
    }
    const unsigned char* t = d + len - 32;
    b->d = d; b->len = len;
    b->intSize = t[6];            // offsetIntSize
    b->oidSize = t[7];            // objectRefSize
    b->numObjects = be(t+8, 8);   // numObjects
    b->topIndex   = be(t+16, 8);  // topObjectIndex
    b->tableOff   = be(t+24, 8);  // offsetTableOffset
    return b->tableOff < len && b->intSize && b->oidSize && b->numObjects;
}
static const unsigned char* bp_obj(const Bp* b, unsigned long oi) {
    return b->d + be(b->d + b->tableOff + oi * b->intSize, b->intSize);
}
static int bp_charp(const Bp* b, unsigned long oi, char* out, size_t bufsz) {
    const unsigned char* o = bp_obj(b, oi);
    if ((o[0] & 0xf0) != 0x50) return 0;
    unsigned long len = o[0] & 0x0f;
    const unsigned char* data = o + 1;
    if ((o[0] & 0x0f) == 0x0f) {
        int sz = o[1] & 0x0f;
        len = be(o+2, 1 << sz);
        data = o + 2 + (1 << sz);
    }
    size_t n = len < bufsz - 1 ? (size_t)len : bufsz - 1;
    memcpy(out, data, n);
    out[n] = 0;
    return (int)n;
}
static int bp_bool(const Bp* b, unsigned long oi) {
    return bp_obj(b, oi)[0] == 0x09;
}
typedef void (*DictFn)(const Bp* b, unsigned long keyOi, unsigned long valOi, void* ud);
static void bp_iter_dict(const Bp* b, unsigned long oi, DictFn fn, void* ud) {
    const unsigned char* o = bp_obj(b, oi);
    if ((o[0] & 0xf0) != 0xd0) return;
    unsigned long n = o[0] & 0x0f, hdr = 1;
    if ((o[0] & 0x0f) == 0x0f) {
        int sz = o[1] & 0x0f;
        n = be(o+2, 1 << sz);
        hdr = 2 + (1 << sz);
    }
    for (unsigned long i = 0; i < n; i++) {
        unsigned long k = be(o + hdr + i * b->oidSize, b->oidSize);
        unsigned long v = be(o + hdr + n * b->oidSize + i * b->oidSize, b->oidSize);
        fn(b, k, v, ud);
    }
}

// ---------------------------------------------------------------- frames ---
typedef struct { float x, y, w, h; int rotated; char name[64]; } Frame;

#define MAX_ARROW 16
#define MAX_SKIN  9
static Frame gArrow[MAX_ARROW];
static int   gArrowCount = 0;
static Frame gSkin[MAX_SKIN];
static int   gSkinCount = 0;

static const char* kSkinWanted[] = {
    "Kratos_body.png", "Ninja_head.png", "Mummy_head.png", "Ranger_head.png",
    "Primitive_head.png", "Spartan_arm_left.png", "Robopork_head.png",
    "Mohican_body.png", "PixelPig_arm_right.png"
};

static Frame* gStoreBuf;
static int    gStoreCap;
static int*   gStoreCount;
static char   gStoreName[64];
static int    gStoreRotated;

static void entry_cb(const Bp* b, unsigned long keyOi, unsigned long valOi, void* ud) {
    (void)ud;
    char key[64] = {0};
    if (!bp_charp(b, keyOi, key, sizeof key)) return;
    if (!strcmp(key, "textureRotated")) {
        gStoreRotated = bp_bool(b, valOi);
    } else if (!strcmp(key, "textureRect")) {
        char rc[128] = {0};
        if (bp_charp(b, valOi, rc, sizeof rc)) {
            float x, y, w, h;
            if (4 == sscanf(rc, "{{%f, %f}, {%f, %f}}", &x, &y, &w, &h)) {
                if (gStoreRotated) { float t = w; w = h; h = t; }
                if (gStoreCount && *gStoreCount < gStoreCap) {
                    Frame* f = &gStoreBuf[(*gStoreCount)++];
                    memset(f, 0, sizeof *f);
                    f->x = x; f->y = y; f->w = w; f->h = h;
                    f->rotated = gStoreRotated;
                    strncpy(f->name, gStoreName, sizeof f->name - 1);
                    LOGI("frame: %s rect {%.0f,%.0f,%.0fx%.0f} rot=%d",
                         f->name, x, y, f->w, f->h, f->rotated);
                }
            }
        }
    }
}
static int gStoreRotated;
// store targets by name: choose arrow vs skin sheet by frame name
static void sheet_target(const char* fname) {
    if (!strncmp(fname, "Hud_Arrow", 9) || !strncmp(fname, "arrow", 5)) {
        gStoreBuf = gArrow; gStoreCap = MAX_ARROW; gStoreCount = &gArrowCount;
    } else {
        for (unsigned i = 0; i < sizeof kSkinWanted / sizeof *kSkinWanted; i++)
            if (!strcmp(fname, kSkinWanted[i])) {
                gStoreBuf = gSkin; gStoreCap = MAX_SKIN; gStoreCount = &gSkinCount;
                return;
            }
        gStoreBuf = NULL; gStoreCount = NULL;
    }
}
static void frames_cb2(const Bp* b, unsigned long keyOi, unsigned long valOi, void* ud) {
    (void)ud;
    if (!bp_charp(b, keyOi, gStoreName, sizeof gStoreName)) return;
    sheet_target(gStoreName);
    gStoreRotated = 0;
    bp_iter_dict(b, valOi, entry_cb, NULL);
}
static void root_cb2(const Bp* b, unsigned long keyOi, unsigned long valOi, void* ud) {
    (void)ud;
    char key[64] = {0};
    if (!bp_charp(b, keyOi, key, sizeof key)) return;
    if (!strcmp(key, "frames")) {
        bp_iter_dict(b, valOi, frames_cb2, NULL);
        LOGI("sheet parsed: arrow=%d skin=%d", gArrowCount, gSkinCount);
    }
}
static int parse_plist_bytes(const unsigned char* d, int len) {
    Bp bp;
    LOGI("PARSE v3: len=%d head=%c%c%c%c %02x %02x %02x %02x", len,
         len > 8 ? d[0] : '?', len > 8 ? d[1] : '?', len > 8 ? d[2] : '?', len > 8 ? d[3] : '?',
         len > 8 ? d[4] : 0, len > 8 ? d[5] : 0, len > 8 ? d[6] : 0, len > 8 ? d[7] : 0);
    if (!bp_init(&bp, d, (size_t)len)) {
        LOGW("bplist00 init failed (len=%d)", len);
        return 0;
    }
    bp_iter_dict(&bp, bp.topIndex, root_cb2, NULL);
    return gArrowCount + gSkinCount;
}
static void parse_sheet(const char* pngpl) {
    char plist[160];
    const char* dot = strrchr(pngpl, '.');
    if (!dot) return;
    snprintf(plist, sizeof plist, "assets/unpack/%.*s.plist", (int)(dot - pngpl), pngpl);
    unsigned char* data = NULL; int dlen = 0;
    if (!zip_read(plist, &data, &dlen)) return;
    // trim trailing pad/inflate slack: bplist has fixed trailer; parse reads trailer at d+len.
    parse_plist_bytes(data, dlen);
    free(data);
}

// -------------------------------------------------------------- textures ---
typedef struct { GLuint id; int w, h, ready; } Tex;
static Tex gTex[2];               // 0 = Arrow@2x, 1 = HamboSkins1@2x
static char gPending[96] = {0};
static int  gSheetIx;

static int tex_index_for(const char* name) {
    return strstr(name, "HamboSkins") ? 1 : 0;
}

void engine_on_bitmap(JNIEnv* env, jint w, jint h, jbyteArray pix) {
    if (w == 0 || h == 0) {
        *gPending = 0;
        return;
    }
    jbyte* p = (*env)->GetByteArrayElements(env, pix, NULL);
    Tex* t = &gTex[gSheetIx];
    if (t->id == 0) glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
    (*env)->ReleaseByteArrayElements(env, pix, p, JNI_ABORT);
    t->w = w; t->h = h; t->ready = 1;
    LOGI("texture[%d] %s %dx%d ready", gSheetIx, gPending, w, h);
    parse_sheet(gPending);
    *gPending = 0;
}

void engine_on_run(JNIEnv* env, jobject activity) {
    if (!gApk[0]) LOGI("boot: APK path not set yet");
    gArrowCount = 0;
    gSkinCount = 0;
    jclass cj = (*env)->GetObjectClass(env, activity);
    jmethodID m = (*env)->GetStaticMethodID(env, cj, "loadTexture", "(Ljava/lang/String;)V");
    if (!m) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        LOGW("loadTexture not found");
        return;
    }
    const char* sheets[] = { "Arrow@2x.pngpl", "HamboSkins1@2x.pngpl" };
    for (int i = 0; i < 2; i++) {
        strncpy(gPending, sheets[i], sizeof gPending - 1);
        gSheetIx = tex_index_for(sheets[i]);
        jstring s = (*env)->NewStringUTF(env, sheets[i]);
        (*env)->CallStaticVoidMethod(env, cj, m, s);
        (*env)->DeleteLocalRef(env, s);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            LOGW("loadTexture(%s) threw", sheets[i]);
        }
    }
    *gPending = 0;
}

// --------------------------------------------------------------- render ----
static int gInitGl = 0;

static void draw_sprite(const Tex* t, const Frame* f, float cx, float cy, float scale) {
    float w = f->w * scale, h = f->h * scale;
    float u0 = f->x / t->w, v0 = f->y / t->h;
    float u1 = (f->x + f->w) / t->w, v1 = (f->y + f->h) / t->h;
    GLfloat quad[8] = {
        cx - w/2, cy - h/2,
        cx + w/2, cy - h/2,
        cx - w/2, cy + h/2,
        cx + w/2, cy + h/2
    };
    GLfloat uv[8] = { u0, v1, u1, v1, u0, v0, u1, v0 };
    glBindTexture(GL_TEXTURE_2D, t->id);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, quad);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

void engine_render(void) {
    if (!gInitGl) {
        gInitGl = 1;
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(1, 1, 1, 1);
    }
    glClearColor(0.08f, 0.10f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    extern int gPW, gPH;
    glOrthof(-gPW * 0.5f, gPW * 0.5f, -gPH * 0.5f, gPH * 0.5f, 0.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_TEXTURE_2D);

    // Row A: arrow sheet (top), Row B: skin parts (bottom)
    int arrowVisible = gArrowCount > 0 && gTex[0].ready;
    int skinVisible  = gSkinCount  > 0 && gTex[1].ready;
    if (arrowVisible) {
        float scale = gPW * 0.86f;
        float total = 1;
        for (int i = 0; i < gArrowCount; i++) total += gArrow[i].w;
        scale /= total;
        float hmax = 1;
        for (int i = 0; i < gArrowCount; i++) if (gArrow[i].h > hmax) hmax = gArrow[i].h;
        scale = scale < (gPH * 0.18f) / hmax ? scale : (gPH * 0.18f) / hmax;
        float x = -gPW * 0.43f;
        for (int i = 0; i < gArrowCount; i++) {
            x += gArrow[i].w * scale * 0.5f;
            draw_sprite(&gTex[0], &gArrow[i], x, gPH * 0.22f, scale);
            x += gArrow[i].w * scale * 0.5f + gPW * 0.01f;
        }
    }
    if (skinVisible) {
        float scale = gPW * 0.8f;
        float total = 1;
        for (int i = 0; i < gSkinCount; i++) total += gSkin[i].w;
        scale /= total;
        float hmax = 1;
        for (int i = 0; i < gSkinCount; i++) if (gSkin[i].h > hmax) hmax = gSkin[i].h;
        scale = scale < (gPH * 0.22f) / hmax ? scale : (gPH * 0.22f) / hmax;
        float x = -gPW * 0.40f;
        for (int i = 0; i < gSkinCount; i++) {
            x += gSkin[i].w * scale * 0.5f;
            draw_sprite(&gTex[1], &gSkin[i], x, -gPH * 0.22f, scale);
            x += gSkin[i].w * scale * 0.5f + gPW * 0.02f;
        }
    }
    glDisable(GL_TEXTURE_2D);
}
