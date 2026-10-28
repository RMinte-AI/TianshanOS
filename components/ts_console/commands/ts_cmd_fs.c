/**
 * @file ts_cmd_fs.c
 * @brief File System Shell Commands
 * 
 * 实现类 Unix 文件操作命令：
 * - ls [path]           列出目录内容
 * - cat <file>          显示文件内容
 * - cd <path>           切换目录
 * - pwd                 显示当前目录
 * - mkdir <path>        创建目录
 * - rm <path>           删除文件或目录
 * - cp <src> <dst>      复制文件
 * - mv <src> <dst>      移动/重命名文件
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 * @date 2026-01-15
 */

#include "ts_console.h"
#include "ts_storage.h"
#include "ts_api.h"
#include "ts_log.h"
#include "argtable3/argtable3.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define TAG "cmd_fs"

/*===========================================================================*/
/*                          Global State                                      */
/*===========================================================================*/

// 当前工作目录
static char s_cwd[256] = "/sdcard";

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief 格式化文件大小为人类可读格式
 */
static void format_size(size_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(buf, len, "%.1fG", (float)bytes / (1024 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, len, "%.1fM", (float)bytes / (1024 * 1024));
    } else if (bytes >= 1024) {
        snprintf(buf, len, "%.1fK", (float)bytes / 1024);
    } else {
        snprintf(buf, len, "%zu", bytes);
    }
}

/**
 * @brief 将相对路径转换为绝对路径
 */
static void resolve_path(const char *path, char *resolved, size_t len)
{
    if (!path || strlen(path) == 0) {
        strncpy(resolved, s_cwd, len);
        resolved[len - 1] = '\0';
        return;
    }
    
    if (path[0] == '/') {
        // 绝对路径
        strncpy(resolved, path, len);
        resolved[len - 1] = '\0';
    } else if (strcmp(path, "..") == 0) {
        // 上级目录
        strncpy(resolved, s_cwd, len);
        resolved[len - 1] = '\0';
        char *last_slash = strrchr(resolved, '/');
        if (last_slash && last_slash != resolved) {
            *last_slash = '\0';
        }
    } else if (strcmp(path, ".") == 0) {
        // 当前目录
        strncpy(resolved, s_cwd, len);
        resolved[len - 1] = '\0';
    } else {
        // 相对路径
        if (strcmp(s_cwd, "/") == 0) {
            snprintf(resolved, len, "/%s", path);
        } else {
            snprintf(resolved, len, "%s/%s", s_cwd, path);
        }
    }
    
    // 移除尾部斜杠
    size_t rlen = strlen(resolved);
    if (rlen > 1 && resolved[rlen - 1] == '/') {
        resolved[rlen - 1] = '\0';
    }
}

/*===========================================================================*/
/*                          Command: ls                                       */
/*===========================================================================*/

static struct {
    struct arg_lit *all;
    struct arg_lit *long_fmt;
    struct arg_lit *human;
    struct arg_str *path;
    struct arg_lit *help;
    struct arg_end *end;
} s_ls_args;

