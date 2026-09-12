// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include <stdarg.h>
#include <syscall.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>

#define TIOCGWINSZ 0x5413
#define TIOCSPGRP  0x5410

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define MIN_INSTALL_SECTORS 2097152 

static int sc_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int sc_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return i;
}

#define BG_BLUE      "\x1b[44m"
#define FG_WHITE     "\x1b[97m"
#define FG_BLACK     "\x1b[30m"
#define FG_YELLOW    "\x1b[93m"
#define FG_RED       "\x1b[31m"
#define BG_WHITE     "\x1b[47m"
#define BG_BLACK     "\x1b[40m"
#define BG_RED       "\x1b[41m"
#define RESET        "\x1b[0m"
#define KEY_UP     1000
#define KEY_DOWN   1001
#define KEY_LEFT   1002
#define KEY_RIGHT  1003
#define KEY_ENTER  1004
#define KEY_SPACE  1005
#define KEY_ESC    1006
#define KEY_RESIZE 1007

static volatile sig_atomic_t g_installer_winch = 0;
static void handle_installer_sigwinch(int sig) {
    (void)sig;
    g_installer_winch = 1;
}

static int term_cols = 80;
static int term_rows = 25;

static void update_term_size(void) {
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        term_cols = ws.ws_col;
        term_rows = ws.ws_row;
    }
}

static int get_key(void) {
    char ch;
    while (1) {
        if (g_installer_winch) {
            g_installer_winch = 0;
            update_term_size();
            return KEY_RESIZE;
        }

        struct pollfd pfd = { .fd = 0, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, -1);

        if (g_installer_winch) {
            g_installer_winch = 0;
            update_term_size();
            return KEY_RESIZE;
        }
        
        if (read(0, &ch, 1) <= 0) continue;
        
        if (ch == '\x1b') {
            char seq[2];
            usleep(10000); 
            int r = read(0, &seq[0], 1);
            if (r > 0 && seq[0] == '[') {
                r = read(0, &seq[1], 1);
                if (r > 0) {
                    switch (seq[1]) {
                        case 'A': return KEY_UP;
                        case 'B': return KEY_DOWN;
                        case 'C': return KEY_RIGHT;
                        case 'D': return KEY_LEFT;
                    }
                }
            }
            return KEY_ESC;
        }
        if (ch == '\r' || ch == '\n') return KEY_ENTER;
        if (ch == ' ') return KEY_SPACE;
        if (ch == 127 || ch == '\b') return '\b';
        return (unsigned char)ch;
    }
}

static void clear_screen(void) {
    if (write(STDOUT_FILENO, "\x1b[2J", 4) != 4 ||
        write(STDOUT_FILENO, "\x1b[H", 3) != 3) {
        perror("write");
    }
}

static void clear_screen_blue(void) {
    sys_write(1, "\x1b[44m\x1b[97m", 10);
    clear_screen();
    for (int r = 0; r < term_rows; r++) {
        char pos[32];
        int len = snprintf(pos, sizeof(pos), "\x1b[%d;1H", r + 1);
        sys_write(1, pos, len);
        sys_write(1, "\x1b[44m", 5);
        for (int c = 0; c < term_cols; c++) {
            sys_write(1, " ", 1);
        }
    }
}

static void write_str(int x, int y, const char *str, const char *style) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH%s", y, x, style ? style : "");
    sys_write(1, buf, len);
    sys_write(1, str, strlen(str));
}

static void draw_box(int x, int y, int w, int h, const char *title) {
    for (int r = 1; r <= h; r++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH\x1b[40m  ", y + r, x + w);
        sys_write(1, buf, len);
    }
    char bbuf[64];
    int blen = snprintf(bbuf, sizeof(bbuf), "\x1b[%d;%dH\x1b[40m", y + h, x + 2);
    sys_write(1, bbuf, blen);
    for (int col = 0; col < w; col++) {
        sys_write(1, " ", 1);
    }

    for (int r = 0; r < h; r++) {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH\x1b[47m\x1b[30m", y + r, x);
        sys_write(1, buf, len);

        if (r == 0) {
            sys_write(1, "┌", 3);
            int title_len = title ? strlen(title) : 0;
            int dash_before = (w - 2 - title_len) / 2;
            int dash_after = w - 2 - title_len - dash_before;
            if (title_len > 0) {
                dash_before = (w - 4 - title_len) / 2;
                dash_after = w - 4 - title_len - dash_before;
            }
            for (int i = 0; i < dash_before; i++) sys_write(1, "─", 3);
            if (title_len > 0) {
                sys_write(1, "[", 1);
                sys_write(1, "\x1b[31m", 5); 
                sys_write(1, title, title_len);
                sys_write(1, "\x1b[30m", 5);
                sys_write(1, "]", 1);
            }
            for (int i = 0; i < dash_after; i++) sys_write(1, "─", 3);
            sys_write(1, "┐", 3);
        } else if (r == h - 1) {
            sys_write(1, "└", 3);
            for (int i = 0; i < w - 2; i++) sys_write(1, "─", 3);
            sys_write(1, "┘", 3);
        } else {
            sys_write(1, "│", 3);
            for (int i = 0; i < w - 2; i++) sys_write(1, " ", 1);
            sys_write(1, "│", 3);
        }
    }
}

typedef struct {
    char devname[16];
    uint32_t sectors;
    uint32_t mb;
} TargetDisk;

static int get_available_disks(TargetDisk *disks, int max_disks) {
    int count = 0;
    DIR *dir = opendir("/dev");
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strncmp(name, "sd", 2) != 0 && strncmp(name, "hd", 2) != 0 && strncmp(name, "vd", 2) != 0)
            continue;

        size_t len = strlen(name);
        if (len == 0 || (name[len - 1] >= '0' && name[len - 1] <= '9'))
            continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        off_t size_bytes = lseek(fd, 0, SEEK_END);
        if (size_bytes <= 0) {
            uint64_t bsize = 0;
            if (ioctl(fd, 0x80081272 /* BLKGETSIZE64 */, &bsize) == 0 && bsize > 0) {
                size_bytes = (off_t)bsize;
            }
        }
        close(fd);

        if (size_bytes > 0) {
            sc_strncpy(disks[count].devname, name, sizeof(disks[count].devname));
            disks[count].sectors = (uint32_t)(size_bytes / 512);
            disks[count].mb = (uint32_t)(size_bytes / (1024 * 1024));
            count++;
            if (count >= max_disks) break;
        }
    }
    closedir(dir);
    return count;
}

typedef struct {
    char filename[64];
    char pkgname[64];
    int enabled;
} PackageOption;

static int get_package_options(PackageOption *options, int max_options) {
    int count = 0;
    FAT32_FileInfo *entries = (FAT32_FileInfo *)malloc(sizeof(FAT32_FileInfo) * 32);
    if (!entries) return 0;
    int offset = 0;
    while (count < max_options) {
        int n = sys_list_offset("/usr/share/packages", entries, 32, offset);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (entries[i].is_directory) continue;
            size_t len = strlen(entries[i].name);
            if (len > 4 && strcmp(entries[i].name + len - 4, ".bup") == 0) {
                sc_strncpy(options[count].filename, entries[i].name, sizeof(options[count].filename));
                sc_strncpy(options[count].pkgname, entries[i].name, sizeof(options[count].pkgname));
                options[count].pkgname[len - 4] = '\0';
                options[count].enabled = 1;
                count++;
                if (count >= max_options) break;
            }
        }
        offset += n;
    }
    free(entries);
    return count;
}

static char excludes[2048][128];
static int num_excludes = 0;

static void load_excludes(void) {
    int fd = sys_open("/usr/share/packages/excludes.txt", "r");
    if (fd < 0) return;
    
    char *buf = (char*)malloc(65536);
    if (!buf) { sys_close(fd); return; }
    
    int n = sys_read(fd, buf, 65536 - 1);
    if (n > 0) {
        buf[n] = '\0';
        char *line = buf;
        while (line && *line && num_excludes < 2048) {
            char *next = strchr(line, '\n');
            if (next) *next = '\0';
            
            int len = strlen(line);
            if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
            
            if (line[0] != '\0') {
                sc_strncpy(excludes[num_excludes], line, 128);
                num_excludes++;
            }
            
            if (next) line = next + 1;
            else line = NULL;
        }
    }
    free(buf);
    sys_close(fd);
}

