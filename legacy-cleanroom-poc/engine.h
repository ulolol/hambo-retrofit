#ifndef HAMBO_ENGINE_H
#define HAMBO_ENGINE_H
#include <jni.h>

// Phase 3c engine module for the arm64 rebuild.
// Proves the asset protocol: APK-zip access via MsetAPKPath, binary-plist
// (bplist00) parsing of unpack/* sprite atlases, multi-texture upload through
// the game's own Java loader (cocojava.loadTexture), and GL atlas rendering.

void engine_on_bitmap(JNIEnv* env, jint w, jint h, jbyteArray pix);   // one texture upload
void engine_on_run(JNIEnv* env, jobject activity);                    // boot: request sheets + parse atlases
void engine_set_apk_path(JNIEnv* env, jstring path);
void engine_render(void);                                             // called per frame (GL context current)

#endif