static int cmd_ls(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_ls_args);
    
    if (s_ls_args.help->count > 0) {
        ts_console_printf("Usage: ls [options] [path]\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  -a, --all     Show hidden files\n");
        ts_console_printf("  -l, --long    Long format\n");
        ts_console_printf("  -h, --human   Human readable sizes\n");
        ts_console_printf("      --help    Show this help\n\n");
        ts_console_printf("Examples:\n");
        ts_console_printf("  ls\n");
        ts_console_printf("  ls -l /sdcard\n");
        ts_console_printf("  ls -la /spiffs\n");
        return 0;
    }
    
    if (nerrors != 0) {
        arg_print_errors(stderr, s_ls_args.end, "ls");
        return 1;
    }
    
    const char *path_arg = s_ls_args.path->count > 0 ? s_ls_args.path->sval[0] : NULL;
    bool show_all = s_ls_args.all->count > 0;
    bool long_fmt = s_ls_args.long_fmt->count > 0;
    bool human = s_ls_args.human->count > 0 || long_fmt;
    
    char resolved[256];
    resolve_path(path_arg, resolved, sizeof(resolved));
    
    // 检查是否是文件
    struct stat st;
    if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) {
        // 单个文件
        char size_str[16];
        if (human) {
            format_size(st.st_size, size_str, sizeof(size_str));
        } else {
            snprintf(size_str, sizeof(size_str), "%ld", (long)st.st_size);
        }
        
        if (long_fmt) {
            ts_console_printf("-rw-r--r-- %8s %s\n", size_str, resolved);
        } else {
            ts_console_printf("%s\n", resolved);
        }
        return 0;
    }
    
    DIR *dir = opendir(resolved);
    if (!dir) {
        ts_console_error("ls: cannot access '%s': %s\n", resolved, strerror(errno));
        return 1;
    }
    
    struct dirent *entry;
    char fullpath[512];
    char size_str[16];
    int count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        // 跳过 . 和 .. 除非指定 -a
        if (!show_all) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            // 跳过隐藏文件
            if (entry->d_name[0] == '.') {
                continue;
            }
        }
        
        snprintf(fullpath, sizeof(fullpath), "%s/%s", resolved, entry->d_name);
        
        bool is_dir = false;
        size_t file_size = 0;
        
        if (stat(fullpath, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            file_size = st.st_size;
        }
        
        if (human) {
            format_size(file_size, size_str, sizeof(size_str));
        } else {
            snprintf(size_str, sizeof(size_str), "%zu", file_size);
        }
        
        if (long_fmt) {
            ts_console_printf("%s %8s %s%s%s\n",
                is_dir ? "drwxr-xr-x" : "-rw-r--r--",
                is_dir ? "-" : size_str,
                is_dir ? "\033[34m" : "",
                entry->d_name,
                is_dir ? "/\033[0m" : "");
        } else {
            if (is_dir) {
                ts_console_printf("\033[34m%s/\033[0m  ", entry->d_name);
            } else {
                ts_console_printf("%s  ", entry->d_name);
            }
            count++;
            if (count % 4 == 0) {
                ts_console_printf("\n");
            }
        }
    }
    
    if (!long_fmt && count % 4 != 0) {
        ts_console_printf("\n");
    }
    
    closedir(dir);
    return 0;
}

/*===========================================================================*/
/*                          Command: cat                                      */
/*===========================================================================*/

static struct {
    struct arg_lit *number;
    struct arg_str *file;
    struct arg_lit *help;
    struct arg_end *end;
} s_cat_args;

static int cmd_cat(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_cat_args);
    
    if (s_cat_args.help->count > 0) {
        ts_console_printf("Usage: cat [options] <file>\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  -n, --number  Show line numbers\n");
        ts_console_printf("      --help    Show this help\n");
        return 0;
    }
    
    if (nerrors != 0 || s_cat_args.file->count == 0) {
        ts_console_error("Usage: cat <file>\n");
        return 1;
    }
    
    char resolved[256];
    resolve_path(s_cat_args.file->sval[0], resolved, sizeof(resolved));
    
    FILE *f = fopen(resolved, "r");
    if (!f) {
        ts_console_error("cat: %s: %s\n", resolved, strerror(errno));
        return 1;
    }
    
    bool number = s_cat_args.number->count > 0;
    char line[512];
    int line_num = 1;
    
    while (fgets(line, sizeof(line), f) != NULL) {
        if (number) {
            ts_console_printf("%4d  %s", line_num++, line);
        } else {
            ts_console_printf("%s", line);
        }
    }
    
    fclose(f);
    return 0;
}

/*===========================================================================*/
/*                          Command: cd                                       */
/*===========================================================================*/

static struct {
    struct arg_str *path;
    struct arg_end *end;
} s_cd_args;

static int cmd_cd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_cd_args);
    
    if (nerrors != 0) {
        arg_print_errors(stderr, s_cd_args.end, "cd");
        return 1;
    }
    
    const char *path_arg = s_cd_args.path->count > 0 ? s_cd_args.path->sval[0] : "/sdcard";
    
    char resolved[256];
    resolve_path(path_arg, resolved, sizeof(resolved));
    
    // 验证目录存在
    struct stat st;
    if (stat(resolved, &st) != 0) {
        ts_console_error("cd: %s: No such directory\n", resolved);
        return 1;
    }
    
    if (!S_ISDIR(st.st_mode)) {
        ts_console_error("cd: %s: Not a directory\n", resolved);
        return 1;
    }
    
    strncpy(s_cwd, resolved, sizeof(s_cwd) - 1);
    s_cwd[sizeof(s_cwd) - 1] = '\0';
    
    return 0;
}

/*===========================================================================*/
/*                          Command: pwd                                      */
/*===========================================================================*/

static int cmd_pwd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ts_console_printf("%s\n", s_cwd);
    return 0;
}

/*===========================================================================*/
/*                          Command: mkdir                                    */
/*===========================================================================*/

static struct {
    struct arg_lit *parents;
    struct arg_str *path;
    struct arg_lit *help;
    struct arg_end *end;
} s_mkdir_args;