static int should_exclude(const char *path) {
    if (sc_strcmp(path, "/bin/boredos_install") == 0 ||
        sc_strcmp(path, "/bin/boredos_install.elf") == 0 ||
        sc_strcmp(path, "/bin/installer") == 0 ||
        sc_strcmp(path, "/bin/installer.elf") == 0 ||
        sc_strcmp(path, "/usr/share/applications/installer.desktop") == 0 ||
        strncmp(path, "/usr/share/packages", 19) == 0) {
        return 1;
    }
    for (int i = 0; i < num_excludes; i++) {
        if (sc_strcmp(path, excludes[i]) == 0) {
            return 1;
        }
        int len = strlen(excludes[i]);
        if (strncmp(path, excludes[i], len) == 0 && (path[len] == '/' || path[len] == '\0')) {
            return 1;
        }
    }
    return 0;
}

static int g_current_percent = 0;
static const char *g_current_stage = "";
static char g_copy_err[128] = "";

static void show_progress(const char *stage_name, int percent);

static int run_command_silent(const char *bin, const char *args, const char *logfile) {
    int saved_out = dup(1);
    int saved_err = dup(2);

    int log_fd = open(logfile ? logfile : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd >= 0) {
        dup2(log_fd, 1);
        dup2(log_fd, 2);
        close(log_fd);
    }

    int pid = sys_spawn(bin, args, 0, 0);
    int status = -1;
    if (pid >= 0) {
        waitpid(pid, &status, 0);
    }

    if (saved_out >= 0) {
        dup2(saved_out, 1);
        close(saved_out);
    }
    if (saved_err >= 0) {
        dup2(saved_err, 2);
        close(saved_err);
    }

    return status;
}

static int copy_file(const char *src, const char *dst) {
    show_progress(g_current_stage, g_current_percent);
    int sfd = sys_open(src, "r");
    if (sfd < 0) {
        snprintf(g_copy_err, sizeof(g_copy_err), "cannot open src: %s (err %d)", src, sfd);
        return -1;
    }
    sys_delete(dst);
    int dfd = sys_open(dst, "w");
    if (dfd < 0) {
        snprintf(g_copy_err, sizeof(g_copy_err), "cannot open dst: %s (err %d)", dst, dfd);
        sys_close(sfd);
        return -1;
    }

    char *buf = (char*)malloc(65536);
    if (!buf) {
        snprintf(g_copy_err, sizeof(g_copy_err), "malloc failed");
        sys_close(sfd); sys_close(dfd);
        return -1;
    }
    int n;
    while ((n = sys_read(sfd, buf, 65536)) > 0) {
        int written = 0;
        while (written < n) {
            int w = sys_write_fs(dfd, buf + written, n - written);
            if (w <= 0) {
                snprintf(g_copy_err, sizeof(g_copy_err), "write failed to %s: w=%d n=%d", dst, w, n - written);
                sys_close(sfd); sys_close(dfd);
                free(buf);
                return -1;
            }
            written += w;
        }
    }
    free(buf);
    sys_close(sfd);
    sys_close(dfd);
    return 0;
}

static int copy_file_optional(const char *src, const char *dst) {
    if (!sys_exists(src)) return 0;
    return copy_file(src, dst);
}

static int copy_tree(const char *src_dir, const char *dst_dir) {
    if (should_exclude(src_dir)) return 0;
    sys_mkdir(dst_dir);
    
    int chunk_size = 128;
    FAT32_FileInfo *entries = (FAT32_FileInfo *)malloc(sizeof(FAT32_FileInfo) * chunk_size);
    if (!entries) {
        snprintf(g_copy_err, sizeof(g_copy_err), "malloc entries failed");
        return -1;
    }
    
    int offset = 0;
    while (1) {
        int n = sys_list_offset(src_dir, entries, chunk_size, offset);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (entries[i].name[0] == '.' && entries[i].name[1] == '_') continue;
            if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) continue;

            char src_path[512], dst_path[512];
            snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entries[i].name);
            snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entries[i].name);

            if (should_exclude(src_path)) continue;

            if (entries[i].is_directory) {
                if (copy_tree(src_path, dst_path) != 0) {
                    free(entries);
                    return -1;
                }
            } else {
                if (copy_file(src_path, dst_path) != 0) {
                    free(entries);
                    return -1;
                }
            }
        }
        offset += n;
    }
    free(entries);
    return 0;
}

static int s_last_percent = -1;
static const char *s_last_stage = NULL;

static void show_progress(const char *stage_name, int percent) {
    g_current_stage = stage_name;
    g_current_percent = percent;
    if (s_last_percent == percent && s_last_stage == stage_name) return;
    s_last_percent = percent;
    s_last_stage = stage_name;

    update_term_size();
    int w = 60;
    int h = 8;
    int x = (term_cols - w) / 2;
    int y = (term_rows - h) / 2;
    
    draw_box(x, y, w, h, "Installing BoredOS...");
    
    char stage_buf[64];
    int max_stage_len = w - 8;
    if (max_stage_len > 50) max_stage_len = 50;
    if (max_stage_len < 10) max_stage_len = 10;
    snprintf(stage_buf, sizeof(stage_buf), "%-*.*s", max_stage_len, max_stage_len, stage_name);
    write_str(x + 4, y + 2, stage_buf, BG_WHITE FG_BLACK);
    
    char bar_buf[256];
    int bar_w = 48;
    int filled = (percent * bar_w) / 100;
    
    int bi = 0;
    bi += snprintf(bar_buf + bi, sizeof(bar_buf) - bi, BG_WHITE FG_BLACK "[");
    
    for (int i = 0; i < bar_w; i++) {
        if (i < filled) {
            bar_buf[bi++] = '\xe2';
            bar_buf[bi++] = '\x96';
            bar_buf[bi++] = '\x88';
        } else {
            bar_buf[bi++] = ' ';
        }
    }
    bar_buf[bi++] = ']';
    bar_buf[bi] = '\0';
    
    write_str(x + 5, y + 4, bar_buf, NULL);
    
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    write_str(x + (w - strlen(pct)) / 2, y + 5, pct, BG_WHITE FG_BLACK);
}

static void show_welcome(void) {
    update_term_size();
    clear_screen_blue();
    
    int w = 60;
    int h = 10;
    int x = (term_cols - w) / 2;
    int y = (term_rows - h) / 2;
    
    draw_box(x, y, w, h, "BoredOS Installer");
    
    write_str(x + 4, y + 2, "Welcome to the BoredOS Installation Utility!", BG_WHITE FG_BLACK);
    write_str(x + 4, y + 4, "This wizard will install BoredOS on your system.", BG_WHITE FG_BLACK);
    write_str(x + 4, y + 5, "Please make sure you have backed up any data.", BG_WHITE FG_BLACK);
    
    write_str(x + (w - 8) / 2, y + 7, "<  OK  >", BG_RED FG_WHITE);
    
    while (1) {
        int k = get_key();
        if (k == KEY_ENTER || k == KEY_SPACE) {
            break;
        }
    }
}

static int show_disk_selection(TargetDisk *disks, int num_disks) {
    int selected = 0;
    int w = 60;
    int h = 8 + num_disks;
    if (h < 11) h = 11;
    
    update_term_size();
    clear_screen_blue();
    int last_cols = term_cols;
    int last_rows = term_rows;
    
    while (1) {
        update_term_size();
        if (term_cols != last_cols || term_rows != last_rows) {
            clear_screen_blue();
            last_cols = term_cols;
            last_rows = term_rows;
        }
        
        int x = (term_cols - w) / 2;
        int y = (term_rows - h) / 2;
        
        draw_box(x, y, w, h, "Select Target Disk");
        write_str(x + 4, y + 2, "Choose the disk to install BoredOS to:", BG_WHITE FG_BLACK);
        
        for (int i = 0; i < num_disks; i++) {
            char line[128];
            snprintf(line, sizeof(line), "  (%c) /dev/%-6s  -  %u MB", 
                     (selected == i) ? '*' : ' ', 
                     disks[i].devname, disks[i].mb);
            
            if (selected == i) {
                write_str(x + 4, y + 4 + i, line, BG_BLACK FG_WHITE);
            } else {
                write_str(x + 4, y + 4 + i, line, BG_WHITE FG_BLACK);
            }
        }
        
        write_str(x + (w - 14) / 2, y + h - 2, "< Continue >", BG_RED FG_WHITE);
        
        int k = get_key();
        if (k == KEY_UP) {
            selected = (selected - 1 + num_disks) % num_disks;
        } else if (k == KEY_DOWN) {
            selected = (selected + 1) % num_disks;
        } else if (k == KEY_ENTER) {
            return selected;
        }
    }
}

