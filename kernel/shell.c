#include "ark.h"

char shell_lines[SHELL_LINES][SHELL_COLS];
int shell_line_count;
int shell_action;
static uint64_t boot_memory_mib;

#define COMMAND_CAP 512

static char *new_line(void) {
    if (shell_line_count >= SHELL_LINES) {
        memmove(shell_lines[0], shell_lines[1],
                (SHELL_LINES - 1) * sizeof(shell_lines[0]));
        shell_line_count = SHELL_LINES - 1;
    }
    char *line = shell_lines[shell_line_count++];
    memset(line, 0, SHELL_COLS);
    return line;
}

/* Each call is a complete output record; long records wrap into display rows. */
void shell_print(const char *text) {
    if (!text) return;
    serial_write(text);
    size_t len = strlen(text);
    if (!len || text[len - 1] != '\n') serial_write("\n");
    char *line = new_line();
    size_t column = 0;
    for (size_t i = 0; text[i]; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (text[i + 1]) line = new_line();
            column = 0;
            continue;
        }
        unsigned spaces = c == '\t' ? 4u - (unsigned)(column % 4) : 1u;
        if (c == '\t') c = ' ';
        else if (c < 32 || c > 126) c = '?';
        while (spaces--) {
            if (column >= SHELL_COLS - 1) {
                line = new_line();
                column = 0;
            }
            line[column++] = (char)c;
            line[column] = '\0';
        }
    }
}

static bool white(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char *skip_white(char *s) {
    while (white(*s)) ++s;
    return s;
}

static char *token(char **cursor) {
    char *start = skip_white(*cursor);
    if (!*start) {
        *cursor = start;
        return 0;
    }
    char *end = start;
    while (*end && !white(*end)) ++end;
    if (*end) *end++ = '\0';
    *cursor = end;
    return start;
}

static bool empty(char *s) { return !*skip_white(s); }

static void append(char *out, size_t cap, const char *text) {
    size_t len = strlen(out);
    if (len < cap) strcopy(out + len, text, cap - len);
}

static void append_number(char *out, size_t cap, uint64_t value) {
    char number[24];
    uint_to_str(value, number);
    append(out, cap, number);
}

static void clear_output(void) {
    memset(shell_lines, 0, sizeof(shell_lines));
    shell_line_count = 0;
    serial_write("\033[2J\033[H");
}

static void print_help(void) {
    shell_print("ArkOS Terminal - built-in commands");
    shell_print("help                    Show this list");
    shell_print("clear                   Clear terminal output");
    shell_print("about / uname           System information");
    shell_print("mem / uptime / date     Boot memory, uptime, UTC clock");
    shell_print("ls                      List RAM files");
    shell_print("cat <name>              Read a RAM file");
    shell_print("touch <name>            Create an empty file");
    shell_print("write <name> <text>     Replace file contents (creates if missing)");
    shell_print("rm <name>               Remove a RAM file");
    shell_print("echo <text>             Print text");
    shell_print("open files|notes|settings|about");
    shell_print("reboot / shutdown       Restart or power off the machine");
    shell_print("");
    shell_print("RAM only: all files and edits reset on reboot. Names are case-sensitive.");
    shell_print("No external programs, disk persistence, network, pipes or redirection.");
}

static void print_about(void) {
    shell_print("ArkOS 0.1.0 - an experimental x86_64 kernel");
    shell_print("Framebuffer desktop, keyboard/mouse input, and a built-in shell.");
    shell_print("The desktop apps share this kernel; they are not isolated processes.");
    shell_print("Files live in RAM only. There are no disk or network drivers.");
}

static void print_files(void) {
    int count = 0;
    shell_print("NAME                                    BYTES");
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (!vfs_files[i].used) continue;
        char line[80] = {0};
        strcopy(line, vfs_files[i].name, sizeof(line));
        size_t j = strlen(line);
        while (j < 40) line[j++] = ' ';
        line[j] = '\0';
        append_number(line, sizeof(line), vfs_files[i].size);
        shell_print(line);
        ++count;
    }
    if (!count) shell_print("[no files]");
    shell_print("RAM filesystem: contents reset on reboot.");
}

static void print_mem(void) {
    char line[100] = "Memory reported at boot: ";
    append_number(line, sizeof(line), boot_memory_mib);
    append(line, sizeof(line), " MiB");
    shell_print(line);
    shell_print("RAM filesystem: 24 slots, up to 4095 content bytes per file.");
    shell_print("This is boot memory information, not a live free-memory counter.");
}

static void print_uptime(void) {
    uint64_t seconds = platform_ticks() / 100;
    char line[100] = "Uptime: ";
    append_number(line, sizeof(line), seconds / 3600);
    append(line, sizeof(line), "h ");
    append_number(line, sizeof(line), (seconds / 60) % 60);
    append(line, sizeof(line), "m ");
    append_number(line, sizeof(line), seconds % 60);
    append(line, sizeof(line), "s");
    shell_print(line);
}

