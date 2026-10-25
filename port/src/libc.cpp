#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <zlib.h>

#include <cerrno>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core.h"

extern GuestLib g_lib;
void init_pthread();

// printf with guest argument layout: long is 32-bit, doubles/long long are 8-byte aligned.
static std::string gformat(const char* f, VaReader va) {
    if (!f) return "";
    std::string out;
    char buf[512], spec[64];
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        const char* start = f++;
        if (*f == '%') { out += '%'; f++; continue; }
        std::string s = "%";
        while (*f && strchr("-+ #0'", *f)) s += *f++;
        auto num = [&] {
            if (*f == '*') { f++; s += std::to_string(static_cast<s32>(va.w())); }
            else while (isdigit(static_cast<unsigned char>(*f))) s += *f++;
        };
        num();
        if (*f == '.') { s += *f++; num(); }
        int longs = 0;
        while (*f && strchr("hlLqjzt", *f)) {
            if (*f == 'l' || *f == 'q' || *f == 'L') longs++;
            if (*f == 'j') longs = 2;
            f++;
        }
        char conv = *f ? *f++ : 0;
        switch (conv) {
            case 'd': case 'i':
                if (longs >= 2) snprintf(buf, sizeof buf, (s + "lld").c_str(), static_cast<long long>(va.dw()));
                else snprintf(buf, sizeof buf, (s + conv).c_str(), static_cast<s32>(va.w()));
                break;
            case 'u': case 'o': case 'x': case 'X':
                if (longs >= 2) snprintf(buf, sizeof buf, (s + "ll" + conv).c_str(), static_cast<unsigned long long>(va.dw()));
                else snprintf(buf, sizeof buf, (s + conv).c_str(), va.w());
                break;
            case 'c': snprintf(buf, sizeof buf, (s + 'c').c_str(), static_cast<int>(va.w())); break;
            case 'p': snprintf(buf, sizeof buf, "0x%x", va.w()); break;
            case 's': {
                u32 p = va.w();
                const char* str = p ? mem::h<char>(p) : "(null)";
                int n = snprintf(nullptr, 0, (s + 's').c_str(), str);
                std::string tmp(n + 1, '\0');
                snprintf(tmp.data(), n + 1, (s + 's').c_str(), str);
                tmp.resize(n);
                out += tmp;
                continue;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                u64 b = va.dw();
                double d;
                memcpy(&d, &b, 8);
                snprintf(buf, sizeof buf, (s + conv).c_str(), d);
                break;
            }
            case 'n': mem::wr32(va.w(), static_cast<u32>(out.size())); continue;
            default:
                snprintf(spec, sizeof spec, "%.*s", static_cast<int>(f - start), start);
                out += spec;
                continue;
        }
        out += buf;
    }
    return out;
}

static int gscan(const char* in, FILE* fp, const char* f, VaReader va) {
    int assigned = 0, pos = 0;
    std::string lit;
    while (*f) {
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') { lit += "%%"; f += 2; } else lit += *f++;
            continue;
        }
        const char* start = f++;
        bool suppress = *f == '*';
        if (suppress) f++;
        std::string width;
        while (isdigit(static_cast<unsigned char>(*f))) width += *f++;
        int longs = 0, halfs = 0;
        while (*f && strchr("hlLqjzt", *f)) {
            if (*f == 'h') halfs++;
            if (*f == 'l' || *f == 'q' || *f == 'L') longs++;
            if (*f == 'j') longs = 2;
            f++;
        }
        std::string set;
        char conv = *f++;
        if (conv == '[') {
            set = "[";
            if (*f == '^') set += *f++;
            if (*f == ']') set += *f++;
            while (*f && *f != ']') set += *f++;
            if (*f) set += *f++;
        }
        std::string sp = lit + "%" + (suppress ? "*" : "") + width;
        lit.clear();
        int n = -1, r;
        auto run = [&](const std::string& fmt, void* dst) {
            std::string full = fmt + "%n";
            if (fp) return suppress ? fscanf(fp, full.c_str(), &n) : fscanf(fp, full.c_str(), dst, &n);
            return suppress ? sscanf(in + pos, full.c_str(), &n) : sscanf(in + pos, full.c_str(), dst, &n);
        };
        u32 dst;
        switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
                long long v = 0;
                r = run(sp + "ll" + (conv == 'p' ? 'x' : conv), &v);
                if (!suppress && r >= 1) {
                    dst = va.w();
                    int size = longs >= 2 ? 8 : halfs >= 2 ? 1 : halfs ? 2 : 4;
                    memcpy(mem::h(dst), &v, size);
                }
                break;
            }
            case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
                double v = 0;
                r = run(sp + "lf", &v);
                if (!suppress && r >= 1) {
                    dst = va.w();
                    if (longs) memcpy(mem::h(dst), &v, 8);
                    else { float fv = static_cast<float>(v); memcpy(mem::h(dst), &fv, 4); }
                }
                break;
            }
            case 's': case 'c': case '[': {
                void* target = suppress ? nullptr : mem::h(va.w());
                r = run(sp + (conv == '[' ? set : std::string(1, conv)), target);
                break;
            }
            case 'n':
                if (!suppress) mem::wr32(va.w(), pos);
                continue;
            default:
                LOGW("scanf: unsupported conversion %.*s", static_cast<int>(f - start), start);
                return assigned;
        }
        if (n < 0) return assigned ? assigned : (r == EOF ? EOF : 0);
        if (!suppress) assigned++;
        pos += n;
    }
    return assigned;
}

// ---------------------------------------------------------------- FILE / DIR / ZLIB / MMAP handles
namespace {
constexpr u32 kFileSize = 84;  // sizeof(FILE) in old 32-bit bionic, for &__sF[i]
u32 g_sF = 0;
std::mutex files_lock;
std::unordered_map<u32, FILE*> files;
std::unordered_map<u32, DIR*> dirs;
std::unordered_map<u32, u32> dirents;
std::unordered_map<u32, gzFile> gzfiles;

std::mutex z_lock;
std::unordered_map<u32, z_stream*> zstreams;

std::mutex mmap_lock;
std::unordered_set<u32> mmap_allocs;

FILE* hfile(u32 g) {
    std::lock_guard lk(files_lock);
    auto it = files.find(g);
    if (it == files.end()) fatal("bad guest FILE* %08x", g);
    return it->second;
}

int log_write(void*, const char* buf, int n) {
    __android_log_print(ANDROID_LOG_ERROR, "HamboGuest", "%.*s", n, buf);
    return n;
}

u32 per_thread_buf(size_t n) {
    thread_local u32 b = 0;
    if (!b) b = mem::g(mem::alloc(4096));
    if (n > 4096) fatal("per-thread buffer too small");
    return b;
}

void tm_to_guest(const tm& t, u32 g) {
    s32 v[11] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday,
                 t.tm_isdst, static_cast<s32>(t.tm_gmtoff), 0};
    memcpy(mem::h(g), v, 44);
}
tm tm_from_guest(u32 g) {
    s32 v[11];
    memcpy(v, mem::h(g), 44);
    tm t{};
    t.tm_sec = v[0]; t.tm_min = v[1]; t.tm_hour = v[2]; t.tm_mday = v[3]; t.tm_mon = v[4];
    t.tm_year = v[5]; t.tm_wday = v[6]; t.tm_yday = v[7]; t.tm_isdst = v[8]; t.tm_gmtoff = v[9];
    return t;
}