static void show_package_selection(PackageOption *options, int num_options) {
    int cursor = 0;
    int w = 64;
    int h = 15;
    int list_h = 6;
    
    update_term_size();
    clear_screen_blue();
    int last_cols = term_cols;
    int last_rows = term_rows;
    
    while (1) {
        update_term_size();
        if (term_cols != last_cols || term_rows != last_rows) {
            clear_screen_blue();
            last_cols = term_cols;
            last_rows = term_rows;
        }
        
        int x = (term_cols - w) / 2;
        int y = (term_rows - h) / 2;
        
        draw_box(x, y, w, h, "Select Optional Packages");
        write_str(x + 4, y + 2, "Select packages to install (Space to toggle, Enter to OK):", BG_WHITE FG_BLACK);
        
        int start_idx = 0;
        if (cursor >= list_h) {
            start_idx = cursor - list_h + 1;
        }
        
        for (int i = 0; i < list_h; i++) {
            int idx = start_idx + i;
            if (idx >= num_options) break;
            
            char line[128];
            snprintf(line, sizeof(line), "  [%c] %-20s", 
                     options[idx].enabled ? '*' : ' ', 
                     options[idx].pkgname);
            
            if (cursor == idx) {
                write_str(x + 4, y + 4 + i, line, BG_BLACK FG_WHITE);
            } else {
                write_str(x + 4, y + 4 + i, line, BG_WHITE FG_BLACK);
            }
        }
        
        write_str(x + 4, y + 4 + list_h + 1, "Use Up/Down to scroll, Space to toggle, Enter to confirm.", BG_WHITE FG_BLACK);
        write_str(x + (w - 14) / 2, y + h - 2, "< Continue >", BG_RED FG_WHITE);
        
        int k = get_key();
        if (k == KEY_UP) {
            cursor = (cursor - 1 + num_options) % num_options;
        } else if (k == KEY_DOWN) {
            cursor = (cursor + 1) % num_options;
        } else if (k == KEY_SPACE) {
            options[cursor].enabled = !options[cursor].enabled;
        } else if (k == KEY_ENTER) {
            return;
        }
    }
}

static int show_fs_selection(void) {
    int w = 55;
    int h = 10;
    int selected = 0;
    
    update_term_size();
    clear_screen_blue();
    int last_cols = term_cols;
    int last_rows = term_rows;
    
    while (1) {
        update_term_size();
        if (term_cols != last_cols || term_rows != last_rows) {
            clear_screen_blue();
            last_cols = term_cols;
            last_rows = term_rows;
        }
        
        int x = (term_cols - w) / 2;
        int y = (term_rows - h) / 2;
        
        draw_box(x, y, w, h, "Root Filesystem");
        write_str(x + 4, y + 2, "Choose filesystem for root partition:", BG_WHITE FG_BLACK);
        
        if (selected == 0) {
            write_str(x + 6, y + 4, "[*] EXT4  (Recommended!)", BG_BLACK FG_WHITE);
            write_str(x + 6, y + 5, "[ ] FAT32", BG_WHITE FG_BLACK);
        } else {
            write_str(x + 6, y + 4, "[ ] EXT4 (Recommended!)", BG_WHITE FG_BLACK);
            write_str(x + 6, y + 5, "[*] FAT32", BG_BLACK FG_WHITE);
        }
        
        write_str(x + (w - 14) / 2, y + h - 2, "< Continue >", BG_RED FG_WHITE);
        
        int k = get_key();
        if (k == KEY_UP || k == KEY_DOWN) {
            selected = !selected;
        } else if (k == KEY_ENTER || k == ' ') {
            return selected;
        }
    }
}

typedef struct {
    int is_dhcp;
    char ip[32];
    char netmask[32];
    char gateway[32];
    char nameserver[32];
} NetworkConfig;

typedef struct {
    const char *continent;
    const char *cities[16];
    int num_cities;
} TzContinent;

static const TzContinent g_tz_continents[] = {
    { "Africa",    { "Cairo", "Johannesburg" }, 2 },
    { "America",   { "Buenos_Aires", "Chicago", "Denver", "Los_Angeles", "New_York", "Sao_Paulo", "Toronto", "Vancouver" }, 8 },
    { "Asia",      { "Dubai", "Hong_Kong", "Kolkata", "Seoul", "Shanghai", "Singapore", "Tokyo" }, 7 },
    { "Australia", { "Melbourne", "Perth", "Sydney" }, 3 },
    { "Europe",    { "Amsterdam", "Berlin", "Kyiv", "London", "Madrid", "Moscow", "Paris", "Rome", "Warsaw" }, 9 },
    { "Pacific",   { "Auckland", "Honolulu" }, 2 },
    { "US",        { "Central", "Eastern", "Mountain", "Pacific" }, 4 },
    { "UTC",       { "UTC" }, 1 }
};
static const int g_num_continents = 8;

static void show_text_input(const char *title, const char *prompt, char *buffer, size_t max_len) {
    int w = 64;
    int h = 11;
    update_term_size();
    clear_screen_blue();
    int last_cols = term_cols;
    int last_rows = term_rows;

    int len = strlen(buffer);
    if (len >= (int)max_len) len = max_len - 1;

    while (1) {
        update_term_size();
        if (term_cols != last_cols || term_rows != last_rows) {
            clear_screen_blue();
            last_cols = term_cols;
            last_rows = term_rows;
        }

        int x = (term_cols - w) / 2;
        int y = (term_rows - h) / 2;

        draw_box(x, y, w, h, title);
        write_str(x + 4, y + 2, prompt, BG_WHITE FG_BLACK);

        char field[80];
        int field_w = w - 8;
        if (field_w > 52) field_w = 52;
        snprintf(field, sizeof(field), "[ %-*.*s ]", field_w - 4, field_w - 4, buffer);
        write_str(x + 4, y + 4, field, BG_BLACK FG_WHITE);

        write_str(x + 4, y + 6, "Press Enter to confirm, Backspace to edit.", BG_WHITE FG_BLACK);
        write_str(x + (w - 14) / 2, y + h - 2, "< Continue >", BG_RED FG_WHITE);

        int k = get_key();
        if (k == KEY_ENTER) {
            if (len > 0) {
                break;
            }
        } else if (k == '\b') {
            if (len > 0) {
                len--;
                buffer[len] = '\0';
            }
        } else if (k == KEY_SPACE || (k >= 32 && k <= 126)) {
            char c = (k == KEY_SPACE) ? ' ' : (char)k;
            if (len < (int)max_len - 1 && len < field_w - 6) {
                buffer[len++] = c;
                buffer[len] = '\0';
            }
        }
    }
}

static void show_hostname_step(char *hostname, size_t max_len) {
    show_text_input("System Hostname", "Enter the system hostname for this computer:", hostname, max_len);
    if (strlen(hostname) == 0) {
        sc_strncpy(hostname, "boredos", max_len);
    }
}