static int fs_mutation_error(const char *command, const char *path, esp_err_t ret)
{
    if (ret == ESP_ERR_NOT_ALLOWED) {
        ts_console_error("%s: '%s' belongs to Automation. Use Automation > Configuration & backup, or edit items in their lists.\n", command, path);
    } else if (ret == ESP_ERR_INVALID_ARG) {
        ts_console_error("%s: invalid path '%s'.\n", command, path);
    } else {
        ts_console_error("%s: could not change '%s'. Check the path and storage.\n", command, path);
    }
    return 1;
}

static int cmd_mkdir(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_mkdir_args);
    
    if (s_mkdir_args.help->count > 0) {
        ts_console_printf("Usage: mkdir [options] <path>\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  -p, --parents  Create parent directories\n");
        ts_console_printf("      --help     Show this help\n");
        return 0;
    }
    
    if (nerrors != 0 || s_mkdir_args.path->count == 0) {
        ts_console_error("Usage: mkdir <path>\n");
        return 1;
    }
    
    char resolved[256];
    resolve_path(s_mkdir_args.path->sval[0], resolved, sizeof(resolved));
    
    esp_err_t ret = s_mkdir_args.parents->count
        ? ts_storage_mkdir_p(resolved) : ts_storage_mkdir(resolved);
    if (ret != ESP_OK) return fs_mutation_error("mkdir", resolved, ret);

    return 0;
}

/*===========================================================================*/
/*                          Command: rm                                       */
/*===========================================================================*/

static struct {
    struct arg_lit *recursive;
    struct arg_lit *force;
    struct arg_str *path;
    struct arg_lit *help;
    struct arg_end *end;
} s_rm_args;

static int cmd_rm(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_rm_args);
    
    if (s_rm_args.help->count > 0) {
        ts_console_printf("Usage: rm [options] <path>\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  -r, --recursive  Remove directories recursively\n");
        ts_console_printf("  -f, --force      Ignore nonexistent files\n");
        ts_console_printf("      --help       Show this help\n");
        return 0;
    }
    
    if (nerrors != 0 || s_rm_args.path->count == 0) {
        ts_console_error("Usage: rm <path>\n");
        return 1;
    }
    
    char resolved[256];
    resolve_path(s_rm_args.path->sval[0], resolved, sizeof(resolved));
    
    bool recursive = s_rm_args.recursive->count > 0;
    bool force = s_rm_args.force->count > 0;
    
    esp_err_t ret = ts_storage_check_write_path(resolved, true);
    if (ret != ESP_OK) return fs_mutation_error("rm", resolved, ret);
    struct stat st;
    if (stat(resolved, &st) != 0) {
        if (force && errno == ENOENT) return 0;
        ts_console_error("rm: cannot remove '%s': %s\n", resolved, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            ts_console_error("rm: cannot remove '%s': Is a directory (use -r)\n", resolved);
            return 1;
        }
        ret = ts_storage_rmdir_r(resolved);
    } else {
        ret = ts_storage_delete(resolved);
    }
    if (ret != ESP_OK) return fs_mutation_error("rm", resolved, ret);

    return 0;
}

/*===========================================================================*/
/*                          Command: cp                                       */
/*===========================================================================*/

static struct {
    struct arg_str *src;
    struct arg_str *dst;
    struct arg_lit *help;
    struct arg_end *end;
} s_cp_args;

static int cmd_cp(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_cp_args);
    
    if (s_cp_args.help->count > 0) {
        ts_console_printf("Usage: cp <source> <destination>\n\n");
        ts_console_printf("Copy a file to another location.\n");
        return 0;
    }
    
    if (nerrors != 0 || s_cp_args.src->count == 0 || s_cp_args.dst->count == 0) {
        ts_console_error("Usage: cp <source> <destination>\n");
        return 1;
    }
    
    char src_resolved[256], dst_resolved[256];
    resolve_path(s_cp_args.src->sval[0], src_resolved, sizeof(src_resolved));
    resolve_path(s_cp_args.dst->sval[0], dst_resolved, sizeof(dst_resolved));
    
    // 检查目标是否是目录
    struct stat st;
    if (stat(dst_resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
        // 追加源文件名
        const char *basename = strrchr(src_resolved, '/');
        basename = basename ? basename + 1 : src_resolved;
        size_t dst_len = strlen(dst_resolved);
        size_t base_len = strlen(basename);
        if (dst_len + 1 + base_len < sizeof(dst_resolved)) {
            dst_resolved[dst_len] = '/';
            memcpy(dst_resolved + dst_len + 1, basename, base_len + 1);
        }
    }
    
    esp_err_t ret = ts_storage_copy(src_resolved, dst_resolved);
    if (ret != ESP_OK) return fs_mutation_error("cp", dst_resolved, ret);

    return 0;
}