void stat_to_guest(const struct stat& s, u32 g) {
    u8* p = mem::h<u8>(g);
    memset(p, 0, 104);
    auto put32 = [&](int off, u32 v) { memcpy(p + off, &v, 4); };
    auto put64 = [&](int off, u64 v) { memcpy(p + off, &v, 8); };
    put64(0, s.st_dev);
    put32(12, static_cast<u32>(s.st_ino));
    put32(16, s.st_mode);
    put32(20, s.st_nlink);
    put32(24, s.st_uid);
    put32(28, s.st_gid);
    put64(32, s.st_rdev);
    put64(48, s.st_size);
    put32(56, s.st_blksize);
    put64(64, s.st_blocks);
    put32(72, s.st_atim.tv_sec); put32(76, s.st_atim.tv_nsec);
    put32(80, s.st_mtim.tv_sec); put32(84, s.st_mtim.tv_nsec);
    put32(88, s.st_ctim.tv_sec); put32(92, s.st_ctim.tv_nsec);
    put64(96, s.st_ino);
}

int open_flags(u32 f) {
#if defined(__x86_64__)
    int h = f & ~0740000;
    if (f & 040000) h |= O_DIRECTORY;
    if (f & 0100000) h |= O_NOFOLLOW;
    if (f & 0200000) h |= O_DIRECT;
    return h;
#else
    return f;
#endif
}

timeval tv_from_guest(u32 g) { return {static_cast<s32>(mem::rd32(g)), static_cast<s32>(mem::rd32(g + 4))}; }
void tv_to_guest(const timeval& t, u32 g) { mem::wr32(g, t.tv_sec); mem::wr32(g + 4, t.tv_usec); }

std::recursive_mutex guard_lock;
thread_local u32 qsort_cmp;
int qsort_tramp(const void* a, const void* b) { return static_cast<s32>(call_guest(qsort_cmp, {mem::g(a), mem::g(b)})); }

u32 addrinfo_to_guest(const addrinfo* a) {
    u32 head = 0, *link = &head;
    for (; a; a = a->ai_next) {
        u32 g = mem::g(mem::calloc(1, 32));
        u32 sa = mem::g(mem::alloc(a->ai_addrlen));
        memcpy(mem::h(sa), a->ai_addr, a->ai_addrlen);
        u32 v[8] = {static_cast<u32>(a->ai_flags), static_cast<u32>(a->ai_family), static_cast<u32>(a->ai_socktype),
                    static_cast<u32>(a->ai_protocol), a->ai_addrlen,
                    a->ai_canonname ? mem::strdup(a->ai_canonname) : 0, sa, 0};
        memcpy(mem::h(g), v, 32);
        *link = g;
        link = mem::h<u32>(g + 28);
    }
    return head;
}

u32 hostent_to_guest(const hostent* he) {
    thread_local u32 g = 0;
    if (!he) return 0;
    if (!g) g = mem::g(mem::calloc(1, 20 + 64 * 4 + 64 * 16));
    u32 lists = g + 20, data = lists + 64 * 4, li = 0, d = data;
    auto add_list = [&](char** l, int len) {
        u32 start = lists + li * 4;
        for (int i = 0; l && l[i] && li < 60; i++) {
            u32 n = len ? len : strlen(l[i]) + 1;
            if (d + n > data + 64 * 16) break;
            memcpy(mem::h(d), l[i], n);
            mem::wr32(lists + 4 * li++, d);
            d += (n + 3) & ~3u;
        }
        mem::wr32(lists + 4 * li++, 0);
        return start;
    };
    u32 name = mem::strdup(he->h_name);
    u32 aliases = add_list(he->h_aliases, 0);
    u32 addrs = add_list(he->h_addr_list, he->h_length);
    u32 v[5] = {name, aliases, static_cast<u32>(he->h_addrtype), static_cast<u32>(he->h_length), addrs};
    memcpy(mem::h(g), v, 20);
    return g;
}

// Guest 32-bit z_stream layout (56 bytes):
// 0: next_in, 4: avail_in, 8: total_in, 12: next_out, 16: avail_out, 20: total_out,
// 24: msg, 28: state, 32: zalloc, 36: zfree, 40: opaque, 44: data_type, 48: adler, 52: reserved
void z_from_guest(u32 g, z_stream* h) {
    u32 nin = mem::rd32(g + 0);
    h->next_in = nin ? mem::h<Bytef>(nin) : nullptr;
    h->avail_in = mem::rd32(g + 4);
    u32 nout = mem::rd32(g + 12);
    h->next_out = nout ? mem::h<Bytef>(nout) : nullptr;
    h->avail_out = mem::rd32(g + 16);
}
void z_to_guest(z_stream* h, u32 g) {
    mem::wr32(g + 0, h->next_in ? mem::g(h->next_in) : 0);
    mem::wr32(g + 4, h->avail_in);
    mem::wr32(g + 8, static_cast<u32>(h->total_in));
    mem::wr32(g + 12, h->next_out ? mem::g(h->next_out) : 0);
    mem::wr32(g + 16, h->avail_out);
    mem::wr32(g + 20, static_cast<u32>(h->total_out));
    mem::wr32(g + 44, h->data_type);
    mem::wr32(g + 48, static_cast<u32>(h->adler));
}
z_stream* get_zstream(u32 g, bool create = false) {
    std::lock_guard lk(z_lock);
    auto it = zstreams.find(g);
    if (it != zstreams.end()) return it->second;
    if (!create) fatal("unknown guest z_stream %08x", g);
    auto* zs = new z_stream{};
    zstreams[g] = zs;
    return zs;
}
void drop_zstream(u32 g) {
    std::lock_guard lk(z_lock);
    auto it = zstreams.find(g);
    if (it != zstreams.end()) {
        delete it->second;
        zstreams.erase(it);
    }
}
}  // namespace