static void show_network_step(NetworkConfig *net_cfg) {
    int w = 64;
    int h = 12;
    int selected = net_cfg->is_dhcp ? 0 : 1;

    update_term_size();
    clear_screen_blue();
    int last_cols = term_cols;
    int last_rows = term_rows;

    while (1) {
        update_term_size();
        if (term_cols != last_cols || term_rows != last_rows) {
            clear_screen_blue();
            last_cols = term_cols;
            last_rows = term_rows;
        }

        int x = (term_cols - w) / 2;
        int y = (term_rows - h) / 2;

        draw_box(x, y, w, h, "Network Configuration");
        write_str(x + 4, y + 2, "Choose how network settings will be configured:", BG_WHITE FG_BLACK);

        if (selected == 0) {
            write_str(x + 6, y + 4, "[*] DHCP   - Automatic IP configuration (Recommended)", BG_BLACK FG_WHITE);
            write_str(x + 6, y + 6, "[ ] Static - Manual IP, Netmask, Gateway, and DNS", BG_WHITE FG_BLACK);
        } else {
            write_str(x + 6, y + 4, "[ ] DHCP   - Automatic IP configuration (Recommended)", BG_WHITE FG_BLACK);
            write_str(x + 6, y + 6, "[*] Static - Manual IP, Netmask, Gateway, and DNS", BG_BLACK FG_WHITE);
        }

        write_str(x + (w - 14) / 2, y + h - 2, "< Continue >", BG_RED FG_WHITE);

        int k = get_key();
        if (k == KEY_UP || k == KEY_DOWN) {
            selected = !selected;
        } else if (k == KEY_ENTER || k == KEY_SPACE) {
            break;
        }
    }

    net_cfg->is_dhcp = (selected == 0);

    if (net_cfg->is_dhcp) {
        show_text_input("DNS Nameserver", "Enter the primary DNS nameserver (e.g. 1.1.1.1):",
                        net_cfg->nameserver, sizeof(net_cfg->nameserver));
    } else {
        show_text_input("Static IP Address", "Enter static IPv4 address (e.g. 192.168.1.50):",
                        net_cfg->ip, sizeof(net_cfg->ip));
        show_text_input("Subnet Mask", "Enter subnet mask (e.g. 255.255.255.0):",
                        net_cfg->netmask, sizeof(net_cfg->netmask));
        show_text_input("Default Gateway", "Enter default gateway IP (e.g. 192.168.1.1):",
                        net_cfg->gateway, sizeof(net_cfg->gateway));
        show_text_input("DNS Nameserver", "Enter primary DNS nameserver (e.g. 1.1.1.1):",
                        net_cfg->nameserver, sizeof(net_cfg->nameserver));
    }
}

static void show_timezone_step(char *tz_out, size_t max_len) {
    int cont_idx = 4;
    int w = 62;
    int h = 16;

    while (1) {
        int selected_continent = -1;
        update_term_size();
        clear_screen_blue();
        int last_cols = term_cols;
        int last_rows = term_rows;

        while (1) {
            update_term_size();
            if (term_cols != last_cols || term_rows != last_rows) {
                clear_screen_blue();
                last_cols = term_cols;
                last_rows = term_rows;
            }

            int x = (term_cols - w) / 2;
            int y = (term_rows - h) / 2;

            draw_box(x, y, w, h, "Timezone: Select Region");
            write_str(x + 4, y + 2, "Select your geographic region or continent:", BG_WHITE FG_BLACK);

            for (int i = 0; i < g_num_continents; i++) {
                char line[64];
                snprintf(line, sizeof(line), "  (%c) %-24s", (cont_idx == i) ? '*' : ' ', g_tz_continents[i].continent);
                if (cont_idx == i) {
                    write_str(x + 6, y + 4 + i, line, BG_BLACK FG_WHITE);
                } else {
                    write_str(x + 6, y + 4 + i, line, BG_WHITE FG_BLACK);
                }
            }

            write_str(x + (w - 14) / 2, y + h - 2, "< Select >", BG_RED FG_WHITE);

            int k = get_key();
            if (k == KEY_UP) {
                cont_idx = (cont_idx - 1 + g_num_continents) % g_num_continents;
            } else if (k == KEY_DOWN) {
                cont_idx = (cont_idx + 1) % g_num_continents;
            } else if (k == KEY_ENTER || k == KEY_SPACE) {
                selected_continent = cont_idx;
                break;
            }
        }

        if (sc_strcmp(g_tz_continents[selected_continent].continent, "UTC") == 0) {
            sc_strncpy(tz_out, "UTC", max_len);
            return;
        }

        const TzContinent *tc = &g_tz_continents[selected_continent];
        int city_idx = 0;
        int h_city = 8 + tc->num_cities;
        if (h_city < 13) h_city = 13;
        int back_to_continents = 0;

        update_term_size();
        clear_screen_blue();
        last_cols = term_cols;
        last_rows = term_rows;

        while (1) {
            update_term_size();
            if (term_cols != last_cols || term_rows != last_rows) {
                clear_screen_blue();
                last_cols = term_cols;
                last_rows = term_rows;
            }

            int x = (term_cols - w) / 2;
            int y = (term_rows - h_city) / 2;

            char title[64];
            snprintf(title, sizeof(title), "Timezone: %s", tc->continent);
            draw_box(x, y, w, h_city, title);

            write_str(x + 4, y + 2, "Select your city or local timezone:", BG_WHITE FG_BLACK);

            for (int i = 0; i < tc->num_cities; i++) {
                char line[64];
                snprintf(line, sizeof(line), "  (%c) %-24s", (city_idx == i) ? '*' : ' ', tc->cities[i]);
                if (city_idx == i) {
                    write_str(x + 6, y + 4 + i, line, BG_BLACK FG_WHITE);
                } else {
                    write_str(x + 6, y + 4 + i, line, BG_WHITE FG_BLACK);
                }
            }

            write_str(x + 12, y + h_city - 2, "< Select >", BG_RED FG_WHITE);
            write_str(x + 36, y + h_city - 2, "< Back >", BG_WHITE FG_BLACK);

            int k = get_key();
            if (k == KEY_UP) {
                city_idx = (city_idx - 1 + tc->num_cities) % tc->num_cities;
            } else if (k == KEY_DOWN) {
                city_idx = (city_idx + 1) % tc->num_cities;
            } else if (k == KEY_LEFT || k == KEY_ESC) {
                back_to_continents = 1;
                break;
            } else if (k == KEY_ENTER || k == KEY_SPACE) {
                snprintf(tz_out, max_len, "%s/%s", tc->continent, tc->cities[city_idx]);
                return;
            }
        }

        if (back_to_continents) {
            continue;
        }
    }
}

static void show_ntp_step(int *ntp_enabled, char *ntp_server, size_t max_len) {
    int w = 64;
    int h = 12;
    int selected = (*ntp_enabled) ? 0 : 1;

    update_term_size();
    clear_screen_blue();
    int last_cols = term_cols;
    int last_rows = term_rows;

    while (1) {
        update_term_size();
        if (term_cols != last_cols || term_rows != last_rows) {
            clear_screen_blue();
            last_cols = term_cols;
            last_rows = term_rows;
        }

        int x = (term_cols - w) / 2;
        int y = (term_rows - h) / 2;

        draw_box(x, y, w, h, "Time Synchronization (NTP)");
        write_str(x + 4, y + 2, "Enable automatic network time synchronization on boot?", BG_WHITE FG_BLACK);

        if (selected == 0) {
            write_str(x + 6, y + 4, "[*] Yes - Enable NTP time synchronization (Recommended)", BG_BLACK FG_WHITE);
            write_str(x + 6, y + 6, "[ ] No  - Do not sync time automatically", BG_WHITE FG_BLACK);
        } else {
            write_str(x + 6, y + 4, "[ ] Yes - Enable NTP time synchronization (Recommended)", BG_WHITE FG_BLACK);
            write_str(x + 6, y + 6, "[*] No  - Do not sync time automatically", BG_BLACK FG_WHITE);
        }

        write_str(x + (w - 14) / 2, y + h - 2, "< Continue >", BG_RED FG_WHITE);

        int k = get_key();
        if (k == KEY_UP || k == KEY_DOWN) {
            selected = !selected;
        } else if (k == KEY_ENTER || k == KEY_SPACE) {
            break;
        }
    }

    *ntp_enabled = (selected == 0);

    if (*ntp_enabled) {
        show_text_input("NTP Server", "Enter the NTP time server pool (e.g. pool.ntp.org):",
                        ntp_server, max_len);
    }
}