/*===========================================================================*/
/*                          Command: mv                                       */
/*===========================================================================*/

static struct {
    struct arg_str *src;
    struct arg_str *dst;
    struct arg_lit *help;
    struct arg_end *end;
} s_mv_args;

static int cmd_mv(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_mv_args);
    
    if (s_mv_args.help->count > 0) {
        ts_console_printf("Usage: mv <source> <destination>\n\n");
        ts_console_printf("Move or rename a file.\n");
        return 0;
    }
    
    if (nerrors != 0 || s_mv_args.src->count == 0 || s_mv_args.dst->count == 0) {
        ts_console_error("Usage: mv <source> <destination>\n");
        return 1;
    }
    
    char src_resolved[256], dst_resolved[256];
    resolve_path(s_mv_args.src->sval[0], src_resolved, sizeof(src_resolved));
    resolve_path(s_mv_args.dst->sval[0], dst_resolved, sizeof(dst_resolved));
    
    // 检查目标是否是目录
    struct stat st;
    if (stat(dst_resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *basename = strrchr(src_resolved, '/');
        basename = basename ? basename + 1 : src_resolved;
        size_t dst_len = strlen(dst_resolved);
        size_t base_len = strlen(basename);
        if (dst_len + 1 + base_len < sizeof(dst_resolved)) {
            dst_resolved[dst_len] = '/';
            memcpy(dst_resolved + dst_len + 1, basename, base_len + 1);
        }
    }
    
    esp_err_t ret = ts_storage_rename(src_resolved, dst_resolved);
    if (ret != ESP_OK) return fs_mutation_error("mv", src_resolved, ret);

    return 0;
}

/*===========================================================================*/
/*                          Command: hexdump                                  */
/*===========================================================================*/

static struct {
    struct arg_int *length;
    struct arg_str *file;
    struct arg_lit *help;
    struct arg_end *end;
} s_hexdump_args;

