// arm64 libhambo.so - Phase 3c: delegating skeleton
// JNI surface generated from smali (56 CocoJNI natives + validateResponse).
// Engine behaviour lives in engine.c (asset pipeline, zip, bplist, atlas GL).
#include <jni.h>
#include <android/log.h>
#include "engine.h"

#define LOG_TAG "HamboSkeleton"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static JavaVM* gJavaVM = NULL;
static jobject gActivity = NULL;
static int gW = 1280, gH = 720;
int gPW = 1920, gPH = 1080;   // shared with engine.c (extern)
static int gDispLogged = 0, gPDispLogged = 0, gActLogged = 0;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    gJavaVM = vm;
    LOGI("JNI_OnLoad bound; JavaVM=%p", (void*)vm);
    return JNI_VERSION_1_4;
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_Maccel(JNIEnv* env, jclass clazz, jfloat arg0, jfloat arg1, jfloat arg2, jlong arg3) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    LOGI("stub: accel");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MadClicked(JNIEnv* env, jclass clazz) {
    LOGI("stub: adClicked");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MbuttonRemoveAdsPressed(JNIEnv* env, jclass clazz) {
    LOGI("stub: buttonRemoveAdsPressed");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_McallUpSellInApp(JNIEnv* env, jclass clazz) {
    LOGI("stub: callUpSellInApp");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MdatePickerClosed(JNIEnv* env, jclass clazz) {
    LOGI("stub: datePickerClosed");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MdatePickerResponce(JNIEnv* env, jclass clazz, jint arg0, jint arg1, jint arg2) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    LOGI("stub: datePickerResponce");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MdisplayInfo(JNIEnv* env, jclass clazz, jint arg0, jint arg1) {
    (void)arg0;
    (void)arg1;
    gW = arg0; gH = arg1; if (!gDispLogged) { gDispLogged = 1; LOGI("logical display %dx%d", gW, gH); }
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MfacebookLikeComplete(JNIEnv* env, jclass clazz) {
    LOGI("stub: facebookLikeComplete");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MfacebookLoginComplete(JNIEnv* env, jclass clazz) {
    LOGI("stub: facebookLoginComplete");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MfacebookRequestComplete(JNIEnv* env, jclass clazz, jint arg0, jint arg1, jbyte* arg2) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    LOGI("stub: facebookRequestComplete");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MfacebookShareCanceled(JNIEnv* env, jclass clazz) {
    LOGI("stub: facebookShareCanceled");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MfacebookShareComplete(JNIEnv* env, jclass clazz) {
    LOGI("stub: facebookShareComplete");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MinterstitialClosed(JNIEnv* env, jclass clazz, jint arg0) {
    (void)arg0;
    LOGI("stub: interstitialClosed");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MkeyboardInputClosed(JNIEnv* env, jclass clazz, jstring arg0) {
    (void)arg0;
    LOGI("stub: keyboardInputClosed");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MkeyboardInputResponse(JNIEnv* env, jclass clazz, jstring arg0) {
    (void)arg0;
    LOGI("stub: keyboardInputResponse");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MlowMemory(JNIEnv* env, jclass clazz) {
    LOGI("stub: lowMemory");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MnativeInitBitmap(JNIEnv* env, jclass clazz, jint arg0, jint arg1, jbyte* arg2) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    engine_on_bitmap(env, arg0, arg1, arg2);
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MnetworkTimeResponce(JNIEnv* env, jclass clazz, jint arg0, jdouble arg1, jint arg2) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    LOGI("stub: networkTimeResponce");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MnotifyAspectRatioChange(JNIEnv* env, jclass clazz) {
    LOGI("stub: notifyAspectRatioChange");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MonMessageBoxButtonPressed(JNIEnv* env, jclass clazz, jint arg0, jint arg1) {
    (void)arg0;
    (void)arg1;
    LOGI("stub: onMessageBoxButtonPressed");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_Mpause(JNIEnv* env, jclass clazz) {
    LOGI("stub: pause");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MpreserveContext(JNIEnv* env, jclass clazz, jint arg0) {
    (void)arg0;
    LOGI("stub: preserveContext");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MpressedBackButton(JNIEnv* env, jclass clazz) {
    LOGI("stub: pressedBackButton");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MpressedKey(JNIEnv* env, jclass clazz, jint arg0, jint arg1) {
    (void)arg0;
    (void)arg1;
    LOGI("stub: pressedKey");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MrealDisplayInfo(JNIEnv* env, jclass clazz, jint arg0, jint arg1) {
    (void)arg0;
    (void)arg1;
    gPW = arg0; gPH = arg1; if (!gPDispLogged) { gPDispLogged = 1; LOGI("pixel display %dx%d", gPW, gPH); }
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_Mrender(JNIEnv* env, jclass clazz) {
    engine_render();
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MrestoreContext(JNIEnv* env, jclass clazz) {
    LOGI("stub: restoreContext");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_Mresume(JNIEnv* env, jclass clazz) {
    LOGI("stub: resume");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_Mrun(JNIEnv* env, jclass clazz) {
    if (gActivity) engine_on_run(env, gActivity);
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetAPKPath(JNIEnv* env, jclass clazz, jstring arg0) {
    (void)arg0;
    engine_set_apk_path(env, arg0);
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetAdLoadFailed(JNIEnv* env, jclass clazz, jint arg0) {
    (void)arg0;
    LOGI("stub: setAdLoadFailed");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetAppVersionNumber(JNIEnv* env, jclass clazz, jstring arg0) {
    (void)arg0;
    LOGI("stub: setAppVersionNumber");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetCountryCode(JNIEnv* env, jclass clazz, jstring arg0) {
    (void)arg0;
    LOGI("stub: setCountryCode");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetDensity(JNIEnv* env, jclass clazz, jfloat arg0) {
    (void)arg0;
    LOGI("stub: setDensity");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetFilesPath(JNIEnv* env, jclass clazz, jstring arg0, jstring arg1) {
    (void)arg0;
    (void)arg1;
    LOGI("stub: setFilesPath");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetGamePause(JNIEnv* env, jclass clazz, jint arg0) {
    (void)arg0;
    LOGI("stub: setGamePause");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetInAppResponce__IIILjava_1lang_1String_2(JNIEnv* env, jclass clazz, jint arg0, jint arg1, jint arg2, jstring arg3) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    LOGI("stub: setInAppResponce");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetInAppResponce__IIILjava_1lang_1String_2Ljava_1lang_1String_2Ljava_1lang_1String_2(JNIEnv* env, jclass clazz, jint arg0, jint arg1, jint arg2, jstring arg3, jstring arg4, jstring arg5) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    LOGI("stub: setInAppResponce");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetIsRetina(JNIEnv* env, jclass clazz, jint arg0) {
    (void)arg0;
    LOGI("stub: setIsRetina");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetMainActivity(JNIEnv* env, jclass clazz, jobject arg0) {
    (void)arg0;
    gActivity = (*env)->NewGlobalRef(env, arg0); if (!gActLogged) { gActLogged = 1; LOGI("activity bound"); }
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetSignInStatus(JNIEnv* env, jclass clazz, jint arg0, jint arg1, jint arg2) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    LOGI("stub: setSignInStatus");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsetTapjoyCoins(JNIEnv* env, jclass clazz, jint arg0) {
    (void)arg0;
    LOGI("stub: setTapjoyCoins");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MshowUpSellScreen(JNIEnv* env, jclass clazz) {
    LOGI("stub: showUpSellScreen");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MsimplePingResponce(JNIEnv* env, jclass clazz, jint arg0, jint arg1) {
    (void)arg0;
    (void)arg1;
    LOGI("stub: simplePingResponce");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MtouchesBegin(JNIEnv* env, jclass clazz, jint arg0, jfloat arg1, jfloat arg2) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    LOGI("stub: touchesBegin");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MtouchesCancel(JNIEnv* env, jclass clazz, jint* arg0, jfloat* arg1, jfloat* arg2) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    LOGI("stub: touchesCancel");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MtouchesEnd(JNIEnv* env, jclass clazz, jint arg0, jfloat arg1, jfloat arg2) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    LOGI("stub: touchesEnd");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_MtouchesMove(JNIEnv* env, jclass clazz, jint* arg0, jfloat* arg1, jfloat* arg2) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    LOGI("stub: touchesMove");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_NFcallAvailabilityChanged(JNIEnv* env, jclass clazz, jint arg0) {
    (void)arg0;
    LOGI("stub: FcallAvailabilityChanged");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_NFcallBoardDidAppear(JNIEnv* env, jclass clazz) {
    LOGI("stub: FcallBoardDidAppear");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_NFcallBoardDidDisappear(JNIEnv* env, jclass clazz) {
    LOGI("stub: FcallBoardDidDisappear");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_NFcallBoardWillAppear(JNIEnv* env, jclass clazz) {
    LOGI("stub: FcallBoardWillAppear");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_NFcallBoardWillDisappear(JNIEnv* env, jclass clazz) {
    LOGI("stub: FcallBoardWillDisappear");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_NFcallNrOfMessagesChanged(JNIEnv* env, jclass clazz, jint arg0) {
    (void)arg0;
    LOGI("stub: FcallNrOfMessagesChanged");
}
JNIEXPORT void JNICALL Java_com_miniclip_nativeJNI_CocoJNI_NFcallNrOfUnreadMessagesChanged(JNIEnv* env, jclass clazz, jint arg0) {
    (void)arg0;
    LOGI("stub: FcallNrOfUnreadMessagesChanged");
}
JNIEXPORT jint JNICALL Java_com_miniclip_nativeJNI_CocoJNI_NFcallShouldShowUrgentMessage(JNIEnv* env, jclass clazz) {
    LOGI("stub: FcallShouldShowUrgentMessage");
    return 0;
}
JNIEXPORT void JNICALL Java_com_miniclip_utils_ReceiptValidator_MvalidateResponse(JNIEnv* env, jclass clazz, jint arg0, jint arg1, jstring arg2, jint arg3) {
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    LOGI("stub: validateResponse");
}