static void show_nova_step(int *nova_enabled) {
    int w = 64;
    int h = 12;
    int selected = (*nova_enabled) ? 0 : 1;

    update_term_size();
    clear_screen_blue();
    int last_cols = term_cols;
    int last_rows = term_rows;

    while (1) {
        update_term_size();
        if (term_cols != last_cols || term_rows != last_rows) {
            clear_screen_blue();
            last_cols = term_cols;
            last_rows = term_rows;
        }

        int x = (term_cols - w) / 2;
        int y = (term_rows - h) / 2;

        draw_box(x, y, w, h, "Desktop Environment (Nova)");
        write_str(x + 4, y + 2, "Enable Nova Desktop Environment on startup?", BG_WHITE FG_BLACK);

        if (selected == 0) {
            write_str(x + 6, y + 4, "[*] Yes - Launch Nova desktop automatically", BG_BLACK FG_WHITE);
            write_str(x + 6, y + 6, "[ ] No  - Boot to text console (CLI)", BG_WHITE FG_BLACK);
        } else {
            write_str(x + 6, y + 4, "[ ] Yes - Launch Nova desktop automatically", BG_WHITE FG_BLACK);
            write_str(x + 6, y + 6, "[*] No  - Boot to text console (CLI)", BG_BLACK FG_WHITE);
        }

        write_str(x + (w - 14) / 2, y + h - 2, "< Continue >", BG_RED FG_WHITE);

        int k = get_key();
        if (k == KEY_UP || k == KEY_DOWN) {
            selected = !selected;
        } else if (k == KEY_ENTER || k == KEY_SPACE) {
            break;
        }
    }

    *nova_enabled = (selected == 0);
}

static int show_confirmation(const char *diskname, int fs_choice, const char *hostname,
                             const NetworkConfig *net_cfg, const char *timezone_str,
                             int ntp_enabled, const char *ntp_server, int nova_enabled) {
    int w = 68;
    int h = 19;
    int selected = 1; 
    
    update_term_size();
    clear_screen_blue();
    int last_cols = term_cols;
    int last_rows = term_rows;
    
    while (1) {
        update_term_size();
        if (term_cols != last_cols || term_rows != last_rows) {
            clear_screen_blue();
            last_cols = term_cols;
            last_rows = term_rows;
        }
        
        int x = (term_cols - w) / 2;
        int y = (term_rows - h) / 2;
        
        draw_box(x, y, w, h, "Confirm Installation Summary");
        
        write_str(x + 4, y + 2, "The installer is ready to install BoredOS with these settings:", BG_WHITE FG_BLACK);

        char line[128];
        snprintf(line, sizeof(line), "  Target Disk:  /dev/%-8s  (Root FS: %s)", diskname, (fs_choice == 0) ? "ext4" : "FAT32");
        write_str(x + 4, y + 4, line, BG_WHITE FG_BLACK);

        snprintf(line, sizeof(line), "  Hostname:     %-20s", hostname);
        write_str(x + 4, y + 5, line, BG_WHITE FG_BLACK);

        if (net_cfg->is_dhcp) {
            snprintf(line, sizeof(line), "  Networking:   DHCP (DNS: %s)", net_cfg->nameserver);
        } else {
            snprintf(line, sizeof(line), "  Networking:   Static IP: %s (GW: %s)", net_cfg->ip, net_cfg->gateway);
        }
        write_str(x + 4, y + 6, line, BG_WHITE FG_BLACK);

        snprintf(line, sizeof(line), "  Timezone:     %-20s", timezone_str);
        write_str(x + 4, y + 7, line, BG_WHITE FG_BLACK);

        if (ntp_enabled) {
            snprintf(line, sizeof(line), "  NTP Sync:     Enabled (%s)", ntp_server);
        } else {
            snprintf(line, sizeof(line), "  NTP Sync:     Disabled");
        }
        write_str(x + 4, y + 8, line, BG_WHITE FG_BLACK);

        snprintf(line, sizeof(line), "  Nova Desktop: %s", nova_enabled ? "Enabled (Start on boot)" : "Disabled (Boot to CLI)");
        write_str(x + 4, y + 9, line, BG_WHITE FG_BLACK);

        char warn[128];
        snprintf(warn, sizeof(warn), "! WARNING: ALL DATA on /dev/%s will be ERASED !", diskname);
        write_str(x + 4, y + 11, warn, BG_WHITE FG_RED);
        write_str(x + 4, y + 12, "This operation is IRREVERSIBLE. Do you wish to proceed?", BG_WHITE FG_BLACK);
        
        if (selected == 0) {
            write_str(x + 14, y + 15, "<  Yes, Install  >", BG_RED FG_WHITE);
            write_str(x + 38, y + 15, "<  No, Cancel  >",   BG_WHITE FG_BLACK);
        } else {
            write_str(x + 14, y + 15, "<  Yes, Install  >", BG_WHITE FG_BLACK);
            write_str(x + 38, y + 15, "<  No, Cancel  >",   BG_RED FG_WHITE);
        }
        
        int k = get_key();
        if (k == KEY_LEFT || k == KEY_RIGHT || k == '\t') {
            selected = !selected;
        } else if (k == KEY_ENTER) {
            return (selected == 0); 
        }
    }
}