static int cmd_hexdump(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_hexdump_args);
    
    if (s_hexdump_args.help->count > 0) {
        ts_console_printf("Usage: hexdump [options] <file>\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  -n, --length <n>  Show first n bytes\n");
        ts_console_printf("      --help        Show this help\n");
        return 0;
    }
    
    if (nerrors != 0 || s_hexdump_args.file->count == 0) {
        ts_console_error("Usage: hexdump <file>\n");
        return 1;
    }
    
    char resolved[256];
    resolve_path(s_hexdump_args.file->sval[0], resolved, sizeof(resolved));
    
    FILE *f = fopen(resolved, "rb");
    if (!f) {
        ts_console_error("hexdump: %s: %s\n", resolved, strerror(errno));
        return 1;
    }
    
    int max_len = s_hexdump_args.length->count > 0 ? s_hexdump_args.length->ival[0] : 256;
    unsigned char buffer[16];
    size_t offset = 0;
    size_t bytes;
    
    while ((bytes = fread(buffer, 1, 16, f)) > 0 && (int)offset < max_len) {
        ts_console_printf("%08zx  ", offset);
        
        // Hex
        for (size_t i = 0; i < 16; i++) {
            if (i < bytes) {
                ts_console_printf("%02x ", buffer[i]);
            } else {
                ts_console_printf("   ");
            }
            if (i == 7) ts_console_printf(" ");
        }
        
        ts_console_printf(" |");
        
        // ASCII
        for (size_t i = 0; i < bytes; i++) {
            char c = buffer[i];
            ts_console_printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        
        ts_console_printf("|\n");
        offset += bytes;
    }
    
    fclose(f);
    return 0;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_fs_register(void)
{
    esp_err_t ret;
    int success = 0, failed = 0;
    
    // ls
    s_ls_args.all      = arg_lit0("a", "all", "Show all");
    s_ls_args.long_fmt = arg_lit0("l", "long", "Long format");
    s_ls_args.human    = arg_lit0("h", "human", "Human sizes");
    s_ls_args.path     = arg_str0(NULL, NULL, "<path>", "Directory");
    s_ls_args.help     = arg_lit0(NULL, "help", "Help");
    s_ls_args.end      = arg_end(5);
    
    const ts_console_cmd_t cmd_ls_def = {
        .command = "ls",
        .help = "List directory contents",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_ls,
        .argtable = &s_ls_args
    };
    ret = ts_console_register_cmd(&cmd_ls_def);
    if (ret == ESP_OK) success++; else failed++;
    
    // cat
    s_cat_args.number = arg_lit0("n", "number", "Line numbers");
    s_cat_args.file   = arg_str1(NULL, NULL, "<file>", "File");
    s_cat_args.help   = arg_lit0(NULL, "help", "Help");
    s_cat_args.end    = arg_end(3);
    
    const ts_console_cmd_t cmd_cat_def = {
        .command = "cat",
        .help = "Display file contents",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_cat,
        .argtable = &s_cat_args
    };
    ret = ts_console_register_cmd(&cmd_cat_def);
    if (ret == ESP_OK) success++; else failed++;
    
    // cd
    s_cd_args.path = arg_str0(NULL, NULL, "<path>", "Directory");
    s_cd_args.end  = arg_end(2);
    
    const ts_console_cmd_t cmd_cd_def = {
        .command = "cd",
        .help = "Change directory",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_cd,
        .argtable = &s_cd_args
    };
    ret = ts_console_register_cmd(&cmd_cd_def);
    if (ret == ESP_OK) success++; else failed++;
    
    // pwd
    const ts_console_cmd_t cmd_pwd_def = {
        .command = "pwd",
        .help = "Print working directory",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_pwd,
        .argtable = NULL
    };
    ret = ts_console_register_cmd(&cmd_pwd_def);
    if (ret == ESP_OK) success++; else failed++;
    
    // mkdir
    s_mkdir_args.parents = arg_lit0("p", "parents", "Create parents");
    s_mkdir_args.path    = arg_str1(NULL, NULL, "<path>", "Directory");
    s_mkdir_args.help    = arg_lit0(NULL, "help", "Help");
    s_mkdir_args.end     = arg_end(3);
    
    const ts_console_cmd_t cmd_mkdir_def = {
        .command = "mkdir",
        .help = "Create directory",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_mkdir,
        .argtable = &s_mkdir_args
    };
    ret = ts_console_register_cmd(&cmd_mkdir_def);
    if (ret == ESP_OK) success++; else failed++;
    
    // rm
    s_rm_args.recursive = arg_lit0("r", "recursive", "Recursive");
    s_rm_args.force     = arg_lit0("f", "force", "Force");
    s_rm_args.path      = arg_str1(NULL, NULL, "<path>", "Path");
    s_rm_args.help      = arg_lit0(NULL, "help", "Help");
    s_rm_args.end       = arg_end(4);
    
    const ts_console_cmd_t cmd_rm_def = {
        .command = "rm",
        .help = "Remove files or directories",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_rm,
        .argtable = &s_rm_args
    };
    ret = ts_console_register_cmd(&cmd_rm_def);
    if (ret == ESP_OK) success++; else failed++;
    
    // cp
    s_cp_args.src  = arg_str1(NULL, NULL, "<src>", "Source");
    s_cp_args.dst  = arg_str1(NULL, NULL, "<dst>", "Destination");
    s_cp_args.help = arg_lit0(NULL, "help", "Help");
    s_cp_args.end  = arg_end(3);
    
    const ts_console_cmd_t cmd_cp_def = {
        .command = "cp",
        .help = "Copy files",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_cp,
        .argtable = &s_cp_args
    };
    ret = ts_console_register_cmd(&cmd_cp_def);
    if (ret == ESP_OK) success++; else failed++;
    
    // mv
    s_mv_args.src  = arg_str1(NULL, NULL, "<src>", "Source");
    s_mv_args.dst  = arg_str1(NULL, NULL, "<dst>", "Destination");
    s_mv_args.help = arg_lit0(NULL, "help", "Help");
    s_mv_args.end  = arg_end(3);
    
    const ts_console_cmd_t cmd_mv_def = {
        .command = "mv",
        .help = "Move or rename files",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_mv,
        .argtable = &s_mv_args
    };
    ret = ts_console_register_cmd(&cmd_mv_def);
    if (ret == ESP_OK) success++; else failed++;
    
    // hexdump
    s_hexdump_args.length = arg_int0("n", "length", "<n>", "Bytes to show");
    s_hexdump_args.file   = arg_str1(NULL, NULL, "<file>", "File");
    s_hexdump_args.help   = arg_lit0(NULL, "help", "Help");
    s_hexdump_args.end    = arg_end(3);
    
    const ts_console_cmd_t cmd_hexdump_def = {
        .command = "hexdump",
        .help = "Hex dump file contents",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_hexdump,
        .argtable = &s_hexdump_args
    };
    ret = ts_console_register_cmd(&cmd_hexdump_def);
    if (ret == ESP_OK) success++; else failed++;
    
    TS_LOGI(TAG, "File system commands registered: %d succeeded, %d failed", success, failed);
    return (failed == 0) ? ESP_OK : ESP_FAIL;
}
