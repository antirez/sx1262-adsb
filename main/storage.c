/* Two rotating logs give a quiet afternoon of noise its own space. Each line
 * stands alone. A torn last write cannot prevent the next boot reading all
 * the earlier packets. The same JSON is available over the USB console. */
#include "storage.h"
#include "cJSON.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "/log"
static const char *packets=ROOT"/packets", *noise=ROOT"/power";
static const char *erase_marker=ROOT"/erase-packets";
static bool erase_pending;

static esp_err_t mount_log(void) {
    esp_vfs_spiffs_conf_t conf={.base_path=ROOT,.partition_label="captures",.max_files=3};
    esp_err_t err=esp_vfs_spiffs_register(&conf);
    if (err==ESP_OK) return err;
    /* Only format a blank partition. A failed mount of an existing log
     * deserves an error on the screen, not the loss of a field outing. */
    const esp_partition_t *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,"captures");
    if (!part) return ESP_ERR_NOT_FOUND;
    uint8_t page[256];
    for (size_t off=0; off<part->size; off+=sizeof(page)) {
        if (esp_partition_read(part,off,page,sizeof(page))!=ESP_OK) return err;
        for (unsigned i=0; i<sizeof(page); i++) if (page[i]!=0xff) return err;
    }
    conf.format_if_mount_failed=true;
    return esp_vfs_spiffs_register(&conf);
}

esp_err_t storage_init(void) {
    esp_err_t err=mount_log();
    if (err!=ESP_OK) return err;
    struct stat st;
    erase_pending=stat(erase_marker,&st)==0;
    return erase_pending || errno==ENOENT ? ESP_OK : ESP_FAIL;
}

static bool remove_if_present(const char *path) {
    return unlink(path)==0 || errno==ENOENT;
}

/* The two packet files make one history. Leave a marker until both are
 * gone, so a power cut halfway through cannot bring the old cards back or
 * let the next packet be appended to a file that's about to be erased. */
static bool finish_erase(void) {
    if (!erase_pending) return true;
    if (!remove_if_present(ROOT"/packets.prev") || !remove_if_present(packets) ||
        !remove_if_present(erase_marker)) return false;
    erase_pending=false;
    return true;
}

EraseResult storage_clear_packets(void) {
    if (!erase_pending) {
        FILE *f=fopen(erase_marker,"w");
        if (!f) return ERASE_FAILED;
        erase_pending=true;
        if (fclose(f)!=0) return ERASE_PENDING;
    }
    return finish_erase() ? ERASE_DONE : ERASE_PENDING;
}

static bool append(const char *path, long limit, const char *line) {
    struct stat st;
    if (stat(path,&st)==0 && st.st_size>=limit) {
        char previous[48];
        snprintf(previous,sizeof(previous),"%s.prev",path);
        if (unlink(previous)<0 && errno!=ENOENT) return false;
        if (rename(path,previous)<0) return false;
    }
    FILE *f=fopen(path,"a");
    if (!f) return false;
    int wrote=fprintf(f,"\n%s\n",line);
    int closed=fclose(f);
    return wrote>0 && closed==0;
}

bool storage_save(const char *line, bool packet) {
    if (packet && !finish_erase()) return false;
    return append(packet?packets:noise,packet?98304:131072,line);
}

static bool unhex(const char *s, uint8_t *bytes, size_t n) {
    if (!s || strlen(s)!=2*n) return false;
    for (size_t i=0; i<n; i++) {
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return false;
        char pair[]={s[0],s[1],0};
        bytes[i]=(uint8_t)strtoul(pair,NULL,16);
        s+=2;
    }
    return true;
}

/* Accept the field app's older packet records as well. This makes it
 * possible to bring a saved history across a firmware replacement. */
static bool parse_card(const char *line, Card *out) {
    cJSON *row=cJSON_Parse(line);
    if (!row) return false;
    Card c={0};
    cJSON *id=cJSON_GetObjectItem(row,"boot"), *rf=cJSON_GetObjectItem(row,"signal_hz");
    uint8_t idbytes[4];
    bool valid=cJSON_IsNumber(rf) && rf->valuedouble==1090000000 &&
        cJSON_IsString(id) && unhex(id->valuestring,idbytes,sizeof(idbytes));
    if (valid) {
        c.boot=strtoul(id->valuestring,NULL,16);
        const char *keys[]={"df17","df11","test_word"};
        for (int i=0; i<3; i++) {
            cJSON *word=cJSON_GetObjectItem(row,keys[i]);
            int length=i==1?7:14;
            if (cJSON_IsString(word) && unhex(word->valuestring,c.word,length)) {
                c.length=length;
                break;
            }
        }
        cJSON *time=cJSON_GetArrayItem(cJSON_GetObjectItem(row,"capture_ms"),1);
        if (!time && c.length==7) {
            cJSON *a=cJSON_GetObjectItem(row,"captures");
            time=cJSON_GetArrayItem(cJSON_GetArrayItem(a,cJSON_GetArraySize(a)-1),0);
        }
        if (!time) time=cJSON_GetObjectItem(row,"uptime_ms");
        cJSON *count=cJSON_GetObjectItem(row,"test_count");
        if (cJSON_IsNumber(count) && count->valueint>=0) c.test_count=count->valueint;
        valid=cJSON_IsNumber(time) && time->valuedouble>=0 && time->valuedouble<1e15 && card_valid(&c);
        if (valid) { c.ms=time->valuedouble; *out=c; }
    }
    cJSON_Delete(row);
    return valid;
}

bool storage_import(const char *json) {
    Card c;
    if (!parse_card(json,&c) || !storage_save(json,true)) return false;
    history_add(c);
    return true;
}

static void read_lines(const char *path, bool restore) {
    FILE *f=fopen(path,"r");
    if (!f) return;
    char line[1536];
    while (fgets(line,sizeof(line),f)) {
        if (line[0]!='{') continue;
        if (restore) {
            Card c;
            if (parse_card(line,&c)) history_add(c);
        } else printf("SXLOG %s",line);
    }
    fclose(f);
}

void storage_restore(void) {
    if (!finish_erase()) return;
    read_lines(ROOT"/packets.prev",true);
    read_lines(packets,true);
}

void storage_dump(void) {
    read_lines(ROOT"/power.prev",false);
    read_lines(noise,false);
    if (finish_erase()) {
        read_lines(ROOT"/packets.prev",false);
        read_lines(packets,false);
    }
    puts("SXEND");
}