static void update_rc_conf(const char *path, const char *hostname, const NetworkConfig *net_cfg,
                           const char *timezone_str, int ntp_enabled, const char *ntp_server,
                           int nova_enabled) {
    int fd = sys_open(path, "r");
    if (fd < 0) return;

    char *buf = (char *)malloc(65536);
    char *out = (char *)malloc(65536);
    if (!buf || !out) {
        if (buf) free(buf);
        if (out) free(out);
        sys_close(fd);
        return;
    }

    int n = sys_read(fd, buf, 65536 - 1);
    sys_close(fd);
    if (n <= 0) {
        free(buf);
        free(out);
        return;
    }
    buf[n] = '\0';

    int out_len = 0;
    int seen_hostname = 0;
    int seen_timezone = 0;
    int seen_ifconfig_auto = 0;
    int seen_ifconfig_default = 0;
    int seen_defaultrouter = 0;
    int seen_nameserver = 0;
    int seen_ntp_enable = 0;
    int seen_ntp_server = 0;
    int seen_nova_enable = 0;

    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';

        int llen = strlen(line);
        if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';

        char *trim = line;
        while (*trim == ' ' || *trim == '\t') trim++;

        if (strncmp(trim, "hostname=", 9) == 0) {
            seen_hostname = 1;
            out_len += snprintf(out + out_len, 65536 - out_len, "hostname=\"%s\"\n", hostname);
        } else if (strncmp(trim, "timezone=", 9) == 0) {
            seen_timezone = 1;
            out_len += snprintf(out + out_len, 65536 - out_len, "timezone=\"%s\"\n", timezone_str);
        } else if (strncmp(trim, "ifconfig_auto=", 14) == 0) {
            seen_ifconfig_auto = 1;
            if (net_cfg->is_dhcp) {
                out_len += snprintf(out + out_len, 65536 - out_len, "ifconfig_auto=\"DHCP\"\n");
            } else {
                seen_ifconfig_default = 1;
                seen_defaultrouter = 1;
                out_len += snprintf(out + out_len, 65536 - out_len,
                    "ifconfig_auto=\"NO\"\n"
                    "ifconfig_DEFAULT=\"inet %s netmask %s\"\n"
                    "defaultrouter=\"%s\"\n",
                    net_cfg->ip, net_cfg->netmask, net_cfg->gateway);
            }
        } else if (strncmp(trim, "ifconfig_DEFAULT=", 17) == 0) {
            if (!seen_ifconfig_default && !net_cfg->is_dhcp) {
                seen_ifconfig_default = 1;
                out_len += snprintf(out + out_len, 65536 - out_len, "ifconfig_DEFAULT=\"inet %s netmask %s\"\n",
                                    net_cfg->ip, net_cfg->netmask);
            }
        } else if (strncmp(trim, "defaultrouter=", 14) == 0) {
            if (!seen_defaultrouter && !net_cfg->is_dhcp) {
                seen_defaultrouter = 1;
                out_len += snprintf(out + out_len, 65536 - out_len, "defaultrouter=\"%s\"\n",
                                    net_cfg->gateway);
            }
        } else if (strncmp(trim, "nameserver=", 11) == 0) {
            seen_nameserver = 1;
            out_len += snprintf(out + out_len, 65536 - out_len, "nameserver=\"%s\"\n", net_cfg->nameserver);
        } else if (strncmp(trim, "ntp_enable=", 11) == 0) {
            seen_ntp_enable = 1;
            out_len += snprintf(out + out_len, 65536 - out_len, "ntp_enable=\"%s\"\n", ntp_enabled ? "YES" : "NO");
        } else if (strncmp(trim, "ntp_server=", 11) == 0) {
            seen_ntp_server = 1;
            out_len += snprintf(out + out_len, 65536 - out_len, "ntp_server=\"%s\"\n", ntp_server);
        } else if (strncmp(trim, "nova_enable=", 12) == 0) {
            seen_nova_enable = 1;
            out_len += snprintf(out + out_len, 65536 - out_len, "nova_enable=\"%s\"\n", nova_enabled ? "YES" : "NO");
        } else {
            out_len += snprintf(out + out_len, 65536 - out_len, "%s\n", line);
        }

        if (next) line = next + 1;
        else line = NULL;
    }

    if (!seen_hostname) {
        out_len += snprintf(out + out_len, 65536 - out_len, "hostname=\"%s\"\n", hostname);
    }
    if (!seen_timezone) {
        out_len += snprintf(out + out_len, 65536 - out_len, "timezone=\"%s\"\n", timezone_str);
    }
    if (!seen_ifconfig_auto) {
        if (net_cfg->is_dhcp) {
            out_len += snprintf(out + out_len, 65536 - out_len, "ifconfig_auto=\"DHCP\"\n");
        } else {
            out_len += snprintf(out + out_len, 65536 - out_len,
                "ifconfig_auto=\"NO\"\n"
                "ifconfig_DEFAULT=\"inet %s netmask %s\"\n"
                "defaultrouter=\"%s\"\n",
                net_cfg->ip, net_cfg->netmask, net_cfg->gateway);
        }
    }
    if (!seen_nameserver) {
        out_len += snprintf(out + out_len, 65536 - out_len, "nameserver=\"%s\"\n", net_cfg->nameserver);
    }
    if (!seen_ntp_enable) {
        out_len += snprintf(out + out_len, 65536 - out_len, "ntp_enable=\"%s\"\n", ntp_enabled ? "YES" : "NO");
    }
    if (!seen_ntp_server) {
        out_len += snprintf(out + out_len, 65536 - out_len, "ntp_server=\"%s\"\n", ntp_server);
    }
    if (!seen_nova_enable) {
        out_len += snprintf(out + out_len, 65536 - out_len, "nova_enable=\"%s\"\n", nova_enabled ? "YES" : "NO");
    }

    sys_delete(path);
    int wfd = sys_open(path, "w");
    if (wfd >= 0) {
        sys_write_fs(wfd, out, out_len);
        sys_close(wfd);
    }

    free(buf);
    free(out);
}

static void apply_system_configuration(const char *hostname, const NetworkConfig *net_cfg,
                                       const char *timezone_str, int ntp_enabled,
                                       const char *ntp_server, int nova_enabled) {
    update_rc_conf("/mnt/etc/rc.conf", hostname, net_cfg, timezone_str, ntp_enabled, ntp_server, nova_enabled);

    int fd_hn = sys_open("/mnt/etc/hostname", "w");
    if (fd_hn >= 0) {
        char hn_buf[128];
        int len = snprintf(hn_buf, sizeof(hn_buf), "%s\n", hostname);
        if (len > 0) sys_write_fs(fd_hn, hn_buf, len);
        sys_close(fd_hn);
    }

    int fd_tz = sys_open("/mnt/etc/timezone", "w");
    if (fd_tz >= 0) {
        char tz_buf[128];
        int len = snprintf(tz_buf, sizeof(tz_buf), "%s\n", timezone_str);
        if (len > 0) sys_write_fs(fd_tz, tz_buf, len);
        sys_close(fd_tz);
    }

    char src_tz[128];
    snprintf(src_tz, sizeof(src_tz), "/mnt/usr/share/zoneinfo/%s", timezone_str);
    copy_file_optional(src_tz, "/mnt/etc/localtime");

    int fd_res = sys_open("/mnt/etc/resolv.conf", "w");
    if (fd_res >= 0) {
        char res_buf[128];
        int len = snprintf(res_buf, sizeof(res_buf), "nameserver %s\n", net_cfg->nameserver);
        if (len > 0) sys_write_fs(fd_res, res_buf, len);
        sys_close(fd_res);
    }
}

static int show_developer_warning(void) {
    int w = 66;
    int h = 17;
    int selected = 1; /* default to "No" */

    update_term_size();
    clear_screen_blue();
    int last_cols = term_cols;
    int last_rows = term_rows;

    while (1) {
        update_term_size();
        if (term_cols != last_cols || term_rows != last_rows) {
            clear_screen_blue();
            last_cols = term_cols;
            last_rows = term_rows;
        }

        int x = (term_cols - w) / 2;
        int y = (term_rows - h) / 2;

        draw_box(x, y, w, h, "Developer Warning");

        write_str(x + 4, y + 2,  "! This is a DEVELOPER BETA build of BoredOS.",        BG_WHITE FG_RED);
        write_str(x + 4, y + 3,  "  It will very likely always remain a beta release.", BG_WHITE FG_BLACK);

        write_str(x + 4, y + 5,  "! THIS SOFTWARE IS UNSTABLE.",                        BG_WHITE FG_RED);
        write_str(x + 4, y + 6,  "  It may corrupt data, destroy partitions, fail to",  BG_WHITE FG_BLACK);
        write_str(x + 4, y + 7,  "  boot, or damage your system in unexpected ways.",    BG_WHITE FG_BLACK);
        write_str(x + 4, y + 8,  "  The BoredOS developers are NOT responsible for",    BG_WHITE FG_BLACK);
        write_str(x + 4, y + 9,  "  any damage or data loss caused by this software.",  BG_WHITE FG_BLACK);

        write_str(x + 4, y + 11, "! THIS OS REQUIRES COMMAND-LINE KNOWLEDGE.",          BG_WHITE FG_RED);
        write_str(x + 4, y + 12, "  If you have never used a shell, terminal, or CLI,", BG_WHITE FG_BLACK);
        write_str(x + 4, y + 13, "  please press < No > and do not proceed.",           BG_WHITE FG_BLACK);

        if (selected == 0) {
            write_str(x + 10, y + h - 2, "< I understand, continue >", BG_RED FG_WHITE);
            write_str(x + 40, y + h - 2, "<  No, go back  >",          BG_WHITE FG_BLACK);
        } else {
            write_str(x + 10, y + h - 2, "< I understand, continue >", BG_WHITE FG_BLACK);
            write_str(x + 40, y + h - 2, "<  No, go back  >",          BG_RED FG_WHITE);
        }

        int k = get_key();
        if (k == KEY_LEFT || k == KEY_RIGHT || k == '\t') {
            selected = !selected;
        } else if (k == KEY_ENTER) {
            return (selected == 0);
        }
    }
}