// ---------------------------------------------------------------- registration
void init_libc() {
    init_pthread();

    // --- data symbols
    g_sF = mem::g(mem::calloc(3, kFileSize));
    files[g_sF] = stdin;
    files[g_sF + kFileSize] = files[g_sF + 2 * kFileSize] = funopen(nullptr, nullptr, log_write, nullptr, nullptr);
    setvbuf(files[g_sF + kFileSize], nullptr, _IOLBF, 0);
    register_data("__sF", g_sF);
    u32 guard = mem::g(mem::calloc(1, 4));
    mem::wr32(guard, 0x5f3759df);
    register_data("__stack_chk_guard", guard);
    register_data("__dso_handle", mem::g(mem::calloc(1, 4)));

    u32 page_sz = mem::g(mem::calloc(1, 4));
    mem::wr32(page_sz, 4096);
    register_data("__page_size", page_sz);

    u32 env_var = mem::g(mem::calloc(2, 4));
    mem::wr32(env_var, env_var + 4);  // points to NULL char*
    register_data("environ", env_var);

    {
        u8* t = static_cast<u8*>(mem::calloc(1, 257));
        auto* lo = static_cast<s16*>(mem::calloc(257, 2));
        auto* up = static_cast<s16*>(mem::calloc(257, 2));
        lo[0] = up[0] = -1;
        for (int c = 0; c < 256; c++) {
            lo[c + 1] = c < 128 ? tolower(c) : c;
            up[c + 1] = c < 128 ? toupper(c) : c;
            if (c >= 128) continue;
            u8 fl = 0;
            if (isupper(c)) fl |= 0x01;
            if (islower(c)) fl |= 0x02;
            if (isdigit(c)) fl |= 0x04;
            if (isspace(c)) fl |= 0x08;
            if (ispunct(c)) fl |= 0x10;
            if (iscntrl(c)) fl |= 0x20;
            if (isxdigit(c) && !isdigit(c)) fl |= 0x40;
            if (c == ' ') fl |= 0x80;
            t[c + 1] = fl;
        }
        auto ptr_var = [](void* table) {
            u32 v = mem::g(mem::alloc(4));
            mem::wr32(v, mem::g(table));
            return v;
        };
        register_data("_ctype_", ptr_var(t));
        register_data("_tolower_tab_", ptr_var(lo));
        register_data("_toupper_tab_", ptr_var(up));
    }

    // --- memory / strings
    register_import("malloc", [](Ctx& c) {
        if (c.arg(0) >= (256u << 20)) LOGW("guest malloc(%u MB) from lr=%08x", c.arg(0) >> 20, c.r[14] - g_lib.base);
        c.ret(mem::g(mem::alloc(c.arg(0))));
    });
    register_import("calloc", [](Ctx& c) {
        if (u64(c.arg(0)) * c.arg(1) >= (256u << 20)) LOGW("guest calloc(%u x %u) from lr=%08x", c.arg(0), c.arg(1), c.r[14] - g_lib.base);
        c.ret(mem::g(mem::calloc(c.arg(0), c.arg(1))));
    });
    register_import("realloc", [](Ctx& c) {
        if (c.arg(1) >= (256u << 20)) LOGW("guest realloc(%u MB) from lr=%08x", c.arg(1) >> 20, c.r[14] - g_lib.base);
        c.ret(mem::g(mem::realloc(mem::h(c.arg(0)), c.arg(1))));
    });
    register_import("free", [](Ctx& c) { mem::free(mem::h(c.arg(0))); });
    register_import("bzero", [](Ctx& c) { memset(mem::h(c.arg(0)), 0, c.arg(1)); });
    register_import("strdup", [](Ctx& c) { c.ret(mem::strdup(mem::h<char>(c.arg(0)))); });

    IMPORT(memcpy); IMPORT(memmove); IMPORT(memset); IMPORT(memcmp);
    IMPORT_T(memchr, void*(const void*, int, size_t));
    IMPORT(strlen); IMPORT(strcmp); IMPORT(strncmp); IMPORT(strcpy); IMPORT(strncpy);
    IMPORT(strcat); IMPORT(strncat); IMPORT(strcoll); IMPORT(strxfrm);
    IMPORT(strspn); IMPORT(strcspn); IMPORT(strcasecmp); IMPORT(strncasecmp); IMPORT(wcslen);
    IMPORT_T(strchr, char*(const char*, int));
    IMPORT_T(strrchr, char*(const char*, int));
    IMPORT_T(strstr, char*(const char*, const char*));
    IMPORT(tolower); IMPORT(toupper); IMPORT(isdigit);

    register_import("strtok", [](Ctx& c) {
        static thread_local u32 save = 0;
        char* s = c.arg(0) ? mem::h<char>(c.arg(0)) : nullptr;
        char* sp = save ? mem::h<char>(save) : nullptr;
        char* r = strtok_r(s, mem::h<char>(c.arg(1)), &sp);
        save = sp ? mem::g(sp) : 0;
        c.ret(r ? mem::g(r) : 0);
    });
    register_import("strtok_r", [](Ctx& c) {
        char* s = c.arg(0) ? mem::h<char>(c.arg(0)) : nullptr;
        u32 sp_g = mem::rd32(c.arg(2));
        char* sp = sp_g ? mem::h<char>(sp_g) : nullptr;
        char* r = strtok_r(s, mem::h<char>(c.arg(1)), &sp);
        mem::wr32(c.arg(2), sp ? mem::g(sp) : 0);
        c.ret(r ? mem::g(r) : 0);
    });

    IMPORT(atoi); IMPORT(atoll);
    register_import("strtod", [](Ctx& c) {
        char* end;
        double d = strtod(mem::h<char>(c.arg(0)), &end);
        if (c.arg(1)) mem::wr32(c.arg(1), mem::g(end));
        write_ret(c, d);
    });
    register_import("strtol", [](Ctx& c) {
        char* end;
        long long v = strtoll(mem::h<char>(c.arg(0)), &end, c.arg(2));
        if (c.arg(1)) mem::wr32(c.arg(1), mem::g(end));
        c.ret(static_cast<u32>(static_cast<s32>(v)));
    });
    register_import("strtoll", [](Ctx& c) {
        char* end;
        long long v = strtoll(mem::h<char>(c.arg(0)), &end, c.arg(2));
        if (c.arg(1)) mem::wr32(c.arg(1), mem::g(end));
        c.ret64(static_cast<u64>(v));
    });
    register_import("strtoul", [](Ctx& c) {
        char* end;
        unsigned long long v = strtoull(mem::h<char>(c.arg(0)), &end, c.arg(2));
        if (c.arg(1)) mem::wr32(c.arg(1), mem::g(end));
        c.ret(static_cast<u32>(v));
    });
    register_import("strtoull", [](Ctx& c) {
        char* end;
        unsigned long long v = strtoull(mem::h<char>(c.arg(0)), &end, c.arg(2));
        if (c.arg(1)) mem::wr32(c.arg(1), mem::g(end));
        c.ret64(v);
    });
    register_import("qsort", [](Ctx& c) {
        u32 saved = qsort_cmp;
        qsort_cmp = c.arg(3);
        qsort(mem::h(c.arg(0)), c.arg(1), c.arg(2), qsort_tramp);
        qsort_cmp = saved;
    }, true);
    register_import("bsearch", [](Ctx& c) {
        u32 saved = qsort_cmp;
        qsort_cmp = c.arg(4);
        void* r = bsearch(mem::h(c.arg(0)), mem::h(c.arg(1)), c.arg(2), c.arg(3), qsort_tramp);
        qsort_cmp = saved;
        c.ret(r ? mem::g(r) : 0);
    }, true);
    IMPORT(lrand48); IMPORT(srand48); IMPORT(arc4random);

    // --- math
    IMPORT_T(acos, double(double)); IMPORT_T(atan, double(double)); IMPORT_T(ceil, double(double));
    IMPORT_T(cos, double(double)); IMPORT_T(sin, double(double));
    IMPORT_T(exp, double(double)); IMPORT_T(floor, double(double)); IMPORT_T(log, double(double));
    IMPORT_T(log10, double(double)); IMPORT_T(sqrt, double(double)); IMPORT_T(fmod, double(double, double));
    IMPORT_T(pow, double(double, double)); IMPORT_T(ldexp, double(double, int)); IMPORT_T(frexp, double(double, int*));
    IMPORT(acosf); IMPORT(asinf); IMPORT(atan2f); IMPORT(atanf); IMPORT(ceilf); IMPORT(cosf); IMPORT(expf);
    IMPORT(floorf); IMPORT(fmaxf); IMPORT(fmodf); IMPORT(log10f); IMPORT(logf); IMPORT(modff);
    IMPORT(powf); IMPORT(roundf); IMPORT(sinf); IMPORT(sqrtf); IMPORT(tanf);

    register_import("__isfinitef", [](Ctx& c) {
        float f; u32 b = c.arg(0); memcpy(&f, &b, 4);
        c.ret(std::isfinite(f));
    });
    register_import("__fpclassifyd", [](Ctx& c) {
        ArgReader ar{c};
        double d = read_arg<double>(ar);
        c.ret(std::fpclassify(d));
    });
    register_import("__signbit", [](Ctx& c) {
        ArgReader ar{c};
        double d = read_arg<double>(ar);
        c.ret(std::signbit(d) ? 1 : 0);
    });

    // --- setjmp / longjmp (entirely in guest CPU state; thunk executes `bx lr` upon return)
    auto do_setjmp = [](Ctx& c) {
        u32 buf = c.arg(0);
        for (int i = 0; i < 8; i++) mem::wr32(buf + i * 4, c.r[4 + i]);
        mem::wr32(buf + 32, c.r[13]);  // sp
        mem::wr32(buf + 36, c.r[14]);  // lr
        c.ret(0);
    };
    auto do_longjmp = [](Ctx& c) {
        u32 buf = c.arg(0);
        u32 val = c.arg(1);
        for (int i = 0; i < 8; i++) c.r[4 + i] = mem::rd32(buf + i * 4);
        c.r[13] = mem::rd32(buf + 32);
        c.r[14] = mem::rd32(buf + 36);
        c.ret(val ? val : 1);
    };
    register_import("setjmp", do_setjmp);
    register_import("sigsetjmp", do_setjmp);
    register_import("longjmp", do_longjmp);
    register_import("siglongjmp", do_longjmp);

    // --- process / misc / dlfcn / unwind
    register_import("abort", [](Ctx& c) { fatal("guest abort() lr=%08x (off=%08x)", c.r[14], c.r[14] - g_lib.base); });
    register_import("exit", [](Ctx& c) { LOGI("guest exit(%d)", c.arg(0)); exit(c.arg(0)); });
    register_import("atexit", [](Ctx& c) { c.ret(0); });
    register_import("__aeabi_atexit", [](Ctx& c) { c.ret(0); });
    register_import("__cxa_atexit", [](Ctx& c) { c.ret(0); });
    register_import("__cxa_finalize", [](Ctx& c) { c.ret(0); });
    register_import("__stack_chk_fail", [](Ctx& c) { fatal("guest stack smashing detected lr=%08x", c.r[14]); });
    register_import("__cxa_guard_acquire", [](Ctx& c) {
        if (mem::rd32(c.arg(0)) & 1) return c.ret(0);
        guard_lock.lock();
        if (mem::rd32(c.arg(0)) & 1) { guard_lock.unlock(); return c.ret(0); }
        c.ret(1);
    });
    register_import("__cxa_guard_release", [](Ctx& c) {
        mem::wr32(c.arg(0), 1);
        guard_lock.unlock();
    });
    register_import("__errno", [](Ctx& c) { c.ret(guest_errno_addr()); });
    register_import("__gnu_Unwind_Find_exidx", [](Ctx& c) {
        mem::wr32(c.arg(1), g_lib.exidx_count);
        c.ret(g_lib.exidx);
    });
    for (const char* uw : {"_Unwind_Complete", "_Unwind_DeleteException", "_Unwind_GetDataRelBase",
                           "_Unwind_GetLanguageSpecificData", "_Unwind_GetRegionStart", "_Unwind_GetTextRelBase",
                           "_Unwind_RaiseException", "_Unwind_Resume", "_Unwind_Resume_or_Rethrow",
                           "_Unwind_VRS_Get", "_Unwind_VRS_Set", "__aeabi_unwind_cpp_pr0",
                           "__aeabi_unwind_cpp_pr1", "__gnu_unwind_frame"}) {
        register_import(uw, [](Ctx& c) {
            LOGE("guest called unwind routine from lr=%08x (off=%08x)", c.r[14], c.r[14] - g_lib.base);
            c.ret(0);
        });
    }

    register_import("dlopen", [](Ctx& c) {
        const char* p = c.arg(0) ? mem::h<char>(c.arg(0)) : "NULL";
        LOGI("guest dlopen(%s) -> 0xdead0001", p);
        c.ret(0xdead0001u);
    });
    register_import("dlsym", [](Ctx& c) {
        const char* name = c.arg(1) ? mem::h<char>(c.arg(1)) : "";
        u32 a = g_lib.sym(name);
        if (!a) a = import_address(name);
        if (!a) LOGW("guest dlsym(%s) -> 0", name);
        c.ret(a);
    });
    register_import("dlclose", [](Ctx& c) { c.ret(0); });
    register_import("dlerror", [](Ctx& c) { c.ret(0); });

    IMPORT(getpid); IMPORT(getuid); IMPORT(geteuid);
    IMPORT(sched_yield); IMPORT(usleep); IMPORT(sleep); IMPORT(alarm); IMPORT(pause);
    register_import("bsd_signal", [](Ctx& c) { c.ret(0); });
    register_import("sigaction", [](Ctx& c) { c.ret(0); });
    register_import("kill", [](Ctx& c) { c.ret(0); });
    register_import("fork", [](Ctx& c) { c.ret(static_cast<u32>(-1)); });
    register_import("execve", [](Ctx& c) { c.ret(static_cast<u32>(-1)); });
    register_import("__wait4", [](Ctx& c) { c.ret(static_cast<u32>(-1)); });
    register_import("getpwuid", [](Ctx& c) { c.ret(0); });
    register_import("getgrgid", [](Ctx& c) { c.ret(0); });

    register_import("getenv", [](Ctx& c) {
        const char* name = c.arg(0) ? mem::h<char>(c.arg(0)) : "";
        const char* v = getenv(name);
        c.ret(v ? mem::strdup(v) : 0);
    });
    register_import("setlocale", [](Ctx& c) {
        static u32 loc_c = 0;
        if (!loc_c) loc_c = mem::strdup("C");
        c.ret(loc_c);
    });
    register_import("strerror", [](Ctx& c) {
        u32 b = per_thread_buf(256);
        strncpy(mem::h<char>(b), strerror(c.arg(0)), 255);
        c.ret(b);
    });
    register_import("strerror_r", [](Ctx& c) {
        if (c.arg(1) && c.arg(2)) {
            strncpy(mem::h<char>(c.arg(1)), strerror(c.arg(0)), c.arg(2) - 1);
            mem::h<char>(c.arg(1))[c.arg(2) - 1] = 0;
        }
        c.ret(0);
    });
    register_import("gai_strerror", [](Ctx& c) {
        u32 b = per_thread_buf(256);
        strncpy(mem::h<char>(b), gai_strerror(c.arg(0)), 255);
        c.ret(b);
    });
    register_import("basename", [](Ctx& c) {
        u32 b = per_thread_buf(512);
        strncpy(mem::h<char>(b), basename(mem::h<char>(c.arg(0))), 511);
        c.ret(b);
    });
    register_import("perror", [](Ctx& c) {
        LOGW("guest perror(%s): %s", c.arg(0) ? mem::h<char>(c.arg(0)) : "", strerror(errno));
    });
    register_import("uname", [](Ctx& c) {
        c.ret(uname(mem::h<utsname>(c.arg(0))));
    });

    // --- mmap / munmap inside low-4GB guest arena
    register_import("mmap", [](Ctx& c) {
        u32 len = c.arg(1);
        u32 flags = c.arg(3);
        s32 fd = static_cast<s32>(c.arg(4));
        s32 off = static_cast<s32>(c.arg(5));
        if (!len) return c.ret(0xFFFFFFFFu);
        void* p = mem::alloc_aligned(4096, len);
        if (!p) return c.ret(0xFFFFFFFFu);
        memset(p, 0, len);
        if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
            ssize_t r = pread(fd, p, len, off);
            if (r < 0) {
                mem::free(p);
                return c.ret(0xFFFFFFFFu);
            }
        }
        u32 g = mem::g(p);
        {
            std::lock_guard lk(mmap_lock);
            mmap_allocs.insert(g);
        }
        c.ret(g);
    });
    register_import("munmap", [](Ctx& c) {
        u32 g = c.arg(0);
        bool found = false;
        {
            std::lock_guard lk(mmap_lock);
            auto it = mmap_allocs.find(g);
            if (it != mmap_allocs.end()) {
                mmap_allocs.erase(it);
                found = true;
            }
        }
        if (found) mem::free(mem::h(g));
        c.ret(0);
    });

    // --- zlib
    register_import("zlibVersion", [](Ctx& c) {
        static u32 v = 0;
        if (!v) v = mem::strdup(zlibVersion());
        c.ret(v);
    });
    register_import("zError", [](Ctx& c) {
        const char* s = zError(c.arg(0));
        c.ret(s ? mem::strdup(s) : 0);
    });
    register_import("crc32", [](Ctx& c) {
        c.ret(static_cast<u32>(crc32(c.arg(0), c.arg(1) ? mem::h<Bytef>(c.arg(1)) : nullptr, c.arg(2))));
    });
    register_import("get_crc_table", [](Ctx& c) {
        static u32 tbl = 0;
        if (!tbl) {
            const z_crc_t* h = get_crc_table();
            u32* g = static_cast<u32*>(mem::alloc(256 * 4));
            for (int i = 0; i < 256; i++) g[i] = static_cast<u32>(h[i]);
            tbl = mem::g(g);
        }
        c.ret(tbl);
    });
    register_import("compress", [](Ctx& c) {
        uLongf n = mem::rd32(c.arg(1));
        int r = compress(mem::h<Bytef>(c.arg(0)), &n, mem::h<Bytef>(c.arg(2)), c.arg(3));
        mem::wr32(c.arg(1), static_cast<u32>(n));
        c.ret(r);
    });
    register_import("uncompress", [](Ctx& c) {
        uLongf n = mem::rd32(c.arg(1));
        int r = uncompress(mem::h<Bytef>(c.arg(0)), &n, mem::h<Bytef>(c.arg(2)), c.arg(3));
        mem::wr32(c.arg(1), static_cast<u32>(n));
        c.ret(r);
    });
    register_import("inflateInit_", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g, true);
        z_from_guest(g, h);
        int r = inflateInit_(h, zlibVersion(), sizeof(z_stream));
        z_to_guest(h, g);
        c.ret(r);
    });
    register_import("inflateInit2_", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g, true);
        z_from_guest(g, h);
        int r = inflateInit2_(h, static_cast<s32>(c.arg(1)), zlibVersion(), sizeof(z_stream));
        z_to_guest(h, g);
        c.ret(r);
    });
    register_import("inflate", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g);
        z_from_guest(g, h);
        int r = inflate(h, c.arg(1));
        z_to_guest(h, g);
        c.ret(r);
    });
    register_import("inflateEnd", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g);
        int r = inflateEnd(h);
        drop_zstream(g);
        c.ret(r);
    });
    register_import("inflateReset", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g);
        z_from_guest(g, h);
        int r = inflateReset(h);
        z_to_guest(h, g);
        c.ret(r);
    });
    register_import("deflateInit_", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g, true);
        z_from_guest(g, h);
        int r = deflateInit_(h, static_cast<s32>(c.arg(1)), zlibVersion(), sizeof(z_stream));
        z_to_guest(h, g);
        c.ret(r);
    });
    register_import("deflateInit2_", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g, true);
        z_from_guest(g, h);
        int r = deflateInit2_(h, static_cast<s32>(c.arg(1)), static_cast<s32>(c.arg(2)),
                              static_cast<s32>(c.arg(3)), static_cast<s32>(c.arg(4)),
                              static_cast<s32>(c.arg(5)), zlibVersion(), sizeof(z_stream));
        z_to_guest(h, g);
        c.ret(r);
    });
    register_import("deflate", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g);
        z_from_guest(g, h);
        int r = deflate(h, c.arg(1));
        z_to_guest(h, g);
        c.ret(r);
    });
    register_import("deflateEnd", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g);
        int r = deflateEnd(h);
        drop_zstream(g);
        c.ret(r);
    });
    register_import("deflateReset", [](Ctx& c) {
        u32 g = c.arg(0);
        z_stream* h = get_zstream(g);
        z_from_guest(g, h);
        int r = deflateReset(h);
        z_to_guest(h, g);
        c.ret(r);
    });
    register_import("gzopen", [](Ctx& c) {
        gzFile gz = gzopen(mem::h<char>(c.arg(0)), mem::h<char>(c.arg(1)));
        if (!gz) return c.ret(0);
        u32 g = mem::g(mem::calloc(1, 16));
        std::lock_guard lk(files_lock);
        gzfiles[g] = gz;
        c.ret(g);
    });
    register_import("gzread", [](Ctx& c) {
        gzFile gz;
        {
            std::lock_guard lk(files_lock);
            gz = gzfiles.at(c.arg(0));
        }
        c.ret(gzread(gz, mem::h(c.arg(1)), c.arg(2)));
    });
    register_import("gzclose", [](Ctx& c) {
        gzFile gz;
        {
            std::lock_guard lk(files_lock);
            gz = gzfiles.at(c.arg(0));
            gzfiles.erase(c.arg(0));
        }
        mem::free(mem::h(c.arg(0)));
        c.ret(gzclose(gz));
    });

    // --- time
    register_import("time", [](Ctx& c) {
        u32 t = time(nullptr);
        if (c.arg(0)) mem::wr32(c.arg(0), t);
        c.ret(t);
    });
    IMPORT(clock);
    register_import("difftime", [](Ctx& c) { write_ret(c, static_cast<double>(static_cast<s32>(c.arg(0)) - static_cast<s32>(c.arg(1)))); });
    register_import("gettimeofday", [](Ctx& c) {
        timeval tv;
        int r = gettimeofday(&tv, nullptr);
        if (c.arg(0)) tv_to_guest(tv, c.arg(0));
        c.ret(r);
    });
    register_import("clock_gettime", [](Ctx& c) {
        timespec ts;
        int r = clock_gettime(c.arg(0), &ts);
        mem::wr32(c.arg(1), ts.tv_sec);
        mem::wr32(c.arg(1) + 4, ts.tv_nsec);
        c.ret(r);
    });
    register_import("gmtime_r", [](Ctx& c) {
        time_t t = static_cast<s32>(mem::rd32(c.arg(0)));
        tm r;
        gmtime_r(&t, &r);
        tm_to_guest(r, c.arg(1));
        c.ret(c.arg(1));
    });
    register_import("gmtime", [](Ctx& c) {
        thread_local u32 buf = mem::g(mem::alloc(44));
        time_t t = static_cast<s32>(mem::rd32(c.arg(0)));
        tm r;
        gmtime_r(&t, &r);
        tm_to_guest(r, buf);
        c.ret(buf);
    });
    register_import("localtime_r", [](Ctx& c) {
        time_t t = static_cast<s32>(mem::rd32(c.arg(0)));
        tm r;
        localtime_r(&t, &r);
        tm_to_guest(r, c.arg(1));
        c.ret(c.arg(1));
    });
    register_import("mktime", [](Ctx& c) {
        tm t = tm_from_guest(c.arg(0));
        time_t r = mktime(&t);
        tm_to_guest(t, c.arg(0));
        c.ret(static_cast<u32>(r));
    });
    register_import("ctime", [](Ctx& c) {
        time_t t = static_cast<s32>(mem::rd32(c.arg(0)));
        u32 b = per_thread_buf(64);
        ctime_r(&t, mem::h<char>(b));
        c.ret(b);
    });
    register_import("strftime", [](Ctx& c) {
        tm t = tm_from_guest(c.arg(3));
        c.ret(strftime(mem::h<char>(c.arg(0)), c.arg(1), mem::h<char>(c.arg(2)), &t));
    });

    // --- files
    register_import("open", [](Ctx& c) {
        const char* path = mem::h<char>(c.arg(0));
        int flags = open_flags(c.arg(1));
        c.ret(open(path, flags, c.arg(2)));
    });
    IMPORT(read); IMPORT(write); IMPORT(close); IMPORT(lseek); IMPORT(access); IMPORT(chdir); IMPORT(chmod);
    IMPORT(mkdir); IMPORT(rmdir); IMPORT(remove); IMPORT(rename); IMPORT(unlink);
    IMPORT(fsync); IMPORT(ftruncate); IMPORT(dup2); IMPORT(pipe); IMPORT(readlink); IMPORT(symlink);
    register_import("getcwd", [](Ctx& c) {
        if (c.arg(0)) {
            char* r = getcwd(mem::h<char>(c.arg(0)), c.arg(1));
            return c.ret(r ? c.arg(0) : 0);
        }
        char tmp[1024];
        if (!getcwd(tmp, sizeof tmp)) return c.ret(0);
        c.ret(mem::strdup(tmp));
    });
    register_import("writev", [](Ctx& c) {
        int fd = c.arg(0);
        u32 iov_g = c.arg(1);
        int cnt = c.arg(2);
        std::vector<iovec> iov(cnt > 0 ? cnt : 0);
        for (int i = 0; i < cnt; i++) {
            iov[i].iov_base = mem::h(mem::rd32(iov_g + i * 8));
            iov[i].iov_len = mem::rd32(iov_g + i * 8 + 4);
        }
        c.ret(static_cast<u32>(writev(fd, iov.data(), cnt)));
    });
    register_import("poll", [](Ctx& c) {
        c.ret(poll(mem::h<pollfd>(c.arg(0)), c.arg(1), c.arg(2)));
    });
    register_import("fcntl", [](Ctx& c) { c.ret(fcntl(c.arg(0), c.arg(1), static_cast<long>(c.arg(2)))); });
    register_import("ioctl", [](Ctx& c) { c.ret(ioctl(c.arg(0), c.arg(1), mem::h(c.arg(2)))); });
    register_import("stat", [](Ctx& c) {
        struct stat s;
        int r = stat(mem::h<char>(c.arg(0)), &s);
        if (!r) stat_to_guest(s, c.arg(1));
        c.ret(r);
    });
    register_import("lstat", [](Ctx& c) {
        struct stat s;
        int r = lstat(mem::h<char>(c.arg(0)), &s);
        if (!r) stat_to_guest(s, c.arg(1));
        c.ret(r);
    });
    register_import("fstat", [](Ctx& c) {
        struct stat s;
        int r = fstat(c.arg(0), &s);
        if (!r) stat_to_guest(s, c.arg(1));
        c.ret(r);
    });
    register_import("utimes", [](Ctx& c) {
        timeval tv[2];
        if (c.arg(1)) { tv[0] = tv_from_guest(c.arg(1)); tv[1] = tv_from_guest(c.arg(1) + 8); }
        c.ret(utimes(mem::h<char>(c.arg(0)), c.arg(1) ? tv : nullptr));
    });
    register_import("opendir", [](Ctx& c) {
        DIR* d = opendir(mem::h<char>(c.arg(0)));
        if (!d) return c.ret(0);
        u32 g = mem::g(mem::calloc(1, 16));
        u32 ent = mem::g(mem::calloc(1, sizeof(dirent)));
        std::lock_guard lk(files_lock);
        dirs[g] = d;
        dirents[g] = ent;
        c.ret(g);
    });
    register_import("readdir", [](Ctx& c) {
        DIR* d;
        u32 ent;
        {
            std::lock_guard lk(files_lock);
            d = dirs.at(c.arg(0));
            ent = dirents.at(c.arg(0));
        }
        dirent* e = readdir(d);
        if (!e) return c.ret(0);
        memcpy(mem::h(ent), e, sizeof(dirent));
        c.ret(ent);
    });
    register_import("closedir", [](Ctx& c) {
        std::lock_guard lk(files_lock);
        int r = closedir(dirs.at(c.arg(0)));
        mem::free(mem::h(dirents.at(c.arg(0))));
        mem::free(mem::h(c.arg(0)));
        dirs.erase(c.arg(0));
        dirents.erase(c.arg(0));
        c.ret(r);
    });

    // --- stdio
    register_import("fopen", [](Ctx& c) {
        FILE* f = fopen(mem::h<char>(c.arg(0)), mem::h<char>(c.arg(1)));
        if (!f) return c.ret(0);
        u32 g = mem::g(mem::calloc(1, kFileSize));
        std::lock_guard lk(files_lock);
        files[g] = f;
        c.ret(g);
    });
    register_import("fdopen", [](Ctx& c) {
        FILE* f = fdopen(c.arg(0), mem::h<char>(c.arg(1)));
        if (!f) return c.ret(0);
        u32 g = mem::g(mem::calloc(1, kFileSize));
        std::lock_guard lk(files_lock);
        files[g] = f;
        c.ret(g);
    });
    register_import("fclose", [](Ctx& c) {
        FILE* f = hfile(c.arg(0));
        if (c.arg(0) - g_sF < 3 * kFileSize) return c.ret(0);
        int r = fclose(f);
        std::lock_guard lk(files_lock);
        files.erase(c.arg(0));
        mem::free(mem::h(c.arg(0)));
        c.ret(r);
    });
    register_import("fread", [](Ctx& c) { c.ret(fread(mem::h(c.arg(0)), c.arg(1), c.arg(2), hfile(c.arg(3)))); });
    register_import("fwrite", [](Ctx& c) { c.ret(fwrite(mem::h(c.arg(0)), c.arg(1), c.arg(2), hfile(c.arg(3)))); });
    register_import("fseek", [](Ctx& c) { c.ret(fseek(hfile(c.arg(0)), static_cast<s32>(c.arg(1)), c.arg(2))); });
    register_import("ftell", [](Ctx& c) { c.ret(static_cast<u32>(ftell(hfile(c.arg(0))))); });
    register_import("rewind", [](Ctx& c) { rewind(hfile(c.arg(0))); });
    register_import("fflush", [](Ctx& c) { c.ret(fflush(c.arg(0) ? hfile(c.arg(0)) : nullptr)); });
    register_import("setvbuf", [](Ctx& c) {
        c.ret(setvbuf(hfile(c.arg(0)), c.arg(1) ? mem::h<char>(c.arg(1)) : nullptr, c.arg(2), c.arg(3)));
    });
    register_import("fgetc", [](Ctx& c) { c.ret(fgetc(hfile(c.arg(0)))); });
    register_import("getc", [](Ctx& c) { c.ret(getc(hfile(c.arg(0)))); });
    register_import("ungetc", [](Ctx& c) { c.ret(ungetc(c.arg(0), hfile(c.arg(1)))); });
    register_import("fputc", [](Ctx& c) { c.ret(fputc(c.arg(0), hfile(c.arg(1)))); });
    register_import("putc", [](Ctx& c) { c.ret(putc(c.arg(0), hfile(c.arg(1)))); });
    register_import("fputs", [](Ctx& c) { c.ret(fputs(mem::h<char>(c.arg(0)), hfile(c.arg(1)))); });
    register_import("fgets", [](Ctx& c) {
        char* r = fgets(mem::h<char>(c.arg(0)), c.arg(1), hfile(c.arg(2)));
        c.ret(r ? c.arg(0) : 0);
    });
    register_import("putchar", [](Ctx& c) { c.ret(fputc(c.arg(0), hfile(g_sF + kFileSize))); });
    register_import("puts", [](Ctx& c) { LOGI("guest: %s", mem::h<char>(c.arg(0))); c.ret(1); });

    register_import("printf", [](Ctx& c) {
        ArgReader ar{c, 1};
        std::string s = gformat(mem::h<char>(c.arg(0)), {&ar});
        c.ret(fputs(s.c_str(), hfile(g_sF + kFileSize)) >= 0 ? s.size() : -1);
    });
    register_import("vprintf", [](Ctx& c) {
        std::string s = gformat(mem::h<char>(c.arg(0)), {nullptr, c.arg(1)});
        c.ret(fputs(s.c_str(), hfile(g_sF + kFileSize)) >= 0 ? s.size() : -1);
    });
    register_import("fprintf", [](Ctx& c) {
        ArgReader ar{c, 2};
        std::string s = gformat(mem::h<char>(c.arg(1)), {&ar});
        c.ret(fputs(s.c_str(), hfile(c.arg(0))) >= 0 ? s.size() : -1);
    });
    register_import("vfprintf", [](Ctx& c) {
        std::string s = gformat(mem::h<char>(c.arg(1)), {nullptr, c.arg(2)});
        c.ret(fputs(s.c_str(), hfile(c.arg(0))) >= 0 ? s.size() : -1);
    });
    register_import("sprintf", [](Ctx& c) {
        ArgReader ar{c, 2};
        std::string s = gformat(mem::h<char>(c.arg(1)), {&ar});
        memcpy(mem::h(c.arg(0)), s.c_str(), s.size() + 1);
        c.ret(s.size());
    });
    register_import("vsprintf", [](Ctx& c) {
        std::string s = gformat(mem::h<char>(c.arg(1)), {nullptr, c.arg(2)});
        memcpy(mem::h(c.arg(0)), s.c_str(), s.size() + 1);
        c.ret(s.size());
    });
    static auto put_n = [](Ctx& c, const std::string& s) {
        u32 n = c.arg(1);
        if (n) {
            size_t k = std::min<size_t>(s.size(), n - 1);
            memcpy(mem::h(c.arg(0)), s.data(), k);
            mem::h<char>(c.arg(0))[k] = 0;
        }
        c.ret(s.size());
    };
    register_import("snprintf", [](Ctx& c) {
        ArgReader ar{c, 3};
        put_n(c, gformat(mem::h<char>(c.arg(2)), {&ar}));
    });
    register_import("vsnprintf", [](Ctx& c) { put_n(c, gformat(mem::h<char>(c.arg(2)), {nullptr, c.arg(3)})); });
    register_import("__android_log_print", [](Ctx& c) {
        ArgReader ar{c, 3};
        std::string s = gformat(mem::h<char>(c.arg(2)), {&ar});
        __android_log_write(ANDROID_LOG_ERROR, c.arg(1) ? mem::h<char>(c.arg(1)) : "HamboGuest", s.c_str());
        c.ret(1);
    });
    register_import("sscanf", [](Ctx& c) {
        ArgReader ar{c, 2};
        c.ret(gscan(mem::h<char>(c.arg(0)), nullptr, mem::h<char>(c.arg(1)), {&ar}));
    });
    register_import("fscanf", [](Ctx& c) {
        ArgReader ar{c, 2};
        c.ret(gscan(nullptr, hfile(c.arg(0)), mem::h<char>(c.arg(1)), {&ar}));
    });
    register_import("vscanf", [](Ctx& c) { c.ret(EOF); });

    // --- sockets
    IMPORT(socket); IMPORT(bind); IMPORT(connect); IMPORT(listen); IMPORT(accept); IMPORT(send); IMPORT(recv);
    IMPORT(sendto); IMPORT(recvfrom); IMPORT(shutdown); IMPORT(getsockname); IMPORT(getpeername);
    IMPORT(gethostname); IMPORT(inet_addr); IMPORT(inet_pton);
    register_import("inet_ntop", [](Ctx& c) {
        const char* r = inet_ntop(c.arg(0), mem::h(c.arg(1)), mem::h<char>(c.arg(2)), c.arg(3));
        c.ret(r ? c.arg(2) : 0);
    });
    register_import("setsockopt", [](Ctx& c) {
        if (c.arg(1) == SOL_SOCKET && (c.arg(2) == SO_RCVTIMEO || c.arg(2) == SO_SNDTIMEO)) {
            timeval tv = tv_from_guest(c.arg(3));
            return c.ret(setsockopt(c.arg(0), c.arg(1), c.arg(2), &tv, sizeof tv));
        }
        c.ret(setsockopt(c.arg(0), c.arg(1), c.arg(2), mem::h(c.arg(3)), c.arg(4)));
    });
    IMPORT(getsockopt);
    register_import("select", [](Ctx& c) {
        timeval tv, *ptv = nullptr;
        if (c.arg(4)) { tv = tv_from_guest(c.arg(4)); ptv = &tv; }
        int r = select(c.arg(0), mem::h<fd_set>(c.arg(1)), mem::h<fd_set>(c.arg(2)), mem::h<fd_set>(c.arg(3)), ptv);
        if (ptv) tv_to_guest(tv, c.arg(4));
        c.ret(r);
    });
    register_import("inet_ntoa", [](Ctx& c) {
        in_addr a{c.arg(0)};
        u32 b = per_thread_buf(32);
        strcpy(mem::h<char>(b), inet_ntoa(a));
        c.ret(b);
    });
    register_import("gethostbyname", [](Ctx& c) { c.ret(hostent_to_guest(gethostbyname(mem::h<char>(c.arg(0))))); });
    register_import("gethostbyaddr", [](Ctx& c) {
        c.ret(hostent_to_guest(gethostbyaddr(mem::h(c.arg(0)), c.arg(1), c.arg(2))));
    });
    register_import("getaddrinfo", [](Ctx& c) {
        addrinfo hints{}, *res = nullptr;
        if (c.arg(2)) {
            hints.ai_flags = mem::rd32(c.arg(2));
            hints.ai_family = mem::rd32(c.arg(2) + 4);
            hints.ai_socktype = mem::rd32(c.arg(2) + 8);
            hints.ai_protocol = mem::rd32(c.arg(2) + 12);
        }
        int r = getaddrinfo(c.arg(0) ? mem::h<char>(c.arg(0)) : nullptr, c.arg(1) ? mem::h<char>(c.arg(1)) : nullptr,
                            c.arg(2) ? &hints : nullptr, &res);
        mem::wr32(c.arg(3), r ? 0 : addrinfo_to_guest(res));
        if (res) freeaddrinfo(res);
        c.ret(r);
    });
    register_import("freeaddrinfo", [](Ctx& c) {
        for (u32 a = c.arg(0); a;) {
            u32 next = mem::rd32(a + 28);
            if (mem::rd32(a + 20)) mem::free(mem::h(mem::rd32(a + 20)));
            mem::free(mem::h(mem::rd32(a + 24)));
            mem::free(mem::h(a));
            a = next;
        }
    });
}

