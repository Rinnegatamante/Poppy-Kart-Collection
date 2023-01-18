/* main.c -- Poppy Kart .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2022 Rinnegatamante
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_net.h>
#include <SLES/OpenSLES.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>

#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "sha1.h"

#ifdef DEBUG
#define dlog printf
#else
#define dlog
#endif

#define CONFIG_FILE_PATH "app0:settings.cfg"

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

int enable_dlcs = 0;

void loadConfig(void) {
  char buffer[30];
  int value;

  FILE *config = fopen(CONFIG_FILE_PATH, "r");

  if (config) {
    while (EOF != fscanf(config, "%[^=]=%d\n", buffer, &value)) {
      if (strcmp("enable_dlcs", buffer) == 0) enable_dlcs = value;
    }
    fclose(config);
  }
}

static char data_path[256];

static char fake_vm[0x1000];
static char fake_env[0x1000];

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

so_module poppykart_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n) {
	return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n) {
	return sceClibMemmove(dest, src, n);
}

void *__wrap_memset(void *s, int c, size_t n) {
	return sceClibMemset(s, c, n);
}

char *getcwd_hook(char *buf, size_t size) {
	strcpy(buf, data_path);
	return buf;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
	*memptr = memalign(alignment, size);
	return 0;
}

int debugPrintf(char *text, ...) {
#ifdef DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, text);
	vsprintf(string, text, list);
	va_end(list);

	SceUID fd = sceIoOpen("ux0:data/poppykart_log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, string, strlen(string));
		sceIoClose(fd);
	}
#endif
	return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOG] %s: %s\n", tag, string);
	return 0;
}

int __android_log_write(int prio, const char *tag, const char *fmt, ...) {
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOGW] %s: %s\n", tag, string);
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list) {
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOGV] %s: %s\n", tag, string);
	return 0;
}

int ret0(void) {
	return 0;
}

int ret1(void) {
	return 1;
}

int pthread_mutex_init_fake(pthread_mutex_t **uid, const pthread_mutexattr_t *mutexattr) {
	pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
	if (!m)
		return -1;

	const int recursive = (mutexattr && *(const int *)mutexattr == 1);
	*m = recursive ? PTHREAD_RECURSIVE_MUTEX_INITIALIZER : PTHREAD_MUTEX_INITIALIZER;

	int ret = pthread_mutex_init(m, mutexattr);
	if (ret < 0) {
		free(m);
		return -1;
	}

	*uid = m;

	return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
	if (uid && *uid && (uintptr_t)*uid > 0x8000) {
		pthread_mutex_destroy(*uid);
		free(*uid);
		*uid = NULL;
	}
	return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
	int ret = 0;
	if (!*uid) {
		ret = pthread_mutex_init_fake(uid, NULL);
	} else if ((uintptr_t)*uid == 0x4000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	} else if ((uintptr_t)*uid == 0x8000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	}
	if (ret < 0)
		return ret;
	return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
	int ret = 0;
	if (!*uid) {
		ret = pthread_mutex_init_fake(uid, NULL);
	} else if ((uintptr_t)*uid == 0x4000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	} else if ((uintptr_t)*uid == 0x8000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	}
	if (ret < 0)
		return ret;
	return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
	int ret = 0;
	if (!*uid) {
		ret = pthread_mutex_init_fake(uid, NULL);
	} else if ((uintptr_t)*uid == 0x4000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	} else if ((uintptr_t)*uid == 0x8000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	}
	if (ret < 0)
		return ret;
	return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
	pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
	if (!c)
		return -1;

	*c = PTHREAD_COND_INITIALIZER;

	int ret = pthread_cond_init(c, NULL);
	if (ret < 0) {
		free(c);
		return -1;
	}

	*cnd = c;

	return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
	if (cnd && *cnd) {
		pthread_cond_destroy(*cnd);
		free(*cnd);
		*cnd = NULL;
	}
	return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_timedwait(*cnd, *mtx, t);
}

int clock_gettime_hook(int clk_id, struct timespec *t) {
	struct timeval now;
	int rv = gettimeofday(&now, NULL);
	if (rv)
		return rv;
	t->tv_sec = now.tv_sec;
	t->tv_nsec = now.tv_usec * 1000;

	return 0;
}

int pthread_cond_timedwait_relative_np_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, struct timespec *ts) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	
	if (ts != NULL) {
		struct timespec ct;
		clock_gettime_hook(0, &ct);
		ts->tv_sec += ct.tv_sec;
		ts->tv_nsec += ct.tv_nsec;
	}
	
	pthread_cond_timedwait(*cnd, *mtx, ts); // FIXME
	return 0;
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
	return pthread_create(thread, NULL, entry, arg);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
	if (!once_control || !init_routine)
		return -1;
	if (__sync_lock_test_and_set(once_control, 1) == 0)
		(*init_routine)();
	return 0;
}

int GetCurrentThreadId(void) {
	return sceKernelGetThreadId();
}

extern void *__aeabi_ldiv0;

int GetEnv(void *vm, void **env, int r2) {
	*env = fake_env;
	return 0;
}

void throw_exc(char **str, void *a, int b) {
	printf("throwing %s\n", *str);
}

FILE *fopen_hook(char *fname, char *mode) {
	FILE *f;
	char real_fname[256];
	//printf("fopen(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%sassets/%s", data_path, fname);
		f = fopen(real_fname, mode);
	} else {
		f = fopen(fname, mode);
	}
	return f;
}

int open_hook(const char *fname, int flags, mode_t mode) {
	int f;
	char real_fname[256];
	printf("open(%s)\n", fname);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%sassets/%s", data_path, fname);
		f = open(real_fname, flags, mode);
	} else {
		f = open(fname, flags, mode);
	}
	return f;
}

FILE *android_fopen(char *fname, char *type, int a3) {
	return fopen_hook(fname, type);
}

int GetGameBackupDirectory(char *str) {
	strcpy(str, data_path);
	return 0;
}

void patch_game(void) {
	hook_addr(so_symbol(&poppykart_mod, "_ZN9CMonetize21Analytics_SendRequestEP8CContextSt3mapIPc17CHttpVariantValueSt4lessIS3_ESaISt4pairIKS3_S4_EEE"), (uintptr_t)&ret0);
	if (enable_dlcs)
		hook_addr(so_symbol(&poppykart_mod, "_ZN13CAchievements17GetIsGameUnlockedEv"), (uintptr_t)&ret1);
	hook_addr(so_symbol(&poppykart_mod, "__cxa_throw"), (uintptr_t)&throw_exc);
	hook_addr(so_symbol(&poppykart_mod, "android_fopen"), (uintptr_t)&android_fopen);
	hook_addr(so_symbol(&poppykart_mod, "_ZN9CMonetize22GetGameBackupDirectoryEPc"), (uintptr_t)&GetGameBackupDirectory);
}

extern void *__aeabi_atexit;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_dadd;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_uldivmod;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_call_unexpected;
extern void *__gnu_unwind_frame;
extern void *__stack_chk_fail;
int open(const char *pathname, int flags);

static int __stack_chk_guard_fake = 0x42424242;

static FILE __sF_fake[0x1000][3];

int stat_hook(const char *pathname, void *statbuf) {
	//dlog("stat(%s)\n", pathname);
	struct stat st;
	int res = stat(pathname, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return memalign(length, 0x1000);
}

int munmap(void *addr, size_t length) {
	free(addr);
	return 0;
}

int fstat_hook(int fd, void *statbuf) {
	struct stat st;
	int res = fstat(fd, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

char *basename(char *path) {
	char *p = path;
	if (strlen(path) == 1)
		return path;
	char *slash = strstr(p, "/");
	while (slash) {
		p = slash + 1;
		slash = strstr(p, "/");
	}
	return p;
}

void *sceClibMemclr(void *dst, SceSize len) {
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
	return sceClibMemset(dst, ch, len);
}

void *Android_JNI_GetEnv() {
	return fake_env;
}

char *SDL_AndroidGetExternalStoragePath() {
	return data_path;
}

char *SDL_AndroidGetInternalStoragePath() {
	return data_path;
}

int g_SDL_BufferGeometry_w;
int g_SDL_BufferGeometry_h;

void abort_hook() {
	//dlog("ABORT CALLED!!!\n");
	uint8_t *p = NULL;
	p[0] = 1;
}

int ret99() {
	return 99;
}

int chdir_hook(const char *path) {
	return 0;
}

void glShaderSource_fake(GLuint shader, GLsizei count, const GLchar **string, const GLint *length) {
	dlog("Shader with count %d\n", count);
}

static so_default_dynlib gl_hook[] = {
	{"glShaderSource", (uintptr_t)&glShaderSource_fake},
	{"glCompileShader", (uintptr_t)&ret0},
};
static size_t gl_numhook = sizeof(gl_hook) / sizeof(*gl_hook);

void *SDL_GL_GetProcAddress_fake(const char *symbol) {
	dlog("looking for symbol %s\n", symbol);
	for (size_t i = 0; i < gl_numhook; ++i) {
		if (!strcmp(symbol, gl_hook[i].symbol)) {
			return (void *)gl_hook[i].func;
		}
	}
	void *r = vglGetProcAddress(symbol);
	if (!r) {
		dlog("Cannot find symbol %s\n", symbol);
	}
	return r;
}

#define SCE_ERRNO_MASK 0xFF

#define DT_DIR 4
#define DT_REG 8

struct android_dirent {
	char pad[18];
	unsigned char d_type;
	char d_name[256];
};

typedef struct {
	SceUID uid;
	struct android_dirent dir;
} android_DIR;

int closedir_fake(android_DIR *dirp) {
	if (!dirp || dirp->uid < 0) {
		errno = EBADF;
		return -1;
	}

	int res = sceIoDclose(dirp->uid);
	dirp->uid = -1;

	free(dirp);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return -1;
	}

	errno = 0;
	return 0;
}

android_DIR *opendir_fake(const char *dirname) {
	printf("opendir(%s)\n", dirname);
	SceUID uid = sceIoDopen(dirname);

	if (uid < 0) {
		errno = uid & SCE_ERRNO_MASK;
		return NULL;
	}

	android_DIR *dirp = calloc(1, sizeof(android_DIR));

	if (!dirp) {
		sceIoDclose(uid);
		errno = ENOMEM;
		return NULL;
	}

	dirp->uid = uid;

	errno = 0;
	return dirp;
}

struct android_dirent *readdir_fake(android_DIR *dirp) {
	if (!dirp) {
		errno = EBADF;
		return NULL;
	}

	SceIoDirent sce_dir;
	int res = sceIoDread(dirp->uid, &sce_dir);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return NULL;
	}

	if (res == 0) {
		errno = 0;
		return NULL;
	}

	dirp->dir.d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG;
	strcpy(dirp->dir.d_name, sce_dir.d_name);
	return &dirp->dir;
}

SDL_Surface *IMG_Load_hook(const char *file) {
	char real_fname[256];
	printf("loading %s\n", file);
	if (strncmp(file, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", data_path, file);
		return IMG_Load(real_fname);
	}
	return IMG_Load(file);
}

SDL_RWops *SDL_RWFromFile_hook(const char *fname, const char *mode) {
	SDL_RWops *f;
	char real_fname[256];
	printf("SDL_RWFromFile(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", data_path, fname);
		//printf("SDL_RWFromFile patched to %s\n", real_fname);
		f = SDL_RWFromFile(real_fname, mode);
	} else {
		f = SDL_RWFromFile(fname, mode);
	}
	return f;
}

Mix_Music *Mix_LoadMUS_hook(const char *fname) {
	Mix_Music *f;
	char real_fname[256];
	printf("Mix_LoadMUS(%s)\n", fname);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", data_path, fname);
		f = Mix_LoadMUS(real_fname);
	} else {
		f = Mix_LoadMUS(fname);
	}
	return f;
}

int Mix_OpenAudio_hook(int frequency, Uint16 format, int channels, int chunksize) {
	return Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024);
}

extern void SDL_ResetKeyboard(void);

size_t __strlen_chk(const char *s, size_t s_len) {
    return strlen(s);
}

SDL_Window *SDL_CreateWindow_hook(const char *title, int x, int y, int w, int h, Uint32 flags) {
	return SDL_CreateWindow("Poppy Kart", 0, 0, SCREEN_W, SCREEN_H, flags);
}

uint8_t *SDL_GetKeyboardState_hook(int *numkeys) {
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);
	uint8_t *res = SDL_GetKeyboardState(numkeys);
	res[SDL_SCANCODE_LEFT] = ((pad.buttons & SCE_CTRL_LEFT) || pad.lx < 80) ? 1 : 0;
	res[SDL_SCANCODE_RIGHT] = ((pad.buttons & SCE_CTRL_RIGHT) || pad.lx > 150) ? 1 : 0;
	res[SDL_SCANCODE_UP] = (pad.buttons & SCE_CTRL_CROSS) ? 1 : 0;
	res[SDL_SCANCODE_DOWN] = (pad.buttons & SCE_CTRL_SQUARE) ? 1 : 0;
	res[SDL_SCANCODE_SPACE] = (pad.buttons & SCE_CTRL_CIRCLE) ? 1 : 0;
	res[SDL_SCANCODE_P] = (pad.buttons & SCE_CTRL_START) ? 1 : 0;
	return res;
}

static so_default_dynlib default_dynlib[] = {
	{ "opendir", (uintptr_t)&opendir_fake },
	{ "readdir", (uintptr_t)&readdir_fake },
	{ "closedir", (uintptr_t)&closedir_fake },
	{ "g_SDL_BufferGeometry_w", (uintptr_t)&g_SDL_BufferGeometry_w },
	{ "g_SDL_BufferGeometry_h", (uintptr_t)&g_SDL_BufferGeometry_h },
	{ "__aeabi_memclr", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr4", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr8", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove4", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memmove8", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memset", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset4", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset8", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
	{ "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
	{ "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
	{ "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
	{ "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
	{ "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
	{ "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
	{ "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
	{ "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
	{ "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
	{ "__android_log_print", (uintptr_t)&__android_log_print },
	{ "__android_log_vprint", (uintptr_t)&__android_log_vprint },
	{ "__android_log_write", (uintptr_t)&__android_log_write },
	{ "__cxa_atexit", (uintptr_t)&__cxa_atexit },
	{ "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
	{ "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
	{ "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
	{ "__cxa_finalize", (uintptr_t)&__cxa_finalize },
	{ "__errno", (uintptr_t)&__errno },
	{ "__strlen_chk", (uintptr_t)&__strlen_chk },
	{ "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
	{ "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
	{ "dl_unwind_find_exidx", (uintptr_t)&ret0 },
	{ "__sF", (uintptr_t)&__sF_fake },
	{ "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
	{ "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
	{ "_ctype_", (uintptr_t)&BIONIC_ctype_},
	{ "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_},
	{ "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_},
	{ "abort", (uintptr_t)&abort_hook },
	{ "access", (uintptr_t)&access },
	{ "acos", (uintptr_t)&acos },
	{ "acosh", (uintptr_t)&acosh },
	{ "asctime", (uintptr_t)&asctime },
	{ "acosf", (uintptr_t)&acosf },
	{ "asin", (uintptr_t)&asin },
	{ "asinh", (uintptr_t)&asinh },
	{ "asinf", (uintptr_t)&asinf },
	{ "atan", (uintptr_t)&atan },
	{ "atanh", (uintptr_t)&atanh },
	{ "atan2", (uintptr_t)&atan2 },
	{ "atan2f", (uintptr_t)&atan2f },
	{ "atanf", (uintptr_t)&atanf },
	{ "atoi", (uintptr_t)&atoi },
	{ "atol", (uintptr_t)&atol },
	{ "atoll", (uintptr_t)&atoll },
	{ "basename", (uintptr_t)&basename },
	// { "bind", (uintptr_t)&bind },
	{ "bsd_signal", (uintptr_t)&ret0 },
	{ "bsearch", (uintptr_t)&bsearch },
	{ "btowc", (uintptr_t)&btowc },
	{ "calloc", (uintptr_t)&calloc },
	{ "ceil", (uintptr_t)&ceil },
	{ "ceilf", (uintptr_t)&ceilf },
	{ "chdir", (uintptr_t)&chdir_hook },
	{ "clearerr", (uintptr_t)&clearerr },
	{ "clock", (uintptr_t)&clock },
	{ "clock_gettime", (uintptr_t)&clock_gettime_hook },
	{ "close", (uintptr_t)&close },
	{ "cos", (uintptr_t)&cos },
	{ "cosf", (uintptr_t)&cosf },
	{ "cosh", (uintptr_t)&cosh },
	{ "crc32", (uintptr_t)&crc32 },
	{ "deflate", (uintptr_t)&deflate },
	{ "deflateEnd", (uintptr_t)&deflateEnd },
	{ "deflateInit_", (uintptr_t)&deflateInit_ },
	{ "deflateInit2_", (uintptr_t)&deflateInit2_ },
	{ "deflateReset", (uintptr_t)&deflateReset },
	{ "dlopen", (uintptr_t)&ret0 },
	// { "dlsym", (uintptr_t)&dlsym_hook },
	{ "exit", (uintptr_t)&exit },
	{ "exp", (uintptr_t)&exp },
	{ "exp2", (uintptr_t)&exp2 },
	{ "expf", (uintptr_t)&expf },
	{ "fabsf", (uintptr_t)&fabsf },
	{ "fclose", (uintptr_t)&fclose },
	{ "fcntl", (uintptr_t)&ret0 },
	// { "fdopen", (uintptr_t)&fdopen },
	{ "ferror", (uintptr_t)&ferror },
	// { "fflush", (uintptr_t)&fflush },
	// { "fgets", (uintptr_t)&fgets },
	{ "floor", (uintptr_t)&floor },
	{ "floorf", (uintptr_t)&floorf },
	{ "fmod", (uintptr_t)&fmod },
	{ "fmodf", (uintptr_t)&fmodf },
	{ "fopen", (uintptr_t)&fopen_hook },
	{ "open", (uintptr_t)&open_hook },
	{ "fprintf", (uintptr_t)&fprintf },
	{ "fputc", (uintptr_t)&fputc },
	// { "fputwc", (uintptr_t)&fputwc },
	// { "fputs", (uintptr_t)&fputs },
	{ "fread", (uintptr_t)&fread },
	{ "free", (uintptr_t)&free },
	{ "frexp", (uintptr_t)&frexp },
	{ "frexpf", (uintptr_t)&frexpf },
	// { "fscanf", (uintptr_t)&fscanf },
	{ "fseek", (uintptr_t)&fseek },
	{ "fseeko", (uintptr_t)&fseeko },
	{ "fstat", (uintptr_t)&fstat },
	{ "ftell", (uintptr_t)&ftell },
	{ "ftello", (uintptr_t)&ftello },
	// { "ftruncate", (uintptr_t)&ftruncate },
	{ "fwrite", (uintptr_t)&fwrite },
	{ "getc", (uintptr_t)&getc },
	{ "getpid", (uintptr_t)&ret0 },
	{ "getcwd", (uintptr_t)&getcwd_hook },
	{ "getenv", (uintptr_t)&ret0 },
	{ "getwc", (uintptr_t)&getwc },
	{ "gettimeofday", (uintptr_t)&gettimeofday },
	{ "gzopen", (uintptr_t)&gzopen },
	{ "inflate", (uintptr_t)&inflate },
	{ "inflateEnd", (uintptr_t)&inflateEnd },
	{ "inflateInit_", (uintptr_t)&inflateInit_ },
	{ "inflateInit2_", (uintptr_t)&inflateInit2_ },
	{ "inflateReset", (uintptr_t)&inflateReset },
	{ "isascii", (uintptr_t)&isascii },
	{ "isalnum", (uintptr_t)&isalnum },
	{ "isalpha", (uintptr_t)&isalpha },
	{ "iscntrl", (uintptr_t)&iscntrl },
	{ "isdigit", (uintptr_t)&isdigit },
	{ "islower", (uintptr_t)&islower },
	{ "ispunct", (uintptr_t)&ispunct },
	{ "isprint", (uintptr_t)&isprint },
	{ "isspace", (uintptr_t)&isspace },
	{ "isupper", (uintptr_t)&isupper },
	{ "iswalpha", (uintptr_t)&iswalpha },
	{ "iswcntrl", (uintptr_t)&iswcntrl },
	{ "iswctype", (uintptr_t)&iswctype },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswlower", (uintptr_t)&iswlower },
	{ "iswprint", (uintptr_t)&iswprint },
	{ "iswpunct", (uintptr_t)&iswpunct },
	{ "iswspace", (uintptr_t)&iswspace },
	{ "iswupper", (uintptr_t)&iswupper },
	{ "iswxdigit", (uintptr_t)&iswxdigit },
	{ "isxdigit", (uintptr_t)&isxdigit },
	{ "ldexp", (uintptr_t)&ldexp },
	{ "ldexpf", (uintptr_t)&ldexpf },
	// { "listen", (uintptr_t)&listen },
	{ "localtime", (uintptr_t)&localtime },
	{ "localtime_r", (uintptr_t)&localtime_r },
	{ "log", (uintptr_t)&log },
	{ "logf", (uintptr_t)&logf },
	{ "log10", (uintptr_t)&log10 },
	{ "log10f", (uintptr_t)&log10f },
	{ "longjmp", (uintptr_t)&longjmp },
	{ "lrand48", (uintptr_t)&lrand48 },
	{ "lrint", (uintptr_t)&lrint },
	{ "lrintf", (uintptr_t)&lrintf },
	{ "lseek", (uintptr_t)&lseek },
	{ "malloc", (uintptr_t)&malloc },
	{ "mbrtowc", (uintptr_t)&mbrtowc },
	{ "memalign", (uintptr_t)&memalign },
	{ "memchr", (uintptr_t)&sceClibMemchr },
	{ "memcmp", (uintptr_t)&sceClibMemcmp },
	{ "memcpy", (uintptr_t)&sceClibMemcpy },
	{ "memmove", (uintptr_t)&sceClibMemmove },
	{ "memset", (uintptr_t)&sceClibMemset },
	{ "mkdir", (uintptr_t)&mkdir },
	// { "mmap", (uintptr_t)&mmap},
	// { "munmap", (uintptr_t)&munmap},
	{ "modf", (uintptr_t)&modf },
	{ "modff", (uintptr_t)&modff },
	// { "poll", (uintptr_t)&poll },
	{ "pow", (uintptr_t)&pow },
	{ "powf", (uintptr_t)&powf },
	{ "printf", (uintptr_t)&printf },
	{ "pthread_attr_destroy", (uintptr_t)&ret0 },
	{ "pthread_attr_init", (uintptr_t)&ret0 },
	{ "pthread_attr_setdetachstate", (uintptr_t)&ret0 },
	{ "pthread_attr_setstacksize", (uintptr_t)&ret0 },
	{ "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},
	{ "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
	{ "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},
	{ "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
	{ "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
	{ "pthread_cond_timedwait_relative_np", (uintptr_t)&pthread_cond_timedwait_relative_np_fake}, // FIXME
	{ "pthread_create", (uintptr_t)&pthread_create_fake },
	{ "pthread_getschedparam", (uintptr_t)&pthread_getschedparam },
	{ "pthread_getspecific", (uintptr_t)&pthread_getspecific },
	{ "pthread_key_create", (uintptr_t)&pthread_key_create },
	{ "pthread_key_delete", (uintptr_t)&pthread_key_delete },
	{ "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
	{ "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
	{ "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
	{ "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
	{ "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
	{ "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy},
	{ "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init},
	{ "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype},
	{ "pthread_once", (uintptr_t)&pthread_once_fake },
	{ "pthread_self", (uintptr_t)&pthread_self },
	{ "pthread_setname_np", (uintptr_t)&ret0 },
	{ "pthread_getschedparam", (uintptr_t)&pthread_getschedparam },
	{ "pthread_setschedparam", (uintptr_t)&pthread_setschedparam },
	{ "pthread_setspecific", (uintptr_t)&pthread_setspecific },
	{ "sched_get_priority_min", (uintptr_t)&ret0 },
	{ "sched_get_priority_max", (uintptr_t)&ret99 },
	{ "putc", (uintptr_t)&putc },
	{ "puts", (uintptr_t)&puts },
	{ "putwc", (uintptr_t)&putwc },
	{ "qsort", (uintptr_t)&qsort },
	{ "rand", (uintptr_t)&rand },
	{ "read", (uintptr_t)&read },
	{ "realpath", (uintptr_t)&realpath },
	{ "realloc", (uintptr_t)&realloc },
	// { "recv", (uintptr_t)&recv },
	{ "roundf", (uintptr_t)&roundf },
	{ "rint", (uintptr_t)&rint },
	{ "rintf", (uintptr_t)&rintf },
	// { "send", (uintptr_t)&send },
	// { "sendto", (uintptr_t)&sendto },
	{ "setenv", (uintptr_t)&ret0 },
	{ "setjmp", (uintptr_t)&setjmp },
	{ "setlocale", (uintptr_t)&ret0 },
	// { "setsockopt", (uintptr_t)&setsockopt },
	{ "setvbuf", (uintptr_t)&setvbuf },
	{ "sin", (uintptr_t)&sin },
	{ "sinf", (uintptr_t)&sinf },
	{ "sinh", (uintptr_t)&sinh },
	//{ "sincos", (uintptr_t)&sincos },
	{ "snprintf", (uintptr_t)&snprintf },
	// { "socket", (uintptr_t)&socket },
	{ "sprintf", (uintptr_t)&sprintf },
	{ "sqrt", (uintptr_t)&sqrt },
	{ "sqrtf", (uintptr_t)&sqrtf },
	{ "srand", (uintptr_t)&srand },
	{ "srand48", (uintptr_t)&srand48 },
	{ "sscanf", (uintptr_t)&sscanf },
	{ "stat", (uintptr_t)&stat_hook },
	{ "strcasecmp", (uintptr_t)&strcasecmp },
	{ "strcasestr", (uintptr_t)&strstr },
	{ "strcat", (uintptr_t)&strcat },
	{ "strchr", (uintptr_t)&strchr },
	{ "strcmp", (uintptr_t)&sceClibStrcmp },
	{ "strcoll", (uintptr_t)&strcoll },
	{ "strcpy", (uintptr_t)&strcpy },
	{ "strcspn", (uintptr_t)&strcspn },
	{ "strdup", (uintptr_t)&strdup },
	{ "strerror", (uintptr_t)&strerror },
	{ "strftime", (uintptr_t)&strftime },
	{ "strlcpy", (uintptr_t)&strlcpy },
	{ "strlen", (uintptr_t)&strlen },
	{ "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
	{ "strncat", (uintptr_t)&sceClibStrncat },
	{ "strncmp", (uintptr_t)&sceClibStrncmp },
	{ "strncpy", (uintptr_t)&sceClibStrncpy },
	{ "strpbrk", (uintptr_t)&strpbrk },
	{ "strrchr", (uintptr_t)&sceClibStrrchr },
	{ "strstr", (uintptr_t)&sceClibStrstr },
	{ "strtod", (uintptr_t)&strtod },
	{ "strtol", (uintptr_t)&strtol },
	{ "strtoul", (uintptr_t)&strtoul },
	{ "strtoll", (uintptr_t)&strtoll },
	{ "strtoull", (uintptr_t)&strtoull },
	{ "strxfrm", (uintptr_t)&strxfrm },
	{ "sysconf", (uintptr_t)&ret0 },
	{ "tan", (uintptr_t)&tan },
	{ "tanf", (uintptr_t)&tanf },
	{ "tanh", (uintptr_t)&tanh },
	{ "time", (uintptr_t)&time },
	{ "tolower", (uintptr_t)&tolower },
	{ "toupper", (uintptr_t)&toupper },
	{ "towlower", (uintptr_t)&towlower },
	{ "towupper", (uintptr_t)&towupper },
	{ "ungetc", (uintptr_t)&ungetc },
	{ "ungetwc", (uintptr_t)&ungetwc },
	{ "usleep", (uintptr_t)&usleep },
	{ "vfprintf", (uintptr_t)&vfprintf },
	{ "vprintf", (uintptr_t)&vprintf },
	{ "vsnprintf", (uintptr_t)&vsnprintf },
	{ "vsprintf", (uintptr_t)&vsprintf },
	{ "vswprintf", (uintptr_t)&vswprintf },
	{ "wcrtomb", (uintptr_t)&wcrtomb },
	{ "wcscoll", (uintptr_t)&wcscoll },
	{ "wcscmp", (uintptr_t)&wcscmp },
	{ "wcsncpy", (uintptr_t)&wcsncpy },
	{ "wcsftime", (uintptr_t)&wcsftime },
	{ "wcslen", (uintptr_t)&wcslen },
	{ "wcsxfrm", (uintptr_t)&wcsxfrm },
	{ "wctob", (uintptr_t)&wctob },
	{ "wctype", (uintptr_t)&wctype },
	{ "wmemchr", (uintptr_t)&wmemchr },
	{ "wmemcmp", (uintptr_t)&wmemcmp },
	{ "wmemcpy", (uintptr_t)&wmemcpy },
	{ "wmemmove", (uintptr_t)&wmemmove },
	{ "wmemset", (uintptr_t)&wmemset },
	{ "write", (uintptr_t)&write },
	// { "writev", (uintptr_t)&writev },
	{ "glColor4f", (uintptr_t)&glColor4f },
	{ "glClearColor", (uintptr_t)&glClearColor },
	{ "glAlphaFunc", (uintptr_t)&glAlphaFunc },
	{ "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
	{ "glTexImage2D", (uintptr_t)&glTexImage2D },
	{ "glDeleteTextures", (uintptr_t)&glDeleteTextures },
	{ "glGenTextures", (uintptr_t)&glGenTextures },
	{ "glBindTexture", (uintptr_t)&glBindTexture },
	{ "glTexParameteri", (uintptr_t)&glTexParameteri },
	{ "glGetError", (uintptr_t)&glGetError },
	{ "glMatrixMode", (uintptr_t)&glMatrixMode },
	{ "glMultMatrixf", (uintptr_t)&glMultMatrixf },
	{ "glLoadIdentity", (uintptr_t)&glLoadIdentity },
	{ "glScalef", (uintptr_t)&glScalef },
	{ "glRotatef", (uintptr_t)&glRotatef },
	{ "glPushMatrix", (uintptr_t)&glPushMatrix },
	{ "glPopMatrix", (uintptr_t)&glPopMatrix },
	{ "glClear", (uintptr_t)&glClear },
	{ "glOrthof", (uintptr_t)&glOrthof },
	{ "glLineWidth", (uintptr_t)&glLineWidth },
	{ "glViewport", (uintptr_t)&glViewport },
	{ "glTranslatef", (uintptr_t)&glTranslatef },
	{ "glScissor", (uintptr_t)&glScissor },
	{ "glIsTexture", (uintptr_t)&glIsTexture },
	{ "glEnable", (uintptr_t)&glEnable },
	{ "glDisable", (uintptr_t)&glDisable },
	{ "glEnableClientState", (uintptr_t)&glEnableClientState },
	{ "glDisableClientState", (uintptr_t)&glDisableClientState },
	{ "glBlendFunc", (uintptr_t)&glBlendFunc },
	{ "glColorPointer", (uintptr_t)&glColorPointer },
	{ "glVertexPointer", (uintptr_t)&glVertexPointer },
	{ "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
	{ "glDrawArrays", (uintptr_t)&glDrawArrays },
	{ "glDrawElements", (uintptr_t)&glDrawElements },
	{ "SDL_AndroidGetActivityClass", (uintptr_t)&ret0 },
	{ "SDL_IsTextInputActive", (uintptr_t)&SDL_IsTextInputActive },
	{ "SDL_GameControllerEventState", (uintptr_t)&SDL_GameControllerEventState },
	{ "SDL_WarpMouseInWindow", (uintptr_t)&SDL_WarpMouseInWindow },
	{ "SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath },
	{ "SDL_AndroidGetInternalStoragePath", (uintptr_t)&SDL_AndroidGetInternalStoragePath },
	{ "SDL_Android_Init", (uintptr_t)&ret1 },
	{ "SDL_AddTimer", (uintptr_t)&SDL_AddTimer },
	{ "SDL_CondSignal", (uintptr_t)&SDL_CondSignal },
	{ "SDL_CondWait", (uintptr_t)&SDL_CondWait },
	{ "SDL_ConvertSurfaceFormat", (uintptr_t)&SDL_ConvertSurfaceFormat },
	{ "SDL_CreateCond", (uintptr_t)&SDL_CreateCond },
	{ "SDL_CreateMutex", (uintptr_t)&SDL_CreateMutex },
	{ "SDL_CreateRenderer", (uintptr_t)&SDL_CreateRenderer },
	{ "SDL_CreateRGBSurface", (uintptr_t)&SDL_CreateRGBSurface },
	{ "SDL_CreateTexture", (uintptr_t)&SDL_CreateTexture },
	{ "SDL_CreateTextureFromSurface", (uintptr_t)&SDL_CreateTextureFromSurface },
	{ "SDL_CreateThread", (uintptr_t)&SDL_CreateThread },
	{ "SDL_CreateWindow", (uintptr_t)&SDL_CreateWindow_hook },
	{ "SDL_Delay", (uintptr_t)&SDL_Delay },
	{ "SDL_DestroyMutex", (uintptr_t)&SDL_DestroyMutex },
	{ "SDL_DestroyRenderer", (uintptr_t)&SDL_DestroyRenderer },
	{ "SDL_DestroyTexture", (uintptr_t)&SDL_DestroyTexture },
	{ "SDL_DestroyWindow", (uintptr_t)&SDL_DestroyWindow },
	{ "SDL_FillRect", (uintptr_t)&SDL_FillRect },
	{ "SDL_FreeSurface", (uintptr_t)&SDL_FreeSurface },
	{ "SDL_GetCurrentDisplayMode", (uintptr_t)&SDL_GetCurrentDisplayMode },
	{ "SDL_GetDisplayMode", (uintptr_t)&SDL_GetDisplayMode },
	{ "SDL_GetError", (uintptr_t)&SDL_GetError },
	{ "SDL_GetModState", (uintptr_t)&SDL_GetModState },
	{ "SDL_GetMouseState", (uintptr_t)&SDL_GetMouseState },
	{ "SDL_GetRGBA", (uintptr_t)&SDL_GetRGBA },
	{ "SDL_GameControllerAddMappingsFromRW", (uintptr_t)&SDL_GameControllerAddMappingsFromRW },
	{ "SDL_GetNumDisplayModes", (uintptr_t)&SDL_GetNumDisplayModes },
	{ "SDL_GetRendererInfo", (uintptr_t)&SDL_GetRendererInfo },
	{ "SDL_GetTextureBlendMode", (uintptr_t)&SDL_GetTextureBlendMode },
	{ "SDL_GetPrefPath", (uintptr_t)&SDL_GetPrefPath },
	{ "SDL_GetTextureColorMod", (uintptr_t)&SDL_GetTextureColorMod },
	{ "SDL_GetTicks", (uintptr_t)&SDL_GetTicks },
	{ "SDL_GetVersion", (uintptr_t)&SDL_GetVersion },
	{ "SDL_GL_BindTexture", (uintptr_t)&SDL_GL_BindTexture },
	{ "SDL_GL_GetCurrentContext", (uintptr_t)&SDL_GL_GetCurrentContext },
	{ "SDL_GL_MakeCurrent", (uintptr_t)&SDL_GL_MakeCurrent },
	{ "SDL_GL_SetAttribute", (uintptr_t)&SDL_GL_SetAttribute },
	{ "SDL_Init", (uintptr_t)&SDL_Init },
	{ "SDL_InitSubSystem", (uintptr_t)&SDL_InitSubSystem },
	{ "SDL_IntersectRect", (uintptr_t)&SDL_IntersectRect },
	{ "SDL_LockMutex", (uintptr_t)&SDL_LockMutex },
	{ "SDL_LockSurface", (uintptr_t)&SDL_LockSurface },
	{ "SDL_Log", (uintptr_t)&ret0 },
	{ "SDL_LogError", (uintptr_t)&ret0 },
	{ "SDL_LogSetPriority", (uintptr_t)&ret0 },
	{ "SDL_MapRGB", (uintptr_t)&SDL_MapRGB },
	{ "SDL_JoystickInstanceID", (uintptr_t)&SDL_JoystickInstanceID },
	{ "SDL_GameControllerGetAxis", (uintptr_t)&SDL_GameControllerGetAxis },
	{ "SDL_MinimizeWindow", (uintptr_t)&SDL_MinimizeWindow },
	{ "SDL_PeepEvents", (uintptr_t)&SDL_PeepEvents },
	{ "SDL_PumpEvents", (uintptr_t)&SDL_PumpEvents },
	{ "SDL_PushEvent", (uintptr_t)&SDL_PushEvent },
	{ "SDL_PollEvent", (uintptr_t)&SDL_PollEvent },
	{ "SDL_QueryTexture", (uintptr_t)&SDL_QueryTexture },
	{ "SDL_Quit", (uintptr_t)&SDL_Quit },
	{ "SDL_RemoveTimer", (uintptr_t)&SDL_RemoveTimer },
	{ "SDL_RenderClear", (uintptr_t)&SDL_RenderClear },
	{ "SDL_RenderCopy", (uintptr_t)&SDL_RenderCopy },
	{ "SDL_RenderFillRect", (uintptr_t)&SDL_RenderFillRect },
	{ "SDL_RenderPresent", (uintptr_t)&SDL_RenderPresent },
	{ "SDL_RWFromFile", (uintptr_t)&SDL_RWFromFile_hook },
	{ "SDL_RWread", (uintptr_t)&SDL_RWread },
	{ "SDL_RWwrite", (uintptr_t)&SDL_RWwrite },
	{ "SDL_RWclose", (uintptr_t)&SDL_RWclose },
	{ "SDL_RWsize", (uintptr_t)&SDL_RWsize },
	{ "SDL_RWFromMem", (uintptr_t)&SDL_RWFromMem },
	{ "SDL_SetColorKey", (uintptr_t)&SDL_SetColorKey },
	{ "SDL_SetEventFilter", (uintptr_t)&SDL_SetEventFilter },
	{ "SDL_SetHint", (uintptr_t)&SDL_SetHint },
	{ "SDL_SetMainReady_REAL", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_SetRenderDrawBlendMode", (uintptr_t)&SDL_SetRenderDrawBlendMode },
	{ "SDL_SetRenderDrawColor", (uintptr_t)&SDL_SetRenderDrawColor },
	{ "SDL_SetRenderTarget", (uintptr_t)&SDL_SetRenderTarget },
	{ "SDL_SetTextureBlendMode", (uintptr_t)&SDL_SetTextureBlendMode },
	{ "SDL_SetTextureColorMod", (uintptr_t)&SDL_SetTextureColorMod },
	{ "SDL_ShowCursor", (uintptr_t)&SDL_ShowCursor },
	{ "SDL_ShowSimpleMessageBox", (uintptr_t)&SDL_ShowSimpleMessageBox },
	{ "SDL_StartTextInput", (uintptr_t)&SDL_StartTextInput },
	{ "SDL_StopTextInput", (uintptr_t)&SDL_StopTextInput },
	{ "SDL_strdup", (uintptr_t)&SDL_strdup },
	{ "SDL_UnlockMutex", (uintptr_t)&SDL_UnlockMutex },
	{ "SDL_UnlockSurface", (uintptr_t)&SDL_UnlockSurface },
	{ "SDL_UpdateTexture", (uintptr_t)&SDL_UpdateTexture },
	{ "SDL_UpperBlit", (uintptr_t)&SDL_UpperBlit },
	{ "SDL_WaitThread", (uintptr_t)&SDL_WaitThread },
	{ "SDL_GetKeyFromScancode", (uintptr_t)&SDL_GetKeyFromScancode },
	{ "SDL_GetNumVideoDisplays", (uintptr_t)&SDL_GetNumVideoDisplays },
	{ "SDL_GetDisplayBounds", (uintptr_t)&SDL_GetDisplayBounds },
	{ "SDL_UnionRect", (uintptr_t)&SDL_UnionRect },
	{ "SDL_GetKeyboardFocus", (uintptr_t)&SDL_GetKeyboardFocus },
	{ "SDL_GetRelativeMouseMode", (uintptr_t)&SDL_GetRelativeMouseMode },
	{ "SDL_NumJoysticks", (uintptr_t)&SDL_NumJoysticks },
	{ "SDL_GL_GetDrawableSize", (uintptr_t)&SDL_GL_GetDrawableSize },
	{ "SDL_GameControllerOpen", (uintptr_t)&SDL_GameControllerOpen },
	{ "SDL_GameControllerGetJoystick", (uintptr_t)&SDL_GameControllerGetJoystick },
	{ "SDL_HapticOpenFromJoystick", (uintptr_t)&SDL_HapticOpenFromJoystick },
	{ "SDL_GetPerformanceFrequency", (uintptr_t)&SDL_GetPerformanceFrequency },
	{ "SDL_GetPerformanceCounter", (uintptr_t)&SDL_GetPerformanceCounter },
	{ "SDL_GetMouseFocus", (uintptr_t)&SDL_GetMouseFocus },
	{ "SDL_ShowMessageBox", (uintptr_t)&SDL_ShowMessageBox },
	{ "SDL_RaiseWindow", (uintptr_t)&SDL_RaiseWindow },
	{ "SDL_GL_GetAttribute", (uintptr_t)&SDL_GL_GetAttribute },
	{ "SDL_GL_CreateContext", (uintptr_t)&SDL_GL_CreateContext },
	{ "SDL_GL_GetProcAddress", (uintptr_t)&SDL_GL_GetProcAddress_fake },
	{ "SDL_GL_DeleteContext", (uintptr_t)&SDL_GL_DeleteContext },
	{ "SDL_GetDesktopDisplayMode", (uintptr_t)&SDL_GetDesktopDisplayMode },
	{ "SDL_SetWindowData", (uintptr_t)&SDL_SetWindowData },
	{ "SDL_GetWindowFlags", (uintptr_t)&SDL_GetWindowFlags },
	{ "SDL_GetWindowSize", (uintptr_t)&SDL_GetWindowSize },
	{ "SDL_GetWindowDisplayIndex", (uintptr_t)&SDL_GetWindowDisplayIndex },
	{ "SDL_SetWindowFullscreen", (uintptr_t)&SDL_SetWindowFullscreen },
	{ "SDL_SetWindowSize", (uintptr_t)&SDL_SetWindowSize },
	{ "SDL_SetWindowPosition", (uintptr_t)&SDL_SetWindowPosition },
	{ "SDL_GL_GetCurrentWindow", (uintptr_t)&SDL_GL_GetCurrentWindow },
	{ "SDL_GetWindowData", (uintptr_t)&SDL_GetWindowData },
	{ "SDL_GetWindowTitle", (uintptr_t)&SDL_GetWindowTitle },
	{ "SDL_ResetKeyboard", (uintptr_t)&SDL_ResetKeyboard },
	{ "SDL_SetWindowTitle", (uintptr_t)&SDL_SetWindowTitle },
	{ "SDL_GetWindowPosition", (uintptr_t)&SDL_GetWindowPosition },
	{ "SDL_GL_SetSwapInterval", (uintptr_t)&ret0 },
	{ "SDL_IsGameController", (uintptr_t)&SDL_IsGameController },
	{ "SDL_JoystickGetDeviceGUID", (uintptr_t)&SDL_JoystickGetDeviceGUID },
	{ "SDL_GameControllerNameForIndex", (uintptr_t)&SDL_GameControllerNameForIndex },
	{ "SDL_GetWindowFromID", (uintptr_t)&SDL_GetWindowFromID },
	{ "SDL_GL_SwapWindow", (uintptr_t)&SDL_GL_SwapWindow },
	{ "SDL_SetMainReady", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_NumAccelerometers", (uintptr_t)&ret0 },
	{ "SDL_AndroidGetJNIEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "Android_JNI_GetEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "SDL_RWFromConstMem", (uintptr_t)&SDL_RWFromConstMem },
	{ "SDL_ConvertSurface", (uintptr_t)&SDL_ConvertSurface },
	{ "SDL_SetError", (uintptr_t)&SDL_SetError },
	{ "SDL_MapRGBA", (uintptr_t)&SDL_MapRGBA },
	{ "SDL_EventState", (uintptr_t)&SDL_EventState },
	{ "SDL_SetSurfaceBlendMode", (uintptr_t)&SDL_SetSurfaceBlendMode },
	{ "SDL_UpperBlitScaled", (uintptr_t)&SDL_UpperBlitScaled },
	{ "SDL_FreeRW", (uintptr_t)&SDL_FreeRW },
	{ "SDL_GetKeyboardState", (uintptr_t)&SDL_GetKeyboardState_hook },
	{ "SDL_JoystickNumAxes", (uintptr_t)&SDL_JoystickNumAxes },
	{ "SDL_JoystickUpdate", (uintptr_t)&SDL_JoystickUpdate },
	{ "SDL_JoystickGetAxis", (uintptr_t)&SDL_JoystickGetAxis },
	{ "SDL_JoystickGetButton", (uintptr_t)&SDL_JoystickGetButton },
	{ "SDL_GetScancodeFromKey", (uintptr_t)&SDL_GetScancodeFromKey },
	{ "SDL_GetKeyName", (uintptr_t)&SDL_GetKeyName },
	{ "SDL_GetScancodeName", (uintptr_t)&SDL_GetScancodeName },
	{ "SDL_JoystickGetHat", (uintptr_t)&SDL_JoystickGetHat },
	{ "SDL_JoystickClose", (uintptr_t)&SDL_JoystickClose },
	{ "SDL_JoystickOpen", (uintptr_t)&SDL_JoystickOpen },
	{ "SDL_JoystickEventState", (uintptr_t)&SDL_JoystickEventState },
	{ "Mix_PauseMusic", (uintptr_t)&Mix_PauseMusic },
	{ "Mix_ResumeMusic", (uintptr_t)&Mix_ResumeMusic },
	{ "Mix_VolumeMusic", (uintptr_t)&Mix_VolumeMusic },
	{ "Mix_LoadMUS", (uintptr_t)&Mix_LoadMUS_hook },
	{ "Mix_PlayMusic", (uintptr_t)&Mix_PlayMusic },
	{ "Mix_FreeMusic", (uintptr_t)&ret0 }, // FIXME
	{ "Mix_RewindMusic", (uintptr_t)&Mix_RewindMusic },
	{ "Mix_SetMusicPosition", (uintptr_t)&Mix_SetMusicPosition },
	{ "Mix_CloseAudio", (uintptr_t)&Mix_CloseAudio },
	{ "Mix_OpenAudio", (uintptr_t)&Mix_OpenAudio_hook },
	{ "Mix_RegisterEffect", (uintptr_t)&Mix_RegisterEffect },
	{ "Mix_Resume", (uintptr_t)&Mix_Resume },
	{ "Mix_AllocateChannels", (uintptr_t)&Mix_AllocateChannels },
	{ "Mix_ChannelFinished", (uintptr_t)&Mix_ChannelFinished },
	{ "Mix_LoadWAV_RW", (uintptr_t)&Mix_LoadWAV_RW },
	{ "Mix_FreeChunk", (uintptr_t)&Mix_FreeChunk },
	{ "Mix_PausedMusic", (uintptr_t)&Mix_PausedMusic },
	{ "Mix_Paused", (uintptr_t)&Mix_Paused },
	{ "Mix_PlayingMusic", (uintptr_t)&Mix_PlayingMusic },
	{ "Mix_Playing", (uintptr_t)&Mix_Playing },
	{ "Mix_Volume", (uintptr_t)&Mix_Volume },
	{ "Mix_SetDistance", (uintptr_t)&Mix_SetDistance },
	{ "Mix_SetPanning", (uintptr_t)&Mix_SetPanning },
	{ "Mix_QuerySpec", (uintptr_t)&Mix_QuerySpec },
	{ "Mix_UnregisterEffect", (uintptr_t)&Mix_UnregisterEffect },
	{ "Mix_HaltMusic", (uintptr_t)&Mix_HaltMusic },
	{ "Mix_HaltChannel", (uintptr_t)&Mix_HaltChannel },
	{ "Mix_LoadMUS_RW", (uintptr_t)&Mix_LoadMUS_RW },
	{ "Mix_PlayChannelTimed", (uintptr_t)&Mix_PlayChannelTimed },
	{ "Mix_Pause", (uintptr_t)&Mix_Pause },
	{ "Mix_Init", (uintptr_t)&Mix_Init },
	{ "TTF_Quit", (uintptr_t)&TTF_Quit },
	{ "TTF_Init", (uintptr_t)&TTF_Init },
	{ "TTF_RenderText_Blended", (uintptr_t)&TTF_RenderText_Blended },
	{ "TTF_OpenFontRW", (uintptr_t)&TTF_OpenFontRW },
	{ "TTF_SetFontOutline", (uintptr_t)&TTF_SetFontOutline },
	{ "TTF_CloseFont", (uintptr_t)&TTF_CloseFont },
	{ "TTF_GlyphIsProvided", (uintptr_t)&TTF_GlyphIsProvided },
	{ "IMG_Load", (uintptr_t)&IMG_Load_hook },
	{ "IMG_Load_RW", (uintptr_t)&IMG_Load_RW },
	{ "raise", (uintptr_t)&raise },
	{ "posix_memalign", (uintptr_t)&posix_memalign },
	{ "compress", (uintptr_t)&compress },
	{ "uncompress", (uintptr_t)&uncompress },
	{ "SDLNet_FreePacket", (uintptr_t)&SDLNet_FreePacket },
	{ "SDLNet_Quit", (uintptr_t)&SDLNet_Quit },
	{ "SDLNet_GetError", (uintptr_t)&SDLNet_GetError },
	{ "SDLNet_Init", (uintptr_t)&SDLNet_Init },
	{ "SDLNet_AllocPacket", (uintptr_t)&SDLNet_AllocPacket },
	{ "SDLNet_UDP_Recv", (uintptr_t)&SDLNet_UDP_Recv },
	{ "SDLNet_UDP_Send", (uintptr_t)&SDLNet_UDP_Send },
	{ "SDLNet_GetLocalAddresses", (uintptr_t)&SDLNet_GetLocalAddresses },
	{ "SDLNet_UDP_Close", (uintptr_t)&SDLNet_UDP_Close },
	{ "SDLNet_ResolveHost", (uintptr_t)&SDLNet_ResolveHost },
	{ "SDLNet_UDP_Open", (uintptr_t)&SDLNet_UDP_Open },
};
static size_t numhooks = sizeof(default_dynlib) / sizeof(*default_dynlib);

int check_kubridge(void) {
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs {
	UNKNOWN = 0,
	INIT,
	GET_SCREEN_WIDTH_DPI,
} MethodIDs;

typedef struct {
	char *name;
	enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{ "<init>", INIT },
};

int GetMethodID(void *env, void *class, const char *name, const char *sig) {
	printf("GetMethodID: %s\n", name);

	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0) {
			return name_to_method_ids[i].id;
		}
	}

	return UNKNOWN;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig) {
	printf("GetStaticMethodID: %s\n", name);
	
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	return UNKNOWN;
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	printf("bruh %d\n", methodID);
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

int64_t CallStaticLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return -1;
}

void *FindClass(void) {
	return (void *)0x41414141;
}

void *NewGlobalRef(void *env, char *str) {
	return (void *)0x42424242;
}

void DeleteGlobalRef(void *env, char *str) {
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args) {
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj) {
	return (void *)0x44444444;
}

char *NewStringUTF(void *env, char *bytes) {
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy) {
	return string;
}

size_t GetStringUTFLength(void *env, char *string) {
	return strlen(string);	
}

int GetJavaVM(void *env, void **vm) {
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

int GetBooleanField(void *env, void *obj, int fieldID) {
	return 1;
}

void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return NULL;
	}
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		break;
	}
}

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID) {
	switch (fieldID) {
	default:
		return NULL;
	}
}

void GetStringUTFRegion(void *env, char *str, size_t start, size_t len, char *buf) {
	sceClibMemcpy(buf, &str[start], len);
	buf[len] = 0;
}

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return NULL;
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

float GetFloatField(void *env, void *obj, int fieldID) {
	switch (fieldID) {
	default:
		return 0.0f;
	}
}

float CallStaticFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN) {
			dlog("CallStaticDoubleMethodV(%d)\n", methodID);
		}
		return 0;
	}
}

int GetArrayLength(void *env, void *array) {
	return 1;
}

/*int crasher(unsigned int argc, void *argv) {
	uint32_t *nullptr = NULL;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_SELECT) *nullptr = 0;
		sceKernelDelayThread(100);
	}
}*/