static void show_message(const char *title, const char *msg1, const char *msg2) {
    update_term_size();
    clear_screen_blue();
    
    int w = 60;
    int h = 10;
    int x = (term_cols - w) / 2;
    int y = (term_rows - h) / 2;
    
    draw_box(x, y, w, h, title);
    write_str(x + 4, y + 2, msg1, BG_WHITE FG_BLACK);
    if (msg2) {
        write_str(x + 4, y + 4, msg2, BG_WHITE FG_BLACK);
    }
    
    write_str(x + (w - 10) / 2, y + 7, "< Reboot >", BG_RED FG_WHITE);
    
    while (1) {
        int k = get_key();
        if (k == KEY_ENTER || k == KEY_SPACE) {
            break;
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    int fg = getpid();
    ioctl(0, TIOCSPGRP, &fg);
    signal(SIGWINCH, handle_installer_sigwinch);

    sys_write(1, "\x1b[?25l", 6); 
    update_term_size();
    load_excludes();
    
    TargetDisk disks[16];
    int num_disks = get_available_disks(disks, 16);
    if (num_disks == 0) {
        show_message("Error", "No hard disks detected on this computer.", "Cannot install BoredOS. Exiting.");
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 1;
    }
    
    show_welcome();
    
    int disk_idx = show_disk_selection(disks, num_disks);
    const char *devname = disks[disk_idx].devname;
    
    PackageOption options[64];
    int num_options = get_package_options(options, 64);
    
    if (num_options > 0) {
        show_package_selection(options, num_options);
    }

    int fs_choice = show_fs_selection();

    char hostname[64] = "boredos";
    show_hostname_step(hostname, sizeof(hostname));

    NetworkConfig net_cfg = {
        .is_dhcp = 1,
        .ip = "192.168.1.50",
        .netmask = "255.255.255.0",
        .gateway = "192.168.1.1",
        .nameserver = "1.1.1.1"
    };
    show_network_step(&net_cfg);

    char timezone_str[64] = "Europe/Amsterdam";
    show_timezone_step(timezone_str, sizeof(timezone_str));

    int ntp_enabled = 1;
    char ntp_server[64] = "pool.ntp.org";
    show_ntp_step(&ntp_enabled, ntp_server, sizeof(ntp_server));

    int nova_enabled = 0;
    show_nova_step(&nova_enabled);
    
    if (!show_confirmation(devname, fs_choice, hostname, &net_cfg, timezone_str, ntp_enabled, ntp_server, nova_enabled)) {
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 0;
    }

    if (!show_developer_warning()) {
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 0;
    }    
    
    update_term_size();
    clear_screen_blue();
    
    int is_uefi = 1;
    int esp_size_mb = 512;
    
    show_progress("Partitioning target disk /dev/...", 5);
    
    char fdisk_args[128];
    int ai = 0;
    const char *cmd = "--script ";
    for (; *cmd; cmd++) fdisk_args[ai++] = *cmd;
    if (is_uefi) {
        const char *u = "--uefi --esp-size ";
        for (; *u; u++) fdisk_args[ai++] = *u;
        char num[16]; int ni = 0;
        int v = esp_size_mb;
        if (v == 0) { num[ni++] = '0'; } else {
            char tmp[16]; int ti = 0;
            while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
            for (int j = ti - 1; j >= 0; j--) num[ni++] = tmp[j];
        }
        for (int j = 0; j < ni; j++) fdisk_args[ai++] = num[j];
        fdisk_args[ai++] = ' ';
    } else {
        const char *b = "--mbr "; for (; *b; b++) fdisk_args[ai++] = *b;
    }
    fdisk_args[ai++] = '/'; fdisk_args[ai++] = 'd'; fdisk_args[ai++] = 'e';
    fdisk_args[ai++] = 'v'; fdisk_args[ai++] = '/';
    for (int j = 0; devname[j]; j++) fdisk_args[ai++] = devname[j];
    fdisk_args[ai] = 0;

    int status = run_command_silent("/bin/fdisk.elf", fdisk_args, "/tmp/fdisk.log");
    if (status != 0) {
        show_message("Error", "fdisk failed to partition the disk.", NULL);
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 1;
    }
    clear_screen_blue();
    s_last_percent = -1;
    s_last_stage = NULL;
    sys_disk_sync(devname);
    sys_disk_rescan(devname);
    
    char esp_dev[16]  = {0};
    char root_dev[16] = {0};
    
    const char *raw_devname = devname;
    if (strncmp(raw_devname, "/dev/", 5) == 0) raw_devname += 5;
    size_t dev_len = strlen(raw_devname);

    if (is_uefi) {
        snprintf(esp_dev, sizeof(esp_dev), "%s1", raw_devname);
        snprintf(root_dev, sizeof(root_dev), "%s2", raw_devname);
    } else {
        snprintf(root_dev, sizeof(root_dev), "%s1", raw_devname);
    }

    char test_path[64];
    bool partitions_ready = false;
    for (int retry = 0; retry < 30; retry++) {
        bool ok = true;
        snprintf(test_path, sizeof(test_path), "/dev/%s", root_dev);
        int tfd = open(test_path, O_RDONLY);
        if (tfd >= 0) {
            close(tfd);
        } else {
            ok = false;
        }

        if (is_uefi && esp_dev[0]) {
            snprintf(test_path, sizeof(test_path), "/dev/%s", esp_dev);
            tfd = open(test_path, O_RDONLY);
            if (tfd >= 0) {
                close(tfd);
            } else {
                ok = false;
            }
        }

        if (ok) {
            partitions_ready = true;
            break;
        }
        sys_disk_rescan(devname);
        usleep(100000);
    }
    
    if (!partitions_ready || !root_dev[0] || (is_uefi && !esp_dev[0])) {
        char err_detail[256] = {0};
        FILE *lf = fopen("/tmp/fdisk.log", "r");
        if (lf) {
            size_t n = fread(err_detail, 1, sizeof(err_detail) - 1, lf);
            err_detail[n] = '\0';
            fclose(lf);
        }
        show_message("Error", "Could not locate target partitions after partitioning.", err_detail[0] ? err_detail : NULL);
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 1;
    }
    
    if (is_uefi) {
        show_progress("Formatting EFI partition (FAT32)...", 12);
        char fat_args[64];
        snprintf(fat_args, sizeof(fat_args), "-F 32 -n EFI /dev/%s", esp_dev);
        int mstatus = run_command_silent("/bin/mkfs_fat.elf", fat_args, "/tmp/mkfs_fat.log");
        if (mstatus != 0) {
            int code = (mstatus >> 8) ? (mstatus >> 8) : mstatus;
            char err_detail[128];
            char log_buf[128] = {0};
            int lfd = open("/tmp/mkfs_fat.log", O_RDONLY);
            if (lfd >= 0) {
                read(lfd, log_buf, sizeof(log_buf) - 1);
                close(lfd);
            }
            if (log_buf[0]) {
                snprintf(err_detail, sizeof(err_detail), "mkfs_fat failed (code %d): %s", code, log_buf);
            } else {
                snprintf(err_detail, sizeof(err_detail), "mkfs_fat failed (status %d, code %d)", mstatus, code);
            }
            show_message("Error", "Failed to format ESP partition.", err_detail);
            sys_write(1, "\x1b[?25h\x1b[0m", 10);
            clear_screen();
            return 1;
        }
    }

    show_progress(fs_choice == 0 ? "Formatting Root partition (ext4)..." : "Formatting Root partition (FAT32)...", 18);
    char root_fs_args[64];
    const char *mkfs_prog = (fs_choice == 0) ? "/bin/mkfs_ext4.elf" : "/bin/mkfs_fat.elf";
    if (fs_choice == 0) {
        snprintf(root_fs_args, sizeof(root_fs_args), "-L BOREDOS /dev/%s", root_dev);
    } else {
        snprintf(root_fs_args, sizeof(root_fs_args), "-F 32 -n BOREDOS /dev/%s", root_dev);
    }

    int rstatus = run_command_silent(mkfs_prog, root_fs_args, "/tmp/mkfs_root.log");
    if (rstatus != 0) {
        int code = (rstatus >> 8) ? (rstatus >> 8) : rstatus;
        char err_detail[128];
        char log_buf[128] = {0};
        int lfd = open("/tmp/mkfs_root.log", O_RDONLY);
        if (lfd < 0) lfd = open("/tmp/mkfs_ext4.log", O_RDONLY);
        if (lfd >= 0) {
            read(lfd, log_buf, sizeof(log_buf) - 1);
            close(lfd);
        }
        if (log_buf[0]) {
            snprintf(err_detail, sizeof(err_detail), "%s failed (code %d): %s", mkfs_prog, code, log_buf);
        } else {
            snprintf(err_detail, sizeof(err_detail), "%s failed (status %d, code %d)", mkfs_prog, rstatus, code);
        }
        show_message("Error", "Failed to format root partition.", err_detail);
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 1;
    }

    clear_screen_blue();
    s_last_percent = -1;
    s_last_stage = NULL;
    sys_disk_rescan(devname);
    
    sys_mkdir("/mnt");
    sys_mkdir("/mnt/boot");
    sys_mkdir("/mnt/esp");
    
    if (sys_disk_mount(root_dev, "/mnt") != 0) {
        show_message("Error", "Failed to mount root partition to /mnt.", NULL);
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 1;
    }
    if (is_uefi) {
        sys_mkdir("/mnt/boot");
        if (sys_disk_mount(esp_dev, "/mnt/boot") != 0) {
            show_message("Error", "Failed to mount ESP partition to /mnt/boot.", NULL);
            sys_write(1, "\x1b[?25h\x1b[0m", 10);
            clear_screen();
            return 1;
        }
    } else {
        sys_mkdir("/mnt/boot");
    }
    
    show_progress("Copying system binaries (/bin)...", 30);
    if (copy_tree("/bin", "/mnt/bin") != 0) {
        show_message("Error", "Failed to copy essential binaries to target.", g_copy_err[0] ? g_copy_err : NULL);
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 1;
    }
    
    show_progress("Copying system libraries (/Library)...", 45);
    if (copy_tree("/Library", "/mnt/Library") != 0) {
        show_message("Error", "Failed to copy /Library contents.", g_copy_err[0] ? g_copy_err : NULL);
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 1;
    }
    
    show_progress("Copying other essential system paths...", 60);
    copy_tree("/docs", "/mnt/docs");
    copy_tree("/root", "/mnt/root");
    copy_tree("/usr", "/mnt/usr");
    copy_tree("/etc", "/mnt/etc");
    apply_system_configuration(hostname, &net_cfg, timezone_str, ntp_enabled, ntp_server, nova_enabled);
    sys_mkdir("/mnt/tmp");
    sys_mkdir("/mnt/var");
    sys_mkdir("/mnt/var/run");
    sys_mkdir("/mnt/dev");
    sys_mkdir("/mnt/proc");
    sys_mkdir("/mnt/sys");
    
    if (copy_file("/boot/boredos.elf", "/mnt/boot/boredos.elf") != 0) {
        char err[128];
        snprintf(err, sizeof(err), "copy failed: %s", g_copy_err);
        show_message("Error", "Failed to copy kernel to target boot.", err);
        sys_write(1, "\x1b[?25h\x1b[0m", 10);
        clear_screen();
        return 1;
    }
    
    int num_selected = 0;
    for (int i = 0; i < num_options; i++) {
        if (options[i].enabled) num_selected++;
    }
    
    int installed_count = 0;
    for (int i = 0; i < num_options; i++) {
        if (options[i].enabled) {
            int pct = 70 + (installed_count * 20) / num_selected;
            char msg[128];
            snprintf(msg, sizeof(msg), "Installing package %s...", options[i].pkgname);
            show_progress(msg, pct);
            
            char bpm_args[256];
            snprintf(bpm_args, sizeof(bpm_args), "--root /mnt install /usr/share/packages/%s", options[i].filename);
            
            run_command_silent("/bin/bpm.elf", bpm_args, "/tmp/bpm.log");
            installed_count++;
        }
    }
    
    show_progress("Configuring Limine bootloader...", 95);
    if (is_uefi) {
        sys_mkdir("/mnt/boot/EFI");
        sys_mkdir("/mnt/boot/EFI/BOOT");
        copy_file("/boot/BOOTX64.EFI", "/mnt/boot/EFI/BOOT/BOOTX64.EFI");
        copy_file_optional("/boot/BOOTIA32.EFI", "/mnt/boot/EFI/BOOT/BOOTIA32.EFI");
        copy_file_optional("/boot/splash.jpg", "/mnt/boot/splash.jpg");
        
        int fd = sys_open("/mnt/boot/limine.conf", "w");
        if (fd >= 0) {
            char cfg[1024];
            int len = snprintf(cfg, sizeof(cfg),
                "timeout: 3\n"
                "verbose: yes\n"
                "\n"
                "${WALLPAPER_PATH}=boot():/splash.jpg\n"
                "\n"
                "wallpaper: ${WALLPAPER_PATH}\n"
                "wallpaper_style: stretched\n"
                "backdrop: 000000\n"
                "term_margin: 200\n"
                "interface_branding: BoredOS\n"
                "\n"
                "/BoredOS\n"
                "    protocol: limine\n"
                "    path: boot():/boredos.elf\n"
                "    cmdline: -v root=/dev/%s init=/bin/yawn.elf\n"
                "\n"
                "/  └──> BoredOS (Silent)\n"
                "    protocol: limine\n"
                "    path: boot():/boredos.elf\n"
                "    cmdline: root=/dev/%s init=/bin/yawn.elf\n",
                root_dev, root_dev);
            if (len > 0) sys_write_fs(fd, cfg, len);
            sys_close(fd);
        }
    } else {
        copy_file_optional("/boot/limine-bios.sys", "/mnt/limine-bios.sys");
        copy_file_optional("/boot/splash.jpg", "/mnt/splash.jpg");
        copy_file_optional("/boot/splash.jpg", "/mnt/boot/splash.jpg");
        int fd = sys_open("/mnt/limine.conf", "w");
        if (fd >= 0) {
            char cfg[1024];
            int len = snprintf(cfg, sizeof(cfg),
                "timeout: 3\n"
                "verbose: yes\n"
                "\n"
                "${WALLPAPER_PATH}=boot():/splash.jpg\n"
                "\n"
                "wallpaper: ${WALLPAPER_PATH}\n"
                "wallpaper_style: stretched\n"
                "backdrop: 000000\n"
                "term_margin: 200\n"
                "interface_branding: BoredOS\n"
                "\n"
                "/BoredOS\n"
                "    protocol: limine\n"
                "    root: boot()\n"
                "    path: /boredos.elf\n"
                "    cmdline: -v root=/dev/%s init=/bin/yawn.elf\n"
                "\n"
                "/  └──> BoredOS (Silent)\n"
                "    protocol: limine\n"
                "    root: boot()\n"
                "    path: /boredos.elf\n"
                "    cmdline: root=/dev/%s init=/bin/yawn.elf\n",
                root_dev, root_dev);
            if (len > 0) sys_write_fs(fd, cfg, len);
            sys_close(fd);
        }
    }

    int fd_motd = sys_open("/mnt/etc/motd", "w");
    if (fd_motd >= 0) {
        const char *inst_motd =
            "\n"
            " BoredOS\n"
            " --------------------------------------------------\n"
            " If you're seeing this, your installation\n"
            " Succeeded. Now it's time to configure\n"
            " Your system. Please adjust /etc/rc.conf\n"
            " To your liking.\n"
            " Thanks for instaling BoredOS!\n"
            "\n"
            " * Website:              https://boredos.dev\n"
            " * System settings:      /etc/rc.conf\n"
            " * Default editor:       tvi (or kilo)\n"
            " * Documentation:        /docs/README.md\n"
            "\n"
            " To hide this banner, edit, remove /etc/motd. or disable\n"
            " motd in /etc/rc.conf\n"
            "\n";
        sys_write_fs(fd_motd, inst_motd, strlen(inst_motd));
        sys_close(fd_motd);
    }
    
    show_progress("Finalizing installation (syncing files)...", 98);
    if (is_uefi) {
        sys_disk_sync("/mnt/boot");
        sys_disk_umount("/mnt/boot");
    }
    sys_disk_sync("/mnt");
    sys_disk_umount("/mnt");
    sys_disk_sync(devname);
    
    show_progress("Installation complete!", 100);
    usleep(500000);
    
    sys_write(1, "\x1b[?25h\x1b[0m", 10);
    clear_screen();
    
    char ok_msg[128];
    snprintf(ok_msg, sizeof(ok_msg), "BoredOS has been successfully installed on /dev/%s.", root_dev);
    show_message("Installation Successful", ok_msg, "Press Enter to reboot and start BoredOS!");
    
    sys_reboot();
    return 0;
}
