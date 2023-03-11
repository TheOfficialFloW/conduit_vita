/* main.c -- The Conduit HD .so loader
 *
 * Copyright (C) 2023 Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <math.h>

#include <errno.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "fios.h"
#include "so_util.h"
#include "jni_patch.h"
#include "movie_patch.h"
#include "mpg123_patch.h"
#include "openal_patch.h"
#include "sha1.h"

#include "libc_bridge.h"

int sceLibcHeapSize = MEMORY_SCELIBC_MB * 1024 * 1024;
int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

unsigned int _oal_thread_priority = 64;
unsigned int _oal_thread_affinity = 0x10000;

SceTouchPanelInfo panelInfoFront;

so_module conduit_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n) {
  return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n) {
  return sceClibMemmove(dest, src, n);
}

void *__wrap_memset(void *s, int c, size_t n) {
  return sceClibMemset(s, c, n);
}

int debugPrintf(char *text, ...) {
#ifdef DEBUG
  va_list list;
  char string[512];

  va_start(list, text);
  vsprintf(string, text, list);
  va_end(list);

  SceUID fd = sceIoOpen("ux0:data/conduit_log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, string, strlen(string));
    sceIoClose(fd);
  }
#endif
  return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
  va_list list;
  char string[512];

  va_start(list, fmt);
  vsprintf(string, fmt, list);
  va_end(list);

  printf("%s: %s\n", tag, string);
#endif
  return 0;
}

int ret0(void) {
  return 0;
}

int ret1(void) {
  return 1;
}

int OS_SystemChip(void) {
  return 4;
}

int OS_ScreenGetWidth(void) {
  return SCREEN_W;
}

int OS_ScreenGetHeight(void) {
  return SCREEN_H;
}

// only used for NVEventAppMain
int pthread_create_fake(int r0, int r1, int r2, void *arg) {
  int (* func)() = *(void **)(arg + 4);
  return func();
}

int pthread_mutex_init_fake(SceKernelLwMutexWork **work) {
  *work = (SceKernelLwMutexWork *)memalign(8, sizeof(SceKernelLwMutexWork));
  if (sceKernelCreateLwMutex(*work, "mutex", 0, 0, NULL) < 0)
    return -1;
  return 0;
}

int pthread_mutex_destroy_fake(SceKernelLwMutexWork **work) {
  if (sceKernelDeleteLwMutex(*work) < 0)
    return -1;
  free(*work);
  return 0;
}

int pthread_mutex_lock_fake(SceKernelLwMutexWork **work) {
  if (!*work)
    pthread_mutex_init_fake(work);
  if (sceKernelLockLwMutex(*work, 1, NULL) < 0)
    return -1;
  return 0;
}

int pthread_mutex_unlock_fake(SceKernelLwMutexWork **work) {
  if (sceKernelUnlockLwMutex(*work, 1) < 0)
    return -1;
  return 0;
}

int sem_init_fake(int *uid) {
  *uid = sceKernelCreateSema("sema", 0, 0, 0x7fffffff, NULL);
  if (*uid < 0)
    return -1;
  return 0;
}

int sem_post_fake(int *uid) {
  if (sceKernelSignalSema(*uid, 1) < 0)
    return -1;
  return 0;
}

int sem_wait_fake(int *uid) {
  if (sceKernelWaitSema(*uid, 1, NULL) < 0)
    return -1;
  return 0;
}

int sem_destroy_fake(int *uid) {
  if (sceKernelDeleteSema(*uid) < 0)
    return -1;
  return 0;
}

int thread_stub(SceSize args, uintptr_t *argp) {
  int (* func)(void *arg) = (void *)argp[0];
  void *arg = (void *)argp[1];
  char *out = (char *)argp[2];
  out[0x41] = 1; // running
  func(arg);
  return sceKernelExitDeleteThread(0);
}

// RevGraph with cpu 2 and priority 3
// RevMain with cpu 1 and priority 3
void *OS_ThreadLaunch(int (* func)(), void *arg, int cpu, char *name, int unused, int priority) {
  int vita_priority;
  int vita_affinity;

  switch (priority) {
    case 0:
      vita_priority = 67;
      break;
    case 1:
      vita_priority = 66;
      break;
    case 2:
      vita_priority = 65;
      break;
    case 3:
      vita_priority = 64;
      break;
    default:
      vita_priority = 0x10000100;
      break;
  }

  switch (cpu) {
    case 0:
      vita_affinity = 0x10000;
      break;
    case 1:
      vita_affinity = 0x20000;
      break;
    case 2:
      vita_affinity = 0x40000;
      break;
    default:
      vita_affinity = 0;
      break;
  }

  SceUID thid = sceKernelCreateThread(name, (SceKernelThreadEntry)thread_stub, vita_priority, 128 * 1024, 0, vita_affinity, NULL);
  if (thid >= 0) {
    char *out = malloc(0x48);
    *(int *)(out + 0x24) = thid;

    uintptr_t args[3];
    args[0] = (uintptr_t)func;
    args[1] = (uintptr_t)arg;
    args[2] = (uintptr_t)out;
    sceKernelStartThread(thid, sizeof(args), args);

    return out;
  }

  return NULL;
}

void OS_ThreadWait(void *thread) {
  if (thread)
    sceKernelWaitThreadEnd(*(int *)(thread + 0x24), NULL, NULL);
}

int OSGetThreadSpecific() {
  printf("OSGetThreadSpecific called\n");
  return 0;
}

int OSGetCurrentThread() {
  printf("OSGetCurrentThread called\n");
  return 0;
}

char *IsInitGraphics;
int (* initGraphics)(void);

int ProcessEvents(void) {
  if (!*IsInitGraphics)
    initGraphics();
  return 0;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

void patch_game(void) {
  IsInitGraphics = (char *)so_symbol(&conduit_mod, "IsInitGraphics");
  initGraphics = (void *)so_symbol(&conduit_mod, "_Z12initGraphicsv");

  *(int *)so_symbol(&conduit_mod, "IsAndroidPaused") = 0;

  hook_addr(so_symbol(&conduit_mod, "__cxa_guard_acquire"), (uintptr_t)&__cxa_guard_acquire);
  hook_addr(so_symbol(&conduit_mod, "__cxa_guard_release"), (uintptr_t)&__cxa_guard_release);

  hook_addr(so_symbol(&conduit_mod, "_Z24NVThreadGetCurrentJNIEnvv"), (uintptr_t)NVThreadGetCurrentJNIEnv);

  hook_addr(so_symbol(&conduit_mod, "_ZN10TouchSense8instanceEv"), (uintptr_t)ret0);
  hook_addr(so_symbol(&conduit_mod, "_Z14ass_CueHapticsP6CStratPK6ASLVar"), (uintptr_t)ret0);

  hook_addr(so_symbol(&conduit_mod, "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"), (uintptr_t)OS_ThreadLaunch);
  hook_addr(so_symbol(&conduit_mod, "_Z13OS_ThreadWaitPv"), (uintptr_t)OS_ThreadWait);
  hook_addr(so_symbol(&conduit_mod, "OSResumeThread"), (uintptr_t)ret0);

  hook_addr(so_symbol(&conduit_mod, "OSSetThreadSpecific"), (uintptr_t)ret0);
  hook_addr(so_symbol(&conduit_mod, "OSGetThreadSpecific"), (uintptr_t)OSGetThreadSpecific);
  hook_addr(so_symbol(&conduit_mod, "OSGetCurrentThread"), (uintptr_t)OSGetCurrentThread);

  hook_addr(so_symbol(&conduit_mod, "_Z13ProcessEventsb"), (uintptr_t)ProcessEvents);

  hook_addr(so_symbol(&conduit_mod, "_Z20AND_SystemInitializev"), (uintptr_t)ret0);

  hook_addr(so_symbol(&conduit_mod, "_Z14AND_FileUpdated"), (uintptr_t)ret0);
  hook_addr(so_symbol(&conduit_mod, "_Z17AND_BillingUpdateb"), (uintptr_t)ret0);

  hook_addr(so_symbol(&conduit_mod, "_Z21OS_BillingIsPurchasedPKc"), (uintptr_t)ret1);

  hook_addr(so_symbol(&conduit_mod, "_Z13OS_SystemChipv"), (uintptr_t)OS_SystemChip);

  hook_addr(so_symbol(&conduit_mod, "_Z17OS_ScreenGetWidthv"), (uintptr_t)OS_ScreenGetWidth);
  hook_addr(so_symbol(&conduit_mod, "_Z18OS_ScreenGetHeightv"), (uintptr_t)OS_ScreenGetHeight);

  hook_addr(so_symbol(&conduit_mod, "_Z9NvAPKOpenPKc"), (uintptr_t)ret0);

  hook_addr(so_symbol(&conduit_mod, "_Z21ass_GetNumScreenshotsP6CStratP6ASLVarPKS1_"), (uintptr_t)ret0);
  hook_addr(so_symbol(&conduit_mod, "_Z17ass_GetScreenshotP6CStratP6ASLVarPKS1_"), (uintptr_t)ret0);
  hook_addr(so_symbol(&conduit_mod, "_Z21ass_ScreenshotCaptureP6CStratPK6ASLVar"), (uintptr_t)ret0);
  hook_addr(so_symbol(&conduit_mod, "_Z22ass_ScreenshotClearAllP6CStratPK6ASLVar"), (uintptr_t)ret0);

  hook_addr(so_symbol(&conduit_mod, "_Z12ass_RateGameP6CStratPK6ASLVar"), (uintptr_t)ret0);
}

void glShaderSourceHook(GLuint shader, GLsizei count, const GLchar **string, const GLint *length) {
  uint32_t sha1[5];
  SHA1_CTX ctx;

  sha1_init(&ctx);
  sha1_update(&ctx, (uint8_t *)*string, *length);
  sha1_final(&ctx, (uint8_t *)sha1);

  char glsl_path[1024];
  snprintf(glsl_path, sizeof(glsl_path), "%s/%08x.glsl", GLSL_PATH, sha1[0]);

  char cg_path[1024];
  snprintf(cg_path, sizeof(cg_path), "%s/%08x.cg.gxp", SHADERS_PATH, sha1[0]);

  FILE *file;

  file = sceLibcBridge_fopen(cg_path, "rb");
  if (!file) {
    file = sceLibcBridge_fopen(glsl_path, "w");
    if (file) {
      sceLibcBridge_fwrite(*string, 1, *length, file);
      sceLibcBridge_fclose(file);
    }

    if (strstr(*string, "gl_FragColor"))
      snprintf(cg_path, sizeof(cg_path), "%s/bf999cdf.cg.gxp", SHADERS_PATH);
    else
      snprintf(cg_path, sizeof(cg_path), "%s/0539a408.cg.gxp", SHADERS_PATH);

    file = sceLibcBridge_fopen(cg_path, "rb");
    if (!file)
      debugPrintf("Error loading dummy shader\n");
  } else {
    // printf("Using %s\n", cg_path);
  }

  sceLibcBridge_fseek(file, 0, SEEK_END);
  int size = sceLibcBridge_ftell(file);
  sceLibcBridge_fseek(file, 0, SEEK_SET);

  char *buf = malloc(size);
  sceLibcBridge_fread(buf, 1, size, file);
  sceLibcBridge_fclose(file);

  glShaderBinary(1, &shader, 0, buf, size);

  free(buf);
}

void glCompileShaderHook(GLuint shader) {
	// glCompileShader(shader);
}

FILE *fopen_hook(const char *filename, const char *mode) {
  // Avoid opening these files because they are in the .obb.
  char *p = strrchr(filename, '.');
  if (p && (strcmp(p, ".mp3") ==  0 || strcmp(p, ".mp4") == 0 || strcmp(p, ".gcs") == 0 || strcmp(p, ".gcm") == 0))
    return NULL;
  return sceLibcBridge_fopen(filename, mode);
}

extern void *__aeabi_atexit;

static FILE __sF_fake[0x100][3];

static so_default_dynlib default_dynlib[] = {
  // { "ImmVibeCloseDevice", (uintptr_t)&ImmVibeCloseDevice },
  // { "ImmVibeGetEffectState", (uintptr_t)&ImmVibeGetEffectState },
  // { "ImmVibeGetIVTEffectIndexFromName", (uintptr_t)&ImmVibeGetIVTEffectIndexFromName },
  // { "ImmVibeInitialize2", (uintptr_t)&ImmVibeInitialize2 },
  // { "ImmVibeOpenDevice", (uintptr_t)&ImmVibeOpenDevice },
  // { "ImmVibePlayUHLEffect", (uintptr_t)&ImmVibePlayUHLEffect },
  // { "ImmVibeStopPlayingEffect", (uintptr_t)&ImmVibeStopPlayingEffect },
  // { "ImmVibeTerminate", (uintptr_t)&ImmVibeTerminate },
  // { "_ITM_deregisterTMCloneTable", (uintptr_t)&_ITM_deregisterTMCloneTable },
  // { "_ITM_registerTMCloneTable", (uintptr_t)&_ITM_registerTMCloneTable },
  // { "_Jv_RegisterClasses", (uintptr_t)&_Jv_RegisterClasses },
  { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  // { "__assert2", (uintptr_t)&__assert2 },
  // { "__deregister_frame_info", (uintptr_t)&__deregister_frame_info },
  { "__errno", (uintptr_t)&__errno },
  // { "__gnu_Unwind_Find_exidx", (uintptr_t)&__gnu_Unwind_Find_exidx },
  // { "__register_frame_info", (uintptr_t)&__register_frame_info },
  { "__sF", (uintptr_t)&__sF_fake },
  { "abort", (uintptr_t)&abort },
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin },
  { "asinf", (uintptr_t)&asinf },
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "atoi", (uintptr_t)&atoi },
  { "bsearch", (uintptr_t)&bsearch },
  { "calloc", (uintptr_t)&calloc },
  { "ceil", (uintptr_t)&ceil },
  { "ceilf", (uintptr_t)&ceilf },
  // { "clock_gettime", (uintptr_t)&clock_gettime },
  // { "close", (uintptr_t)&close },
  // { "closedir", (uintptr_t)&closedir },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
  // { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
  // { "eglQueryString", (uintptr_t)&eglQueryString },
  { "exit", (uintptr_t)&exit },
  { "exp", (uintptr_t)&exp },
  { "expf", (uintptr_t)&expf },
  { "fclose", (uintptr_t)&sceLibcBridge_fclose },
  // { "fflush", (uintptr_t)&fflush },
  // { "fgetc", (uintptr_t)&fgetc },
  // { "fgets", (uintptr_t)&fgets },
  { "floor", (uintptr_t)&floor },
  { "floorf", (uintptr_t)&floorf },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "fopen", (uintptr_t)&fopen_hook },
  // { "fprintf", (uintptr_t)&fprintf },
  // { "fputc", (uintptr_t)&fputc },
  // { "fputs", (uintptr_t)&fputs },
  { "fread", (uintptr_t)&sceLibcBridge_fread },
  { "free", (uintptr_t)&free },
  { "fseek", (uintptr_t)&sceLibcBridge_fseek },
  { "ftell", (uintptr_t)&sceLibcBridge_ftell },
  { "fwrite", (uintptr_t)&sceLibcBridge_fwrite },
  { "getenv", (uintptr_t)&getenv },
  // { "gettid", (uintptr_t)&gettid },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&ret0 },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShaderHook },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&ret0 },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDepthRangef", (uintptr_t)&glDepthRangef },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFramebufferRenderbuffer", (uintptr_t)&ret0 },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&ret0 },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glGetVertexAttribPointerv", (uintptr_t)&glGetVertexAttribPointerv },
  { "glGetVertexAttribiv", (uintptr_t)&glGetVertexAttribiv },
  { "glLinkProgram", (uintptr_t)&glLinkProgram },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&ret0 },
  { "glShaderSource", (uintptr_t)&glShaderSourceHook },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameterf", (uintptr_t)&glTexParameterf },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform2f", (uintptr_t)&glUniform2f },
  { "glUniform3f", (uintptr_t)&glUniform3f },
  { "glUniform3fv", (uintptr_t)&glUniform3fv },
  { "glUniform4f", (uintptr_t)&glUniform4f },
  { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glViewport", (uintptr_t)&glViewport },
  // { "gzclose", (uintptr_t)&gzclose },
  // { "gzgets", (uintptr_t)&gzgets },
  // { "gzopen", (uintptr_t)&gzopen },
  { "isspace", (uintptr_t)&isspace },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "lrand48", (uintptr_t)&lrand48 },
  { "lseek", (uintptr_t)&lseek },
  { "malloc", (uintptr_t)&malloc },
  { "memchr", (uintptr_t)&sceClibMemchr },
  { "memcmp", (uintptr_t)&sceClibMemcmp },
  { "memcpy", (uintptr_t)&sceClibMemcpy },
  { "memmove", (uintptr_t)&sceClibMemmove },
  { "memset", (uintptr_t)&sceClibMemset },
  { "mkdir", (uintptr_t)&mkdir },
  // { "nanosleep", (uintptr_t)&nanosleep },
  // { "open", (uintptr_t)&open },
  // { "opendir", (uintptr_t)&opendir },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "printf", (uintptr_t)&printf },
  // { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy },
  // { "pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam },
  // { "pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize },
  // { "pthread_attr_init", (uintptr_t)&pthread_attr_init },
  // { "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam },
  // { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast },
  // { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy },
  { "pthread_cond_init", (uintptr_t)&ret0 },
  // { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal },
  // { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait },
  // { "pthread_cond_timeout_np", (uintptr_t)&pthread_cond_timeout_np },
  // { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait },
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  // { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  // { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_key_create", (uintptr_t)&ret0 },
  { "pthread_key_delete", (uintptr_t)&ret0 },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_mutexattr_init", (uintptr_t)&ret0 },
  { "pthread_mutexattr_settype", (uintptr_t)&ret0 },
  // { "pthread_once", (uintptr_t)&pthread_once },
  // { "pthread_self", (uintptr_t)&pthread_self },
  // { "pthread_setname_np", (uintptr_t)&pthread_setname_np },
  // { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam },
  { "pthread_setspecific", (uintptr_t)&ret0 },
  { "putchar", (uintptr_t)&putchar },
  { "puts", (uintptr_t)&puts },
  { "qsort", (uintptr_t)&qsort },
  // { "raise", (uintptr_t)&raise },
  { "read", (uintptr_t)&read },
  // { "readdir", (uintptr_t)&readdir },
  { "realloc", (uintptr_t)&realloc },
  // { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max },
  // { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min },
  // { "sched_yield", (uintptr_t)&sched_yield },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  // { "sem_getvalue", (uintptr_t)&sem_getvalue },
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  // { "sem_trywait", (uintptr_t)&sem_trywait },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sigaction", (uintptr_t)&ret0 },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strerror", (uintptr_t)&strerror },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
  { "strncat", (uintptr_t)&sceClibStrncat },
  { "strncmp", (uintptr_t)&sceClibStrncmp },
  { "strncpy", (uintptr_t)&sceClibStrncpy },
  { "strrchr", (uintptr_t)&sceClibStrrchr },
  { "strstr", (uintptr_t)&sceClibStrstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtol", (uintptr_t)&strtol },
  // { "syscall", (uintptr_t)&syscall },
  { "tan", (uintptr_t)&tan },
  { "tanf", (uintptr_t)&tanf },
  { "toupper", (uintptr_t)&toupper },
  { "usleep", (uintptr_t)&usleep },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "write", (uintptr_t)&write },
};

int check_kubridge(void) {
  int search_unk[2];
  return _vshKernelSearchModuleByName("kubridge", search_unk);
}

int file_exists(const char *path) {
  SceIoStat stat;
  return sceIoGetstat(path, &stat) >= 0;
}

int main(int argc, char *argv[]) {
  sceKernelChangeThreadPriority(0, 127);
  sceKernelChangeThreadCpuAffinityMask(0, 0x10000);

  sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panelInfoFront);

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  sceIoMkdir(GLSL_PATH, 0777);

  if (check_kubridge() < 0)
    fatal_error("Error kubridge.skprx is not installed.");

  if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
    fatal_error("Error libshacccg.suprx is not installed.");

  if (so_load(&conduit_mod, SO_PATH, LOAD_ADDRESS) < 0)
    fatal_error("Error could not load %s.", SO_PATH);

  so_relocate(&conduit_mod);
  so_resolve(&conduit_mod, default_dynlib, sizeof(default_dynlib), 0);

  patch_mpg123();
  patch_openal();
  patch_game();
  patch_movie();
  so_flush_caches(&conduit_mod);

  so_initialize(&conduit_mod);

  if (fios_init() < 0)
    fatal_error("Error could not initialize fios.");

  vglSetupGarbageCollector(127, 0x10000);
  vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);

  movie_setup_player();

  jni_load();

  return 0;
}