int main(int argc, char *argv[]) {
	//sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
	//SceUID crasher_thread = sceKernelCreateThread("crasher", crasher, 0x40, 0x1000, 0, 0, NULL);
	//sceKernelStartThread(crasher_thread, 0, NULL);	
	
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	if (check_kubridge() < 0)
		fatal_error("Error kubridge.skprx is not installed.");

	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
		fatal_error("Error libshacccg.suprx is not installed.");
	
	loadConfig();
	
	// Generating selected game path
	char fname[256];
	int8_t game_idx = 1;
	FILE *f = fopen("ux0:data/poppy.tmp", "rb");
	if (f) {
		fread(&game_idx, 1, 1, f);
		fclose(f);
		sceIoRemove("ux0:data/poppy.tmp");
	}
	sprintf(data_path, "ux0:data/poppykart/game%d/", game_idx);
	
	printf("Loading libmain\n");
	sprintf(fname, "%s/libmain.so", data_path);
	if (so_file_load(&poppykart_mod, fname, LOAD_ADDRESS) < 0)
		fatal_error("Error could not load %s.", fname);
	so_relocate(&poppykart_mod);
	so_resolve(&poppykart_mod, default_dynlib, sizeof(default_dynlib), 0);

	vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
	
	patch_game();
	so_flush_caches(&poppykart_mod);
	so_initialize(&poppykart_mod);
	
	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;

	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)ret0; // PushLocalFrame
	*(uintptr_t *)(fake_env + 0x50) = (uintptr_t)ret0; // PopLocalFrame
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x198) = (uintptr_t)GetFloatField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticLongMethodV;
	*(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;
	
	// Disabling rearpad
	SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);

	int (* Java_org_libsdl_app_SDLActivity_nativeInit)(int a1, int a2, int a3) = (void *) so_symbol(&poppykart_mod, "Java_org_libsdl_app_SDLActivity_nativeInit");
	
    Java_org_libsdl_app_SDLActivity_nativeInit(0, 0, 0);
	
	return 0;
}
