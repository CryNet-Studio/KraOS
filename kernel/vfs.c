#include "ark.h"

VFile vfs_files[VFS_MAX_FILES];

/* A deliberately small, case-sensitive RAM filesystem. */
static bool valid_name(const char *name) {
    if (!name || !name[0]) return false;
    size_t i = 0;
    while (i < sizeof(vfs_files[0].name) && name[i]) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
        ++i;
    }
    if (i >= sizeof(vfs_files[0].name)) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    return true;
}

int vfs_find(const char *name) {
    if (!valid_name(name)) return -1;
    for (int i = 0; i < VFS_MAX_FILES; ++i)
        if (vfs_files[i].used && !strcmp(vfs_files[i].name, name)) return i;
    return -1;
}

int vfs_create(const char *name) {
    if (!valid_name(name)) return -1;
    int existing = vfs_find(name);
    if (existing >= 0) return existing;
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (vfs_files[i].used) continue;
        memset(&vfs_files[i], 0, sizeof(vfs_files[i]));
        strcopy(vfs_files[i].name, name, sizeof(vfs_files[i].name));
        vfs_files[i].used = true;
        return i;
    }
    return -1;
}

bool vfs_write(int index, const char *text) {
    if (index < 0 || index >= VFS_MAX_FILES || !vfs_files[index].used || !text)
        return false;
    size_t length = 0;
    while (length < VFS_FILE_CAP && text[length]) ++length;
    if (length >= VFS_FILE_CAP) return false;
    /* memmove permits saving from a view into this file's existing content. */
    memmove(vfs_files[index].data, text, length);
    vfs_files[index].data[length] = '\0';
    vfs_files[index].size = length;
    return true;
}

bool vfs_remove(const char *name) {
    int index = vfs_find(name);
    if (index < 0) return false;
    memset(&vfs_files[index], 0, sizeof(vfs_files[index]));
    return true;
}

void vfs_init(void) {
    memset(vfs_files, 0, sizeof(vfs_files));
    int file = vfs_create("README.txt");
    vfs_write(file,
        "Welcome to ArkOS.\n"
        "This is a small, bootable x86_64 operating-system prototype.\n"
        "Use the desktop apps or type help in Terminal.\n\n"
        "Files are stored in RAM only. Rebooting discards every edit.\n"
        "There is no disk driver, networking, user-process loader,\n"
        "or security isolation. Desktop apps share one kernel.\n\n"
        "RAM file limits: 24 files, 4095 bytes each.\n"
        "Names are case-sensitive and use letters, digits, dot,\n"
        "underscore or hyphen, up to 39 characters.\n");
    file = vfs_create("hello.txt");
    vfs_write(file, "Hello from ArkOS!\nYour kernel is running on bare virtual hardware.\n");
    file = vfs_create("notes.txt");
    vfs_write(file,
        "My ArkOS notes\n\n"
        "Try opening Terminal and entering:\n"
        "  help\n"
        "  ls\n"
        "  cat README.txt\n"
        "  write hello.txt Hello, ArkOS!\n\n"
        "Remember: these files live in RAM and reset on reboot.\n");
}
