#ifndef ARK_H
#define ARK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef struct { uint64_t framebuffer; uint32_t width,height,pitch,bpp; uint8_t red_pos,red_size,green_pos,green_size,blue_pos,blue_size; uint64_t memory_mib; } BootInfo;
enum { EV_KEY=1, EV_MOUSE=2 };
enum { KEY_ENTER=256,KEY_BACKSPACE,KEY_ESCAPE,KEY_UP,KEY_DOWN,KEY_LEFT,KEY_RIGHT,KEY_TAB,KEY_F1,KEY_F2,KEY_F3,KEY_F4,KEY_DELETE };
typedef struct { int type,key,dx,dy; uint8_t buttons; } Event;
void platform_init(uint32_t magic,uint32_t mb_addr,BootInfo *info);
bool platform_next_event(Event *ev);
uint64_t platform_ticks(void);
void platform_idle(void);
void platform_reboot(void);
void platform_poweroff(void);
void serial_write(const char *s);
void platform_time(int *h,int *m,int *s);
void *memset(void *p,int v,size_t n);
void *memcpy(void *d,const void *s,size_t n);
void *memmove(void *d,const void *s,size_t n);
size_t strlen(const char *s);
int strcmp(const char *a,const char *b);
int strncmp(const char *a,const char *b,size_t n);
void strcopy(char *d,const char *s,size_t cap);
void uint_to_str(uint64_t n,char *out);
#define VFS_MAX_FILES 24
#define VFS_FILE_CAP 4096
typedef struct { bool used; char name[40]; char data[VFS_FILE_CAP]; size_t size; } VFile;
extern VFile vfs_files[VFS_MAX_FILES];
void vfs_init(void);
int vfs_find(const char *name);
int vfs_create(const char *name);
bool vfs_write(int index,const char *text);
bool vfs_remove(const char *name);
#define SHELL_LINES 128
#define SHELL_COLS 100
extern char shell_lines[SHELL_LINES][SHELL_COLS];
extern int shell_line_count;
void shell_init(const BootInfo *info);
void shell_execute(const char *command);
void shell_print(const char *text);
/* Shell actions handled by the desktop: 1 files,2 notes,3 settings,4 about. */
extern int shell_action;
#endif
