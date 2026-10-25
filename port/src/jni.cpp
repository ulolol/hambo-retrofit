#include <dlfcn.h>
#include <jni.h>
#include <sys/system_properties.h>

#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core.h"

GuestLib g_lib;
bool register_guest_native(JNIEnv* e, jclass cls, const char* name, const char* sig, u32 fn);

namespace {
JavaVM* g_vm;
u32 g_guest_vm, g_guest_env;
thread_local JNIEnv* t_env;

JNIEnv* env() {
    if (!t_env && g_vm->GetEnv(reinterpret_cast<void**>(&t_env), JNI_VERSION_1_6) != JNI_OK)
        g_vm->AttachCurrentThread(&t_env, nullptr);
    return t_env;
}

// ---------------------------------------------------------------- object handles
std::mutex ref_lock;
std::vector<jobject> refs{nullptr};
std::vector<u32> free_ids;
std::unordered_set<u32> persistent_ids;
std::unordered_map<std::string, u32> class_cache;
thread_local std::vector<u32> t_locals;

u32 put(jobject o) {
    if (!o) return 0;
    std::lock_guard lk(ref_lock);
    u32 id;
    if (!free_ids.empty()) { id = free_ids.back(); free_ids.pop_back(); refs[id] = o; }
    else { id = refs.size(); refs.push_back(o); }
    return id;
}
u32 put_global(jobject o) {
    if (!o) return 0;
    jobject g = env()->NewGlobalRef(o);
    u32 id = put(g);
    std::lock_guard lk(ref_lock);
    persistent_ids.insert(id);
    return id;
}
u32 put_local(jobject o) {
    u32 id = put(o);
    if (id) t_locals.push_back(id);
    return id;
}
jobject get(u32 id) {
    if (!id) return nullptr;
    std::lock_guard lk(ref_lock);
    if (id >= refs.size()) fatal("bad guest jobject %u", id);
    return refs[id];
}
bool is_persistent(u32 id) {
    std::lock_guard lk(ref_lock);
    return persistent_ids.count(id) != 0;
}
void drop(u32 id) {
    if (!id) return;
    std::lock_guard lk(ref_lock);
    if (persistent_ids.count(id)) return;
    refs[id] = nullptr;
    free_ids.push_back(id);
}
void drop_global(u32 id) {
    if (!id) return;
    std::lock_guard lk(ref_lock);
    persistent_ids.erase(id);
    refs[id] = nullptr;
    free_ids.push_back(id);
}
void forget_local(u32 id) {
    for (auto it = t_locals.rbegin(); it != t_locals.rend(); ++it)
        if (*it == id) { *it = 0; return; }
}
struct LocalFrame {
    size_t mark = t_locals.size();
    ~LocalFrame() {
        for (size_t i = mark; i < t_locals.size(); i++) drop(t_locals[i]);
        t_locals.resize(mark);
    }
};

// ---------------------------------------------------------------- method / field IDs
struct Member {
    void* id;
    std::string name;
    std::string args;
    char ret;
};
std::mutex member_lock;
std::vector<Member> members{{nullptr, "", "", 'V'}};
std::unordered_map<void*, u32> member_ids;

void parse_sig(const char* s, std::string& args, char& ret) {
    args.clear();
    if (*s++ != '(') { ret = s[-1] == '[' ? 'L' : s[-1]; return; }
    while (*s && *s != ')') {
        char c = *s;
        while (*s == '[') s++;
        if (*s == 'L') while (*s && *s != ';') s++;
        args += c == '[' ? 'L' : c;
        s++;
    }
    s++;
    ret = *s == '[' ? 'L' : *s;
}
u32 member(void* id, const char* name, const char* sig) {
    if (!id) return 0;
    std::lock_guard lk(member_lock);
    auto [it, fresh] = member_ids.try_emplace(id, members.size());
    if (fresh) {
        Member m{id, name, "", 'V'};
        parse_sig(sig, m.args, m.ret);
        members.push_back(m);
    }
    return it->second;
}
Member mget(u32 i) {
    std::lock_guard lk(member_lock);
    if (!i || i >= members.size()) fatal("bad guest method/field id %u", i);
    return members[i];
}

// ---------------------------------------------------------------- calls
std::vector<jvalue> read_args(const Member& m, VaReader va) {
    std::vector<jvalue> v(m.args.size());
    for (size_t i = 0; i < m.args.size(); i++) {
        switch (m.args[i]) {
            case 'J': v[i].j = static_cast<jlong>(va.dw()); break;
            case 'D': { u64 b = va.dw(); memcpy(&v[i].d, &b, 8); break; }
            case 'F': { u64 b = va.dw(); double d; memcpy(&d, &b, 8); v[i].f = static_cast<float>(d); break; }
            case 'L': v[i].l = get(va.w()); break;
            case 'Z': v[i].z = static_cast<jboolean>(va.w()); break;
            case 'B': v[i].b = static_cast<jbyte>(va.w()); break;
            case 'C': v[i].c = static_cast<jchar>(va.w()); break;
            case 'S': v[i].s = static_cast<jshort>(va.w()); break;
            default: v[i].i = static_cast<jint>(va.w()); break;
        }
    }
    return v;
}
std::vector<jvalue> read_jvalues(const Member& m, u32 arr) {
    std::vector<jvalue> v(m.args.size());
    for (size_t i = 0; i < m.args.size(); i++) {
        u32 a = arr + 8 * i;
        if (m.args[i] == 'L') v[i].l = get(mem::rd32(a));
        else memcpy(&v[i], mem::h(a), 8);
    }
    return v;
}

void count_frame() {
    static std::atomic<u32> frames{0};
    static std::atomic<int64_t> last{0};
    static u64 last_svc = 0;
    frames++;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec, prev = last.load();
    if (now - prev >= 5 && last.compare_exchange_strong(prev, now)) {
        u64 svc = g_svc_count.load();
        if (prev) LOGI("perf: %.1f fps, %llu host calls/s", frames.exchange(0) / double(now - prev),
                       static_cast<unsigned long long>((svc - last_svc) / (now - prev)));
        else frames = 0;
        last_svc = svc;
        if (g_profile) log_svc_top();
        log_guest_top();
    }
}

void exception_guard() {
    if (env()->ExceptionCheck()) {
        LOGW("Java exception during guest JNI call:");
        env()->ExceptionDescribe();
        env()->ExceptionClear();
    }
}

enum Kind { kVirtual, kNonvirtual, kStatic, kCtor };
void call_java(Ctx& c, Kind k, char ret, int mode /*0 = ..., 1 = V, 2 = A*/) {
    JNIEnv* e = env();
    int first = k == kNonvirtual ? 4 : 3;
    jobject obj = get(c.arg(1));
    jclass cls = static_cast<jclass>(k == kNonvirtual ? get(c.arg(2)) : obj);
    Member m = mget(c.arg(first - 1));
    auto mid = static_cast<jmethodID>(m.id);
    std::vector<jvalue> a;
    if (mode == 2) a = read_jvalues(m, c.arg(first));
    else if (mode == 1) a = read_args(m, {nullptr, c.arg(first)});
    else { ArgReader ar{c, first}; a = read_args(m, {&ar}); }
    const jvalue* av = a.data();
    jvalue r{};
#define DISPATCH(T, field)                                                                            \
    r.field = k == kStatic ? e->CallStatic##T##MethodA(cls, mid, av)                                  \
            : k == kNonvirtual ? e->CallNonvirtual##T##MethodA(obj, cls, mid, av)                     \
                               : e->Call##T##MethodA(obj, mid, av)
    switch (ret) {
        case 'V':
            if (k == kStatic) e->CallStaticVoidMethodA(cls, mid, av);
            else if (k == kNonvirtual) e->CallNonvirtualVoidMethodA(obj, cls, mid, av);
            else e->CallVoidMethodA(obj, mid, av);
            break;
        case 'L':
            if (k == kCtor) r.l = e->NewObjectA(cls, mid, av);
            else DISPATCH(Object, l);
            break;
        case 'Z': DISPATCH(Boolean, z); break;
        case 'B': DISPATCH(Byte, b); break;
        case 'C': DISPATCH(Char, c); break;
        case 'S': DISPATCH(Short, s); break;
        case 'I': DISPATCH(Int, i); break;
        case 'J': DISPATCH(Long, j); break;
        case 'F': DISPATCH(Float, f); break;
        case 'D': DISPATCH(Double, d); break;
    }
#undef DISPATCH
    exception_guard();
    switch (ret) {
        case 'L': c.ret(put_local(r.l)); break;
        case 'Z': c.ret(r.z); break;
        case 'B': c.ret(static_cast<u32>(r.b)); break;
        case 'C': c.ret(r.c); break;
        case 'S': c.ret(static_cast<u32>(r.s)); break;
        case 'I': c.ret(r.i); break;
        case 'J': c.ret64(r.j); break;
        case 'F': write_ret(c, r.f); break;
        case 'D': write_ret(c, r.d); break;
    }
}

const char kTypes[] = "LZBCSIJFDV";
const char kPrims[] = "ZBCSIJFD";
int prim_size(char t) { return t == 'J' || t == 'D' ? 8 : t == 'I' || t == 'F' ? 4 : t == 'C' || t == 'S' ? 2 : 1; }

std::mutex arr_lock;
std::unordered_map<u32, std::pair<jarray, char>> pinned;

void region(JNIEnv* e, char t, jarray a, jsize start, jsize len, void* buf, bool set) {
#define R(T, J, CT)                                                                    \
    case J:                                                                            \
        if (set) e->Set##T##ArrayRegion(static_cast<CT##Array>(a), start, len, static_cast<CT*>(buf)); \
        else e->Get##T##ArrayRegion(static_cast<CT##Array>(a), start, len, static_cast<CT*>(buf));     \
        break;
    switch (t) {
        R(Boolean, 'Z', jboolean) R(Byte, 'B', jbyte) R(Char, 'C', jchar) R(Short, 'S', jshort)
        R(Int, 'I', jint) R(Long, 'J', jlong) R(Float, 'F', jfloat) R(Double, 'D', jdouble)
    }
#undef R
}
char array_type(jarray a) {
    JNIEnv* e = env();
    jclass cls = e->GetObjectClass(a);
    jmethodID getName = e->GetMethodID(e->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
    auto name = static_cast<jstring>(e->CallObjectMethod(cls, getName));
    const char* s = e->GetStringUTFChars(name, nullptr);
    char t = s[1];
    e->ReleaseStringUTFChars(name, s);
    return t;
}
u32 pin_array(jarray a, char t, u32 is_copy) {
    JNIEnv* e = env();
    jsize n = e->GetArrayLength(a);
    void* buf = mem::alloc(n * prim_size(t) + 1);
    region(e, t, a, 0, n, buf, false);
    if (is_copy) *mem::h<u8>(is_copy) = 1;
    std::lock_guard lk(arr_lock);
    pinned[mem::g(buf)] = {static_cast<jarray>(e->NewGlobalRef(a)), t};
    return mem::g(buf);
}
void unpin_array(u32 buf, u32 mode) {
    JNIEnv* e = env();
    std::pair<jarray, char> p;
    {
        std::lock_guard lk(arr_lock);
        p = pinned.at(buf);
    }
    if (mode != JNI_ABORT) region(e, p.second, p.first, 0, e->GetArrayLength(p.first), mem::h(buf), true);
    if (mode != JNI_COMMIT) {
        e->DeleteGlobalRef(p.first);
        mem::free(mem::h(buf));
        std::lock_guard lk(arr_lock);
        pinned.erase(buf);
    }
}

// ---------------------------------------------------------------- the guest JNIEnv table
void jni_fn(Ctx& c, int idx);
template <int I> void jni_thunk(Ctx& c) { jni_fn(c, I); }
template <int... I> constexpr std::array<Handler, sizeof...(I)> jni_handlers(std::integer_sequence<int, I...>) {
    return {&jni_thunk<I>...};
}

void jni_fn(Ctx& c, int idx) {
    JNIEnv* e = env();
    auto obj = [&](int i) { return get(c.arg(i)); };
    auto str = [&](int i) { return mem::h<char>(c.arg(i)); };

    if (idx >= 34 && idx <= 63) return call_java(c, kVirtual, kTypes[(idx - 34) / 3], (idx - 34) % 3);
    if (idx >= 64 && idx <= 93) return call_java(c, kNonvirtual, kTypes[(idx - 64) / 3], (idx - 64) % 3);
    if (idx >= 114 && idx <= 143) return call_java(c, kStatic, kTypes[(idx - 114) / 3], (idx - 114) % 3);
    if (idx >= 28 && idx <= 30) return call_java(c, kCtor, 'L', idx - 28);

    bool get_f = (idx >= 95 && idx <= 103) || (idx >= 145 && idx <= 153);
    bool set_f = (idx >= 104 && idx <= 112) || (idx >= 154 && idx <= 162);
    if (get_f || set_f) {
        bool is_static = idx >= 145;
        int base = idx >= 154 ? 154 : idx >= 145 ? 145 : idx >= 104 ? 104 : 95;
        char t = kTypes[idx - base];
        jobject o = obj(1);
        auto fid = static_cast<jfieldID>(mget(c.arg(2)).id);
        auto cls = static_cast<jclass>(o);
        if (get_f) {
            jvalue r{};
#define G(T, f) r.f = is_static ? e->GetStatic##T##Field(cls, fid) : e->Get##T##Field(o, fid)
            switch (t) {
                case 'L': G(Object, l); return c.ret(put_local(r.l));
                case 'Z': G(Boolean, z); return c.ret(r.z);
                case 'B': G(Byte, b); return c.ret(static_cast<u32>(r.b));
                case 'C': G(Char, c); return c.ret(r.c);
                case 'S': G(Short, s); return c.ret(static_cast<u32>(r.s));
                case 'I': G(Int, i); return c.ret(r.i);
                case 'J': G(Long, j); return c.ret64(r.j);
                case 'F': G(Float, f); return write_ret(c, r.f);
                case 'D': G(Double, d); return write_ret(c, r.d);
            }
#undef G
        }
        ArgReader ar{c, 3};
        jvalue v{};
        if (t == 'J' || t == 'D') { u64 b = ar.dw(); memcpy(&v, &b, 8); }
        else if (t == 'L') v.l = get(ar.w());
        else { u32 w = ar.w(); memcpy(&v, &w, 4); }
#define S(T, f) is_static ? e->SetStatic##T##Field(cls, fid, v.f) : e->Set##T##Field(o, fid, v.f)
        switch (t) {
            case 'L': S(Object, l); break;
            case 'Z': S(Boolean, z); break;
            case 'B': S(Byte, b); break;
            case 'C': S(Char, c); break;
            case 'S': S(Short, s); break;
            case 'I': S(Int, i); break;
            case 'J': S(Long, j); break;
            case 'F': S(Float, f); break;
            case 'D': S(Double, d); break;
        }
#undef S
        return;
    }
    if (idx >= 175 && idx <= 182) {
        char t = kPrims[idx - 175];
        jsize n = c.arg(1);
        jarray a = nullptr;
        switch (t) {
            case 'Z': a = e->NewBooleanArray(n); break;
            case 'B': a = e->NewByteArray(n); break;
            case 'C': a = e->NewCharArray(n); break;
            case 'S': a = e->NewShortArray(n); break;
            case 'I': a = e->NewIntArray(n); break;
            case 'J': a = e->NewLongArray(n); break;
            case 'F': a = e->NewFloatArray(n); break;
            case 'D': a = e->NewDoubleArray(n); break;
        }
        return c.ret(put_local(a));
    }
    if (idx >= 183 && idx <= 190) return c.ret(pin_array(static_cast<jarray>(obj(1)), kPrims[idx - 183], c.arg(2)));
    if (idx >= 191 && idx <= 198) return unpin_array(c.arg(2), c.arg(3));
    if (idx >= 199 && idx <= 214) {
        bool set = idx >= 207;
        region(e, kPrims[idx - (set ? 207 : 199)], static_cast<jarray>(obj(1)), c.arg(2), c.arg(3), mem::h(c.arg(4)), set);
        return;
    }

    switch (idx) {
        case 4: return c.ret(e->GetVersion());
        case 6: {
            const char* cname = str(1);
            {
                std::lock_guard lk(ref_lock);
                auto it = class_cache.find(cname);
                if (it != class_cache.end()) return c.ret(it->second);
            }
            jclass cls = e->FindClass(cname);
            exception_guard();
            if (!cls) return c.ret(0);
            u32 id = put_global(cls);
            e->DeleteLocalRef(cls);
            {
                std::lock_guard lk(ref_lock);
                class_cache[cname] = id;
            }
            return c.ret(id);
        }
        case 10: return c.ret(put_global(e->GetSuperclass(static_cast<jclass>(obj(1)))));
        case 11: return c.ret(e->IsAssignableFrom(static_cast<jclass>(obj(1)), static_cast<jclass>(obj(2))));
        case 13: return c.ret(e->Throw(static_cast<jthrowable>(obj(1))));
        case 14: return c.ret(e->ThrowNew(static_cast<jclass>(obj(1)), str(2)));
        case 15: return c.ret(put_local(e->ExceptionOccurred()));
        case 16: return e->ExceptionDescribe();
        case 17: return e->ExceptionClear();
        case 18: fatal("guest FatalError: %s", str(1));
        case 19: return c.ret(e->PushLocalFrame(c.arg(1)));
        case 20: return c.ret(put_local(e->PopLocalFrame(obj(1))));
        case 21: return c.ret(put_global(obj(1)));
        case 22: {
            jobject o = obj(1);
            if (o) e->DeleteGlobalRef(o);
            return drop_global(c.arg(1));
        }
        case 23: {
            if (!is_persistent(c.arg(1))) {
                e->DeleteLocalRef(obj(1));
                forget_local(c.arg(1));
                drop(c.arg(1));
            }
            return;
        }
        case 24: return c.ret(e->IsSameObject(obj(1), obj(2)));
        case 25: return c.ret(put_local(e->NewLocalRef(obj(1))));
        case 26: return c.ret(e->EnsureLocalCapacity(c.arg(1)));
        case 27: return c.ret(put_local(e->AllocObject(static_cast<jclass>(obj(1)))));
        case 31: return c.ret(put_global(e->GetObjectClass(obj(1))));
        case 32: return c.ret(e->IsInstanceOf(obj(1), static_cast<jclass>(obj(2))));
        case 33: case 113: case 94: case 144: {
            auto cls = static_cast<jclass>(obj(1));
            void* id = idx == 33    ? static_cast<void*>(e->GetMethodID(cls, str(2), str(3)))
                       : idx == 113 ? static_cast<void*>(e->GetStaticMethodID(cls, str(2), str(3)))
                       : idx == 94  ? static_cast<void*>(e->GetFieldID(cls, str(2), str(3)))
                                    : static_cast<void*>(e->GetStaticFieldID(cls, str(2), str(3)));
            if (!id) LOGW("guest asked for missing member %s %s", str(2), str(3));
            exception_guard();
            return c.ret(member(id, str(2), str(3)));
        }
        case 163: return c.ret(put_local(e->NewString(mem::h<jchar>(c.arg(1)), c.arg(2))));
        case 164: return c.ret(e->GetStringLength(static_cast<jstring>(obj(1))));
        case 165: case 224: {
            auto s = static_cast<jstring>(obj(1));
            jsize n = e->GetStringLength(s);
            auto* buf = static_cast<jchar*>(mem::alloc((n + 1) * 2));
            e->GetStringRegion(s, 0, n, buf);
            buf[n] = 0;
            if (c.arg(2)) *mem::h<u8>(c.arg(2)) = 1;
            return c.ret(mem::g(buf));
        }
        case 166: case 225: return mem::free(mem::h(c.arg(2)));
        case 167: return c.ret(put_local(e->NewStringUTF(str(1))));
        case 168: return c.ret(e->GetStringUTFLength(static_cast<jstring>(obj(1))));
        case 169: {
            if (!c.arg(1)) return c.ret(0);
            const char* s = e->GetStringUTFChars(static_cast<jstring>(obj(1)), nullptr);
            u32 g = mem::strdup(s);
            e->ReleaseStringUTFChars(static_cast<jstring>(obj(1)), s);
            if (c.arg(2)) *mem::h<u8>(c.arg(2)) = 1;
            return c.ret(g);
        }
        case 170: return mem::free(mem::h(c.arg(2)));
        case 171: return c.ret(e->GetArrayLength(static_cast<jarray>(obj(1))));
        case 172: return c.ret(put_local(e->NewObjectArray(c.arg(1), static_cast<jclass>(obj(2)), obj(3))));
        case 173: return c.ret(put_local(e->GetObjectArrayElement(static_cast<jobjectArray>(obj(1)), c.arg(2))));
        case 174: return e->SetObjectArrayElement(static_cast<jobjectArray>(obj(1)), c.arg(2), obj(3));
        case 215: {
            auto cls = static_cast<jclass>(obj(1));
            bool ok = true;
            for (u32 i = 0; i < c.arg(3); i++) {
                u32 m = c.arg(2) + 12 * i;
                ok &= register_guest_native(e, cls, mem::h<char>(mem::rd32(m)), mem::h<char>(mem::rd32(m + 4)),
                                            mem::rd32(m + 8));
            }
            return c.ret(ok ? 0 : -1);
        }
        case 216: return c.ret(e->UnregisterNatives(static_cast<jclass>(obj(1))));
        case 217: return c.ret(e->MonitorEnter(obj(1)));
        case 218: return c.ret(e->MonitorExit(obj(1)));
        case 219:
            mem::wr32(c.arg(1), g_guest_vm);
            return c.ret(0);
        case 220: return e->GetStringRegion(static_cast<jstring>(obj(1)), c.arg(2), c.arg(3), mem::h<jchar>(c.arg(4)));
        case 221: return e->GetStringUTFRegion(static_cast<jstring>(obj(1)), c.arg(2), c.arg(3), str(4));
        case 222: {
            auto a = static_cast<jarray>(obj(1));
            return c.ret(pin_array(a, array_type(a), c.arg(2)));
        }
        case 223: return unpin_array(c.arg(2), c.arg(3));
        case 226: return c.ret(put_global(e->NewWeakGlobalRef(obj(1))));
        case 227: {
            e->DeleteWeakGlobalRef(obj(1));
            return drop_global(c.arg(1));
        }
        case 228: return c.ret(e->ExceptionCheck());
        case 229: {
            ArgReader ar{c, 2};
            return c.ret(put_local(e->NewDirectByteBuffer(mem::h(c.arg(1)), static_cast<jlong>(ar.dw()))));
        }
        case 230: return c.ret(mem::g(e->GetDirectBufferAddress(obj(1))));
        case 231: return c.ret64(e->GetDirectBufferCapacity(obj(1)));
        case 232: return c.ret(e->GetObjectRefType(obj(1)));
    }
    fatal("guest used unimplemented JNI function #%d", idx);
}

// ---------------------------------------------------------------- the guest JavaVM table
void vm_fn(Ctx& c, int idx) {
    switch (idx) {
        case 4: case 7: {
            JNIEnv* e;
            int r = idx == 4 ? g_vm->AttachCurrentThread(&e, nullptr) : g_vm->AttachCurrentThreadAsDaemon(&e, nullptr);
            t_env = e;
            mem::wr32(c.arg(1), g_guest_env);
            return c.ret(r);
        }
        case 5: {
            int r = g_vm->DetachCurrentThread();
            t_env = nullptr;
            return c.ret(r);
        }
        case 6: {
            JNIEnv* e;
            int r = g_vm->GetEnv(reinterpret_cast<void**>(&e), c.arg(2));
            if (r == JNI_OK) {
                t_env = e;
                mem::wr32(c.arg(1), g_guest_env);
            }
            return c.ret(r);
        }
    }
    fatal("guest used unimplemented JavaVM function #%d", idx);
}
template <int I> void vm_thunk(Ctx& c) { vm_fn(c, I); }

u32 build_table(const Handler* h, int n, const char* what) {
    u32 table = mem::g(mem::calloc(n, 4));
    for (int i = 0; i < n; i++) mem::wr32(table + 4 * i, make_thunk(h[i], what, true));
    u32 obj = mem::g(mem::calloc(1, 4));
    mem::wr32(obj, table);
    return obj;
}

// ---------------------------------------------------------------- Java -> guest natives
struct Words {
    std::vector<u32> w;
    void add(u32 v) { w.push_back(v); }
    void add64(u64 v) {
        if (w.size() & 1) w.push_back(0);
        w.push_back(static_cast<u32>(v));
        w.push_back(static_cast<u32>(v >> 32));
    }
};
template <typename T> void push_arg(Words& w, T v) {
    if constexpr (std::is_same_v<T, jfloat>) { u32 b; memcpy(&b, &v, 4); w.add(b); }
    else if constexpr (std::is_same_v<T, jlong>) w.add64(static_cast<u64>(v));
    else if constexpr (std::is_same_v<T, jdouble>) { u64 b; memcpy(&b, &v, 8); w.add64(b); }
    else if constexpr (std::is_pointer_v<T>) w.add(put_local(v));
    else w.add(static_cast<u32>(v));
}

template <typename R = void, typename... A>
R call_jni_export(const char* sym_name, JNIEnv* e, jclass clazz, A... a) {
    t_env = e;
    u32 fn = g_lib.sym(sym_name);
    if (!fn) {
        LOGW("missing guest JNI symbol %s", sym_name);
        if constexpr (!std::is_void_v<R>) return R{};
        else return;
    }
    LocalFrame frame;
    Words w;
    w.add(g_guest_env);
    w.add(put_local(clazz));
    (push_arg(w, a), ...);
    u64 r = call_guest(fn, w.w.data(), w.w.size());
    if constexpr (std::is_void_v<R>) return;
    else if constexpr (std::is_same_v<R, jboolean>) return static_cast<jboolean>(r & 0xff);
    else if constexpr (std::is_pointer_v<R>) return static_cast<R>(e->NewLocalRef(get(static_cast<u32>(r))));
    else return static_cast<R>(r);
}
}  // namespace

bool register_guest_native(JNIEnv*, jclass, const char* name, const char* sig, u32) {
    LOGW("guest called RegisterNatives(%s %s)", name, sig);
    return false;
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    env();
    mem::init();
    {
        char v[PROP_VALUE_MAX] = "";
        __system_property_get("debug.hambo.profile", v);
        g_profile = v[0] == '1' || v[0] == '2';
    }
    init_libc();
    init_gl();
    init_softfp_imports();

    static constexpr auto jni = jni_handlers(std::make_integer_sequence<int, 233>{});
    g_guest_env = build_table(jni.data(), jni.size(), "JNIEnv");
    static constexpr Handler vmh[8] = {nullptr, nullptr, nullptr, nullptr, &vm_thunk<4>, &vm_thunk<5>, &vm_thunk<6>, &vm_thunk<7>};
    g_guest_vm = build_table(vmh, 8, "JavaVM");

    Dl_info info;
    dladdr(reinterpret_cast<void*>(&JNI_OnLoad), &info);
    std::string path = info.dli_fname;
    path = path.substr(0, path.rfind('/') + 1) + "libhambo_guest.so";
    g_lib = load_guest(path.c_str());
    init_softfp(g_lib);
    // Patch 0x4c413c (ldr r1, [pc, #0x4c0]) -> 0xe59d1004 (ldr r1, [sp, #0x4]) so b2World::Step
    // uses the actual elapsed frame dt at any refresh rate (60/90/120/144Hz).
    if (mem::rd32(g_lib.base + 0x4c413c) == 0xe59f14c0u) {
        mem::wr32(g_lib.base + 0x4c413c, 0xe59d1004u);
        LOGI("patched b2World::Step at 0x4c413c to use dynamic frame dt for 60-144Hz");
    }
    // --- touch-path instrumentation ---------------------------------------------------
    // Java passes MtouchesBegin(id, x_px, y_px); guest MtouchDown (file VA 0x56974c)
    // converts px -> cocos points using REAL_DEVICE_SCREEN_W/H and SCREEN_WIDTH(=320*W/H).
    // Hook the display-info setters to record what the Java side thinks the size is.
    // Global slots are pointer-slots written by Java_..._MdisplayInfo:
    //   DEVICE_W 0x6be990  DEVICE_H 0x6bf4bc  SCREEN_W 0x6bf080  SCREEN_H 0x6bf53c
    //   aspectFactor 0x6bf36c  resetViewport-flag 0x6bf3d0  (all: *(u32*)slot)
    if (u32 a = g_lib.sym("Java_com_miniclip_nativeJNI_CocoJNI_MrealDisplayInfo"))
        hook_guest(a, [](Ctx& c) {
            LOGI("TOUCH realDisplayInfo w=%d h=%d", c.arg(2), c.arg(3));
        }, "MrealDisplayInfoHook");
    if (u32 a = g_lib.sym("Java_com_miniclip_nativeJNI_CocoJNI_MdisplayInfo"))
        hook_guest(a, [](Ctx& c) {
            u32 b = g_lib.base;
            LOGI("TOUCH displayInfo w=%d h=%d ptrDevW=%08x ptrDevH=%08x ptrSW=%08x ptrSH=%08x",
                 c.arg(2), c.arg(3), mem::rd32(b + 0x6be990), mem::rd32(b + 0x6bf4bc),
                 mem::rd32(b + 0x6bf080), mem::rd32(b + 0x6bf53c));
        }, "MdisplayInfoHook");
    hook_guest(g_lib.base + 0x56974c, [](Ctx& c) {
        float x, y;
        u32 xb = c.r[1], yb = c.r[2];
        memcpy(&x, &xb, 4);
        memcpy(&y, &yb, 4);
        u32 b = g_lib.base;
        auto via = [&](u32 slot) { return mem::rd32(mem::rd32(b + slot)); };
        LOGI("TOUCH MtouchDown x=%.2f y=%.2f | DEV=%ux%u SCREEN=%ux%u aspect=%.4f wr=%llu",
             x, y, via(0x6be990), via(0x6bf4bc), via(0x6bf080), via(0x6bf53c),
             static_cast<double>(via(0x6bf36c)), (unsigned long long)guest_write_count());
    }, "MtouchDownHook");
    if (u32 a = g_lib.sym("resetViewport"))
        hook_guest(a, [](Ctx& c) {
            u32 b = g_lib.base;
            auto via = [&](u32 slot) { return mem::rd32(mem::rd32(b + slot)); };
            LOGI("VIEWPORT resetViewport entered: DEV=%ux%u SCREEN=%ux%u aspect=%.4f flag=%d",
                 via(0x6be990), via(0x6bf4bc), via(0x6bf080), via(0x6bf53c),
                 static_cast<double>(via(0x6bf36c)), mem::rd32(mem::rd32(b + 0x6bf3d0)));
        }, "resetViewportHook");
    // Watch the actual layout variables (behind pointer slots) for rogue writes:
    {
        auto reg = [&](u32 slot, const char* tag) {
            u32 p = mem::rd32(g_lib.base + slot);
            LOGI("WATCH register slot %08x -> var %08x (%s)", slot, p, tag);
            watch_add(p, tag);
        };
        reg(0x6be990, "DeviceW");
        reg(0x6bf4bc, "DeviceH");
        reg(0x6bf080, "ScreenW");
        reg(0x6bf53c, "ScreenH");
        reg(0x6bf36c, "AspectF");
        reg(0x6bf3d0, "VpFlag");
    }
    // -----------------------------------------------------------------------------------
    run_guest_constructors(g_lib);

    u32 onload = g_lib.sym("JNI_OnLoad");
    if (!onload) fatal("guest has no JNI_OnLoad");
    LocalFrame frame;
    jint r = static_cast<jint>(call_guest(onload, {g_guest_vm, 0}));
    LOGI("guest JNI_OnLoad returned 0x%x", r);
    return r;
}

#define EXPORT_JNI(name, R, ...) \
    JNIEXPORT R JNICALL Java_com_miniclip_nativeJNI_CocoJNI_##name(JNIEnv* e, jclass c, ##__VA_ARGS__)

EXPORT_JNI(Maccel, void, jfloat a0, jfloat a1, jfloat a2, jlong a3) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_Maccel", e, c, a0, a1, a2, a3);
}
EXPORT_JNI(MadClicked, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MadClicked", e, c);
}
EXPORT_JNI(MbuttonRemoveAdsPressed, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MbuttonRemoveAdsPressed", e, c);
}
EXPORT_JNI(McallUpSellInApp, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_McallUpSellInApp", e, c);
}
EXPORT_JNI(McheckpointResponse, void, jstring a0, jint a1, jint a2) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_McheckpointResponse", e, c, a0, a1, a2);
}
EXPORT_JNI(MdatePickerClosed, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MdatePickerClosed", e, c);
}
EXPORT_JNI(MdatePickerResponce, void, jint a0, jint a1, jint a2) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MdatePickerResponce", e, c, a0, a1, a2);
}
EXPORT_JNI(MdisplayInfo, void, jint a0, jint a1) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MdisplayInfo", e, c, a0, a1);
}
EXPORT_JNI(Mend, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_Mend", e, c);
}
EXPORT_JNI(MfacebookLikeComplete, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MfacebookLikeComplete", e, c);
}
EXPORT_JNI(MfacebookLoginComplete, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MfacebookLoginComplete", e, c);
}
EXPORT_JNI(MfacebookRequestComplete, void, jint a0, jint a1, jbyteArray a2) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MfacebookRequestComplete", e, c, a0, a1, a2);
}
EXPORT_JNI(MfacebookShareCanceled, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MfacebookShareCanceled", e, c);
}
EXPORT_JNI(MfacebookShareComplete, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MfacebookShareComplete", e, c);
}
EXPORT_JNI(MinterstitialClosed, void, jint a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MinterstitialClosed", e, c, a0);
}
EXPORT_JNI(MkeyboardInputClosed, void, jstring a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MkeyboardInputClosed", e, c, a0);
}
EXPORT_JNI(MkeyboardInputResponse, void, jstring a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MkeyboardInputResponse", e, c, a0);
}
EXPORT_JNI(MlowMemory, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MlowMemory", e, c);
}
EXPORT_JNI(MnativeInitBitmap, void, jint a0, jint a1, jbyteArray a2) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MnativeInitBitmap", e, c, a0, a1, a2);
}
EXPORT_JNI(MnetworkTimeResponce, void, jint a0, jdouble a1, jint a2) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MnetworkTimeResponce", e, c, a0, a1, a2);
}
EXPORT_JNI(MnotifyAspectRatioChange, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MnotifyAspectRatioChange", e, c);
}
EXPORT_JNI(MonMessageBoxButtonPressed, void, jint a0, jint a1) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MonMessageBoxButtonPressed", e, c, a0, a1);
}
EXPORT_JNI(Mpause, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_Mpause", e, c);
}
EXPORT_JNI(MpreserveContext, void, jint a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MpreserveContext", e, c, a0);
}
EXPORT_JNI(MpressedBackButton, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MpressedBackButton", e, c);
}
EXPORT_JNI(MpressedKey, void, jint a0, jint a1) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MpressedKey", e, c, a0, a1);
}
EXPORT_JNI(MrealDisplayInfo, void, jint a0, jint a1) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MrealDisplayInfo", e, c, a0, a1);
}
EXPORT_JNI(Mrender, void) {
    static bool s_surface_rate_set = false;
    if (!s_surface_rate_set) {
        s_surface_rate_set = true;
        jclass cj = e->FindClass("com/miniclip/nativeJNI/cocojava");
        jfieldID f_glv = cj ? e->GetStaticFieldID(cj, "mGLView", "Lcom/miniclip/nativeJNI/ClearGLSurfaceView;") : nullptr;
        jobject glv = (cj && f_glv) ? e->GetStaticObjectField(cj, f_glv) : nullptr;
        if (glv) {
            jclass sv_cls = e->GetObjectClass(glv);
            jmethodID get_holder = e->GetMethodID(sv_cls, "getHolder", "()Landroid/view/SurfaceHolder;");
            jobject holder = get_holder ? e->CallObjectMethod(glv, get_holder) : nullptr;
            if (holder) {
                jclass h_cls = e->GetObjectClass(holder);
                jmethodID get_surf = e->GetMethodID(h_cls, "getSurface", "()Landroid/view/Surface;");
                jobject surf = get_surf ? e->CallObjectMethod(holder, get_surf) : nullptr;
                if (surf) {
                    jclass s_cls = e->GetObjectClass(surf);
                    jmethodID set_fr = e->GetMethodID(s_cls, "setFrameRate", "(FI)V");
                    if (set_fr) {
                        e->CallVoidMethod(surf, set_fr, 120.0f, 0);
                        LOGI("set SurfaceView Surface.setFrameRate(120.0f, 0)");
                    }
                }
            }
        }
        if (e->ExceptionCheck()) e->ExceptionClear();
    }
    count_frame();
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_Mrender", e, c);
}
EXPORT_JNI(MrestoreContext, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MrestoreContext", e, c);
}
EXPORT_JNI(Mresume, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_Mresume", e, c);
}
EXPORT_JNI(Mrun, void) {
    LOGI("Mrun starting guest engine");
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_Mrun", e, c);
    LOGI("Mrun completed");
}
EXPORT_JNI(MsetAPKPath, void, jstring a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetAPKPath", e, c, a0);
}
EXPORT_JNI(MsetAdLoadFailed, void, jint a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetAdLoadFailed", e, c, a0);
}
EXPORT_JNI(MsetAppVersionNumber, void, jstring a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetAppVersionNumber", e, c, a0);
}
EXPORT_JNI(MsetCountryCode, void, jstring a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetCountryCode", e, c, a0);
}
EXPORT_JNI(MsetDensity, void, jfloat a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetDensity", e, c, a0);
}
EXPORT_JNI(MsetFilesPath, void, jstring a0, jstring a1) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetFilesPath", e, c, a0, a1);
}
EXPORT_JNI(MsetGamePause, void, jint a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetGamePause", e, c, a0);
}
EXPORT_JNI(MsetInAppResponce, void, jint a0, jint a1, jint a2, jstring a3) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetInAppResponce", e, c, a0, a1, a2, a3);
}
EXPORT_JNI(MsetInAppResponce__IIILjava_1lang_1String_2, void, jint a0, jint a1, jint a2, jstring a3) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetInAppResponce", e, c, a0, a1, a2, a3);
}
EXPORT_JNI(MsetInAppResponce__IIILjava_1lang_1String_2Ljava_1lang_1String_2Ljava_1lang_1String_2, void,
           jint a0, jint a1, jint a2, jstring a3, jstring, jstring) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetInAppResponce", e, c, a0, a1, a2, a3);
}
EXPORT_JNI(MsetIsRetina, void, jint a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetIsRetina", e, c, a0);
}
EXPORT_JNI(MsetMainActivity, void, jobject a0) {
    // Request 120Hz display mode on the Activity window
    if (a0) {
        jclass act_cls = e->GetObjectClass(a0);
        jmethodID get_win = e->GetMethodID(act_cls, "getWindow", "()Landroid/view/Window;");
        jobject win = get_win ? e->CallObjectMethod(a0, get_win) : nullptr;
        if (win) {
            jclass win_cls = e->GetObjectClass(win);
            jmethodID get_attr = e->GetMethodID(win_cls, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
            jmethodID set_attr = e->GetMethodID(win_cls, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
            jobject lp = get_attr ? e->CallObjectMethod(win, get_attr) : nullptr;
            if (lp && set_attr) {
                jclass lp_cls = e->GetObjectClass(lp);
                jfieldID f_rate = e->GetFieldID(lp_cls, "preferredRefreshRate", "F");
                jfieldID f_mode = e->GetFieldID(lp_cls, "preferredDisplayModeId", "I");
                if (f_rate) e->SetFloatField(lp, f_rate, 120.0f);
                if (f_mode) e->SetIntField(lp, f_mode, 1);
                e->CallVoidMethod(win, set_attr, lp);
                LOGI("requested 120Hz preferredRefreshRate=120.0f preferredDisplayModeId=1 on Activity window");
            }
        }
        if (e->ExceptionCheck()) e->ExceptionClear();
    }
    // MsetMainActivity stores the jobject activity directly in a C global without NewGlobalRef!
    t_env = e;
    u32 fn = g_lib.sym("Java_com_miniclip_nativeJNI_CocoJNI_MsetMainActivity");
    if (!fn) return;
    LocalFrame frame;
    Words w;
    w.add(g_guest_env);
    w.add(put_global(c));
    w.add(put_global(a0));
    call_guest(fn, w.w.data(), w.w.size());
}
EXPORT_JNI(MsetSignInStatus, void, jint a0, jint a1, jint a2) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetSignInStatus", e, c, a0, a1, a2);
}
EXPORT_JNI(MsetTapjoyCoins, void, jint a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsetTapjoyCoins", e, c, a0);
}
EXPORT_JNI(MshowUpSellScreen, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MshowUpSellScreen", e, c);
}
EXPORT_JNI(MsimplePingResponce, void, jint a0, jint a1) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MsimplePingResponce", e, c, a0, a1);
}
EXPORT_JNI(MtouchesBegin, void, jint a0, jfloat a1, jfloat a2) {
    LOGI("TOUCH begin id=%d x=%.1f y=%.1f", a0, a1, a2);
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MtouchesBegin", e, c, a0, a1, a2);
}
EXPORT_JNI(MtouchesCancel, void, jintArray a0, jfloatArray a1, jfloatArray a2) {
    LOGI("TOUCH cancel n=%d", a0 ? e->GetArrayLength(a0) : -1);
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MtouchesCancel", e, c, a0, a1, a2);
}
EXPORT_JNI(MtouchesEnd, void, jint a0, jfloat a1, jfloat a2) {
    LOGI("TOUCH end id=%d x=%.1f y=%.1f", a0, a1, a2);
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MtouchesEnd", e, c, a0, a1, a2);
}
EXPORT_JNI(MtouchesMove, void, jintArray a0, jfloatArray a1, jfloatArray a2) {
    if (a0 && a1 && a2 && e->GetArrayLength(a0) > 0) {
        jfloat* xs = e->GetFloatArrayElements(a1, nullptr);
        jfloat* ys = e->GetFloatArrayElements(a2, nullptr);
        jint* ids = e->GetIntArrayElements(a0, nullptr);
        if (xs && ys && ids)
            LOGI("TOUCH move[0] id=%d x=%.1f y=%.1f", ids[0], xs[0], ys[0]);
        if (xs) e->ReleaseFloatArrayElements(a1, xs, JNI_ABORT);
        if (ys) e->ReleaseFloatArrayElements(a2, ys, JNI_ABORT);
        if (ids) e->ReleaseIntArrayElements(a0, ids, JNI_ABORT);
    }
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_MtouchesMove", e, c, a0, a1, a2);
}
EXPORT_JNI(NFcallAvailabilityChanged, void, jint a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_NFcallAvailabilityChanged", e, c, a0);
}
EXPORT_JNI(NFcallBoardDidAppear, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_NFcallBoardDidAppear", e, c);
}
EXPORT_JNI(NFcallBoardDidDisappear, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_NFcallBoardDidDisappear", e, c);
}
EXPORT_JNI(NFcallBoardWillAppear, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_NFcallBoardWillAppear", e, c);
}
EXPORT_JNI(NFcallBoardWillDisappear, void) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_NFcallBoardWillDisappear", e, c);
}
EXPORT_JNI(NFcallNrOfMessagesChanged, void, jint a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_NFcallNrOfMessagesChanged", e, c, a0);
}
EXPORT_JNI(NFcallNrOfUnreadMessagesChanged, void, jint a0) {
    call_jni_export("Java_com_miniclip_nativeJNI_CocoJNI_NFcallNrOfUnreadMessagesChanged", e, c, a0);
}
EXPORT_JNI(NFcallShouldShowUrgentMessage, jint) {
    return call_jni_export<jint>("Java_com_miniclip_nativeJNI_CocoJNI_NFcallShouldShowUrgentMessage", e, c);
}
JNIEXPORT void JNICALL Java_com_miniclip_utils_ReceiptValidator_MvalidateResponse(
    JNIEnv* e, jclass c, jint a0, jint a1, jstring a2, jint a3) {
    call_jni_export("Java_com_miniclip_utils_ReceiptValidator_MvalidateResponse", e, c, a0, a1, a2, a3);
}

}  // extern "C"