// ---------------------------------------------------------------- soft-float / division helpers
namespace {
inline float f32(u32 b) { float x; memcpy(&x, &b, 4); return x; }
inline u32 bits(float x) { u32 b; memcpy(&b, &x, 4); return b; }
inline double f64(u32 lo, u32 hi) { u64 b = lo | (u64(hi) << 32); double x; memcpy(&x, &b, 8); return x; }
inline u64 bits(double x) { u64 b; memcpy(&b, &x, 8); return b; }
inline u64 w64(u32 lo, u32 hi) { return lo | (u64(hi) << 32); }

template <class T> int64_t to_s64(T x) {
    if (x != x) return 0;
    if (x >= T(9223372036854775808.0)) return INT64_MAX;
    if (x <= T(-9223372036854775808.0)) return INT64_MIN;
    return static_cast<int64_t>(x);
}
template <class T> u64 to_u64(T x) {
    if (!(x > T(0))) return 0;
    if (x >= T(18446744073709551616.0)) return UINT64_MAX;
    return static_cast<u64>(x);
}

#define F(name, ...) {name, [](Ctx& c) { auto& r = c.r; (void)r; __VA_ARGS__; }}
const std::pair<const char*, Handler> kSoftFp[] = {
    F("__aeabi_l2f", c.ret(bits(static_cast<float>(static_cast<int64_t>(w64(r[0], r[1])))))),
    F("__aeabi_ul2f", c.ret(bits(static_cast<float>(w64(r[0], r[1]))))),
    F("__aeabi_l2d", c.ret64(bits(static_cast<double>(static_cast<int64_t>(w64(r[0], r[1])))))),
    F("__aeabi_ul2d", c.ret64(bits(static_cast<double>(w64(r[0], r[1]))))),
    F("__aeabi_f2lz", c.ret64(static_cast<u64>(to_s64(f32(r[0]))))),
    F("__aeabi_f2ulz", c.ret64(to_u64(f32(r[0])))),
    F("__aeabi_d2lz", c.ret64(static_cast<u64>(to_s64(f64(r[0], r[1]))))),
    F("__aeabi_d2ulz", c.ret64(to_u64(f64(r[0], r[1])))),
    F("__aeabi_lmul", c.ret64(w64(r[0], r[1]) * w64(r[2], r[3]))),
    F("__aeabi_uldivmod", {
        u64 a = w64(r[0], r[1]), b = w64(r[2], r[3]);
        u64 q = b ? a / b : 0, m = b ? a % b : a;
        r[0] = u32(q); r[1] = u32(q >> 32); r[2] = u32(m); r[3] = u32(m >> 32);
    }),
    F("__aeabi_ldivmod", {
        int64_t a = w64(r[0], r[1]), b = w64(r[2], r[3]);
        bool ovf = a == INT64_MIN && b == -1;
        int64_t q = !b ? 0 : ovf ? a : a / b, m = !b ? a : ovf ? 0 : a % b;
        r[0] = u32(q); r[1] = u32(u64(q) >> 32); r[2] = u32(m); r[3] = u32(u64(m) >> 32);
    }),
};
#undef F

struct VfpStub {
    const char* name;
    std::vector<u32> code;
};
const VfpStub kVfpStubs[] = {
    {"__aeabi_fadd", {0xee000a10, 0xee001a90, 0xee300a20, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_fsub", {0xee000a10, 0xee001a90, 0xee300a60, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_frsub", {0xee000a10, 0xee001a90, 0xee300ac0, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_fmul", {0xee000a10, 0xee001a90, 0xee200a20, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_fdiv", {0xee000a10, 0xee001a90, 0xee800a20, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_dadd", {0xec410b10, 0xec432b11, 0xee300b01, 0xec510b10, 0xe12fff1e}},
    {"__aeabi_dsub", {0xec410b10, 0xec432b11, 0xee300b41, 0xec510b10, 0xe12fff1e}},
    {"__aeabi_drsub", {0xec410b10, 0xec432b11, 0xee310b40, 0xec510b10, 0xe12fff1e}},
    {"__aeabi_dmul", {0xec410b10, 0xec432b11, 0xee200b01, 0xec510b10, 0xe12fff1e}},
    {"__aeabi_ddiv", {0xec410b10, 0xec432b11, 0xee800b01, 0xec510b10, 0xe12fff1e}},
    {"__aeabi_fcmpeq", {0xee000a10, 0xee001a90, 0xeeb40a60, 0xeef1fa10, 0xe3a00000, 0x03a00001, 0xe12fff1e}},
    {"__aeabi_fcmplt", {0xee000a10, 0xee001a90, 0xeeb40a60, 0xeef1fa10, 0xe3a00000, 0x43a00001, 0xe12fff1e}},
    {"__aeabi_fcmple", {0xee000a10, 0xee001a90, 0xeeb40a60, 0xeef1fa10, 0xe3a00000, 0x93a00001, 0xe12fff1e}},
    {"__aeabi_fcmpge", {0xee000a10, 0xee001a90, 0xeeb40a60, 0xeef1fa10, 0xe3a00000, 0xa3a00001, 0xe12fff1e}},
    {"__aeabi_fcmpgt", {0xee000a10, 0xee001a90, 0xeeb40a60, 0xeef1fa10, 0xe3a00000, 0xc3a00001, 0xe12fff1e}},
    {"__aeabi_fcmpun", {0xee000a10, 0xee001a90, 0xeeb40a60, 0xeef1fa10, 0xe3a00000, 0x63a00001, 0xe12fff1e}},
    {"__aeabi_dcmpeq", {0xec410b10, 0xec432b11, 0xeeb40b41, 0xeef1fa10, 0xe3a00000, 0x03a00001, 0xe12fff1e}},
    {"__aeabi_dcmplt", {0xec410b10, 0xec432b11, 0xeeb40b41, 0xeef1fa10, 0xe3a00000, 0x43a00001, 0xe12fff1e}},
    {"__aeabi_dcmple", {0xec410b10, 0xec432b11, 0xeeb40b41, 0xeef1fa10, 0xe3a00000, 0x93a00001, 0xe12fff1e}},
    {"__aeabi_dcmpge", {0xec410b10, 0xec432b11, 0xeeb40b41, 0xeef1fa10, 0xe3a00000, 0xa3a00001, 0xe12fff1e}},
    {"__aeabi_dcmpgt", {0xec410b10, 0xec432b11, 0xeeb40b41, 0xeef1fa10, 0xe3a00000, 0xc3a00001, 0xe12fff1e}},
    {"__aeabi_dcmpun", {0xec410b10, 0xec432b11, 0xeeb40b41, 0xeef1fa10, 0xe3a00000, 0x63a00001, 0xe12fff1e}},
    {"__aeabi_f2iz", {0xee000a10, 0xeebd0ac0, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_f2uiz", {0xee000a10, 0xeebc0ac0, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_i2f", {0xee000a10, 0xeeb80ac0, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_ui2f", {0xee000a10, 0xeeb80a40, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_f2d", {0xee000a10, 0xeeb71ac0, 0xec510b11, 0xe12fff1e}},
    {"__aeabi_d2f", {0xec410b10, 0xeeb70bc0, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_d2iz", {0xec410b10, 0xeebd0bc0, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_d2uiz", {0xec410b10, 0xeebc0bc0, 0xee100a10, 0xe12fff1e}},
    {"__aeabi_i2d", {0xee000a10, 0xeeb81bc0, 0xec510b11, 0xe12fff1e}},
    {"__aeabi_ui2d", {0xee000a10, 0xeeb81b40, 0xec510b11, 0xe12fff1e}},
    {"__aeabi_idiv", {0xe710f110, 0xe12fff1e}},
    {"__aeabi_uidiv", {0xe730f110, 0xe12fff1e}},
    {"__aeabi_idivmod", {0xe712f110, 0xe0610192, 0xe1a00002, 0xe12fff1e}},
    {"__aeabi_uidivmod", {0xe732f110, 0xe0610192, 0xe1a00002, 0xe12fff1e}},
};
}  // namespace

// Called BEFORE load_guest(): resolves SHN_UNDEF __aeabi_* imports directly to guest VFP/IDIV code
// or kSoftFp thunks.
void init_softfp_imports() {
    size_t words = 0;
    for (auto& st : kVfpStubs) words += st.code.size();
    u32 next = mem::g(mem::alloc_aligned(16, words * 4));
    for (auto& st : kVfpStubs) {
        memcpy(mem::h(next), st.code.data(), st.code.size() * 4);
        register_data(st.name, next);
        next += st.code.size() * 4;
    }
    for (auto& [name, h] : kSoftFp) {
        register_import(name, h);
    }
}

void init_softfp(const GuestLib& lib) {
    size_t words = 0;
    for (auto& st : kVfpStubs) words += st.code.size();
    u32 next = mem::g(mem::alloc_aligned(16, words * 4));
    int n = 0;
    for (auto& st : kVfpStubs) {
        u32 a = lib.sym(st.name);
        if (!a || (a & 3)) continue;
        memcpy(mem::h(next), st.code.data(), st.code.size() * 4);
        int64_t off = int64_t(next) - (int64_t(a) + 8);
        if (off >= (1 << 25) || off < -(1 << 25)) continue;
        mem::wr32(a, 0xEA000000 | ((u32(off) >> 2) & 0xFFFFFF));  // b stub
        next += st.code.size() * 4;
        invalidate_guest_code(a, 4);
        n++;
    }
    for (auto& [name, h] : kSoftFp) {
        u32 a = lib.sym(name);
        if (!a || (a & 3)) continue;
        hook_guest(a, h, name);
        invalidate_guest_code(a, 4);
        n++;
    }
    LOGI("patched %d in-guest soft-float/division helpers (plus PLT VFP stubs)", n);
}