static void print_time(void) {
    int hour = 0, minute = 0, second = 0;
    platform_time(&hour, &minute, &second);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        shell_print("RTC clock unavailable.");
        return;
    }
    char line[] = "UTC clock: 00:00:00";
    line[11] = (char)('0' + hour / 10);
    line[12] = (char)('0' + hour % 10);
    line[14] = (char)('0' + minute / 10);
    line[15] = (char)('0' + minute % 10);
    line[17] = (char)('0' + second / 10);
    line[18] = (char)('0' + second % 10);
    shell_print(line);
    shell_print("Calendar date is not exposed by this kernel.");
}

void shell_init(const BootInfo *info) {
    boot_memory_mib = info ? info->memory_mib : 0;
    shell_action = 0;
    shell_line_count = 0;
    memset(shell_lines, 0, sizeof(shell_lines));
    shell_print("ArkOS Terminal 0.1.0");
    shell_print("Type help for commands. Up/Down scroll output.");
    shell_print("Files live in RAM; restart clears your changes.");
}

void shell_execute(const char *command) {
    if (!command) return;
    char buffer[COMMAND_CAP];
    size_t len = 0;
    while (len < COMMAND_CAP && command[len]) ++len;
    if (len >= COMMAND_CAP) {
        shell_print("Command too long (maximum 511 bytes).");
        return;
    }
    memcpy(buffer, command, len + 1);
    char *cursor = skip_white(buffer);
    if (!*cursor) return;
    char prompt[COMMAND_CAP + 6] = "ark> ";
    append(prompt, sizeof(prompt), cursor);
    shell_print(prompt);
    char *cmd = token(&cursor);
    if (!strcmp(cmd, "echo")) {
        shell_print(skip_white(cursor));
        return;
    }
    if (!strcmp(cmd, "write")) {
        char *name = token(&cursor);
        if (!name) { shell_print("Usage: write <name> <text>"); return; }
        char *text = skip_white(cursor);
        int index = vfs_find(name);
        if (index < 0) index = vfs_create(name);
        if (index < 0) {
            shell_print("Cannot create file. Use 1-39 letters, digits, ., _ or -; maximum 24 files.");
        } else if (!vfs_write(index, text)) {
            shell_print("Cannot write file. Maximum content size is 4095 bytes.");
        } else {
            shell_print("Saved in RAM. Changes reset on reboot.");
        }
        return;
    }
    if (!strcmp(cmd, "cat") || !strcmp(cmd, "touch") || !strcmp(cmd, "rm") || !strcmp(cmd, "open")) {
        char *name = token(&cursor);
        if (!name || !empty(cursor)) {
            shell_print("This command requires exactly one name. Type help for syntax.");
            return;
        }
        if (!strcmp(cmd, "cat")) {
            int index = vfs_find(name);
            if (index < 0) shell_print("File not found (names are case-sensitive).");
            else shell_print(vfs_files[index].size ? vfs_files[index].data : "[empty file]");
        } else if (!strcmp(cmd, "touch")) {
            if (vfs_create(name) < 0)
                shell_print("Cannot create file. Use 1-39 letters, digits, ., _ or -; maximum 24 files.");
            else shell_print("File ready in RAM.");
        } else if (!strcmp(cmd, "rm")) {
            shell_print(vfs_remove(name) ? "File removed from RAM." : "File not found.");
        } else {
            if (!strcmp(name, "files")) shell_action = 1;
            else if (!strcmp(name, "notes")) shell_action = 2;
            else if (!strcmp(name, "settings")) shell_action = 3;
            else if (!strcmp(name, "about")) shell_action = 4;
            else shell_print("Usage: open files|notes|settings|about");
        }
        return;
    }
    if (!empty(cursor)) {
        shell_print("Unknown command or unexpected arguments. Type help for syntax.");
        return;
    }
    if (!strcmp(cmd, "help")) print_help();
    else if (!strcmp(cmd, "clear")) clear_output();
    else if (!strcmp(cmd, "about")) print_about();
    else if (!strcmp(cmd, "uname")) shell_print("ArkOS 0.1.0 x86_64");
    else if (!strcmp(cmd, "mem")) print_mem();
    else if (!strcmp(cmd, "uptime")) print_uptime();
    else if (!strcmp(cmd, "date")) print_time();
    else if (!strcmp(cmd, "ls")) print_files();
    else if (!strcmp(cmd, "reboot")) {
        shell_print("Rebooting. RAM files will be discarded.");
        platform_reboot();
    } else if (!strcmp(cmd, "shutdown")) {
        shell_print("Requesting poweroff. RAM files will be discarded.");
        platform_poweroff();
        shell_print("Poweroff unsupported on this machine; use your VM or hardware power control.");
    } else shell_print("Unknown command. Type help for syntax.");
}
