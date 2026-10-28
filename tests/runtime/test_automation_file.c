#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE *test_open(const char *, const char *);
static int test_stat(const char *, struct stat *);
static int test_mkdir(const char *, mode_t);
static int test_unlink(const char *);
static int test_rename(const char *, const char *);
#define fopen(path, mode) test_open(path, mode)
#define stat(path, out) test_stat(path, out)
#define mkdir(path, mode) test_mkdir(path, mode)
#define unlink(path) test_unlink(path)
#define rename(from, to) test_rename(from, to)
#include "../../components/ts_automation/src/ts_automation_file.c"
#undef fopen
#undef stat
#undef mkdir
#undef unlink
#undef rename

static char directory[]="/tmp/tianshan-auto-file-XXXXXX";
static bool fail_install, report_after_install, fail_cleanup;
static bool fail_sd_read;
static const char *old_json="{\"format_version\":1,\"rules\":[{\"id\":\"old\"}]}";
static const char *new_json="{\"format_version\":1,\"rules\":[{\"id\":\"new\"}]}";
static void mapped(const char *path,char *out,size_t capacity)
{
    int n=snprintf(out,capacity,"%s%s",directory,path);
    assert(n>0 && (size_t)n<capacity);
}
static FILE *test_open(const char *path,const char *mode)
{
    if(fail_sd_read && mode[0]=='r' && !strcmp(path,s_paths[TS_AUTO_FILE_SD].current)){errno=EIO;return NULL;}
    char actual[512];mapped(path,actual,sizeof(actual));return fopen(actual,mode);
}
static int test_stat(const char *path,struct stat *out)
{
    char actual[512];mapped(path,actual,sizeof(actual));return stat(actual,out);
}
static int test_mkdir(const char *path,mode_t mode)
{
    char actual[512];mapped(path,actual,sizeof(actual));return mkdir(actual,mode);
}
static int test_unlink(const char *path)
{
    if (fail_cleanup && strstr(path,"state.previous")) {errno=EIO;return -1;}
    char actual[512];mapped(path,actual,sizeof(actual));return unlink(actual);
}
static int test_rename(const char *from,const char *to)
{
    bool install=strstr(from,"state.pending") && strstr(to,"state.json");
    if (install && fail_install) {errno=EIO;return -1;}
    char source[512],target[512];mapped(from,source,sizeof(source));mapped(to,target,sizeof(target));
    struct stat st;
    /* SPIFFS rename rejects an existing target, unlike host POSIX rename. */
    if(stat(target,&st)==0) {errno=EEXIST;return -1;}
    int ret=rename(source,target);
    if(ret==0 && install && report_after_install) {errno=EIO;return -1;}
    return ret;
}
static void loaded(ts_auto_file_location_t location,const char *expected)
{
    char *json=NULL;
    assert(ts_auto_file_load(location,&json)==ESP_OK);
    assert(json && !strcmp(json,expected));free(json);
}
static void interrupted(ts_auto_file_location_t location)
{
    const paths_t *p=&s_paths[location];
    assert(write_pending(p->pending,new_json)==ESP_OK);
    assert(test_rename(p->current,p->previous)==0);
    loaded(location,old_json);
}
int main(void)
{
    assert(mkdtemp(directory));
    assert(test_mkdir("/autocfg",0775)==0);
    assert(test_mkdir("/sdcard",0775)==0);
    for(int location=TS_AUTO_FILE_SD;location<=TS_AUTO_FILE_INTERNAL;++location) {
        char *json=NULL;
        assert(ts_auto_file_load(location,&json)==ESP_ERR_NOT_FOUND && !json);
        assert(ts_auto_file_save(location,old_json)==ESP_OK);
        loaded(location,old_json);
        fail_install=true;
        assert(ts_auto_file_save(location,new_json)==ESP_FAIL);
        fail_install=false;loaded(location,old_json);
        interrupted(location);
        report_after_install=true;
        assert(ts_auto_file_save(location,new_json)==ESP_OK);
        report_after_install=false;loaded(location,new_json);
        fail_cleanup=true;
        assert(ts_auto_file_save(location,old_json)==ESP_OK);
        loaded(location,old_json); /* Committed despite cleanup failure. */
        assert(ts_auto_file_save(location,new_json)!=ESP_OK);
        fail_cleanup=false;loaded(location,old_json);
        assert(ts_auto_file_save(location,new_json)==ESP_OK);
        loaded(location,new_json);
        /* A confirmed paused deletion preserves unrelated semantic errors.
         * File transport validates JSON syntax; the owner validates definitions. */
        const char *paused="{\"format_version\":0,\"enabled\":false,\"rules\":[{\"id\":\"needs-correction\",\"actions\":{\"original\":\"keep\"}}]}";
        assert(ts_auto_file_save(location,paused)==ESP_OK);
        loaded(location,paused);
        assert(test_unlink(s_paths[location].current)==0);
    }
    assert(test_unlink("/autocfg/state.pending")==-1 && errno==ENOENT);
    char path[512];
    const char *dirs[]={"/autocfg","/sdcard/config/automation","/sdcard/config","/sdcard"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);++i) {
        mapped(dirs[i],path,sizeof(path));assert(rmdir(path)==0);
    }
    assert(rmdir(directory)==0);
    puts("PASS configuration files: SPIFFS rename, interrupted write, observed commit and cleanup");
    return 0;
}
