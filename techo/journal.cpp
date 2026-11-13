/* The T-Echo has 28 KiB of internal LittleFS. Store the 256 RSSI bytes as
 * bytes, and turn them into printable JSON only when someone asks over USB.
 * Only the field task uses these files after startup. */
#include <InternalFileSystem.h>
#include "board.h"

struct Record {
    uint32_t format, boot, tests;
    Capture capture;
    uint8_t word[14], length, evidence, count, supports;
    int32_t anchor, score, rank;
    uint32_t blocks[5];
    char method[28];
};
static const uint32_t format = 0x31425341; /* "ASB1" on this little-endian MCU */
struct Log { const char *file, *previous; unsigned limit; };
static const Log packets={"/adsb-packets","/adsb-packets.1",18};
static const Log power={"/adsb-power","/adsb-power.1",4};
static const char *marker="/adsb-erase", *dial="/adsb-hz";
static lfs_t *fs;

static bool remove_file(const char *path) {
    int rc=lfs_remove(fs,path);
    return rc==0 || rc==LFS_ERR_NOENT;
}

static bool write_file(const char *path, const void *data, unsigned size, int mode) {
    lfs_file_t f;
    if (lfs_file_open(fs,&f,path,LFS_O_WRONLY|LFS_O_CREAT|mode)<0) return false;
    bool ok=lfs_file_write(fs,&f,data,size)==(int)size;
    return lfs_file_close(fs,&f)==0 && ok;
}

static bool finish_clear(void) {
    return remove_file(packets.previous) && remove_file(packets.file) && remove_file(marker);
}
bool journal_clear(void) {
    uint8_t yes=1;
    return write_file(marker,&yes,sizeof(yes),LFS_O_TRUNC) && finish_clear();
}
bool journal_dial(uint32_t hz) {
    return write_file(dial,&hz,sizeof(hz),LFS_O_TRUNC);
}

static bool read_log(const char *path, bool dump) {
    lfs_file_t f;
    int rc=lfs_file_open(fs,&f,path,LFS_O_RDONLY);
    if (rc==LFS_ERR_NOENT) return true;
    if (rc<0) return false;
    Record rec;
    int bytes;
    while ((bytes=lfs_file_read(fs,&f,&rec,sizeof(rec)))==(int)sizeof(rec)) {
        if (rec.format!=format || (rec.length!=0 && rec.length!=7 && rec.length!=14) ||
            rec.supports>5 || !memchr(rec.method,0,sizeof(rec.method))) continue;
        if (dump) {
            Result r={};
            r.capture=rec.capture; r.length=rec.length; r.method=rec.method;
            r.anchor=rec.anchor; r.rank=rec.rank; r.score=rec.score;
            r.evidence_index=rec.evidence; r.evidence_count=rec.count; r.support_count=rec.supports;
            memcpy(r.word,rec.word,sizeof(r.word)); memcpy(r.support,rec.blocks,sizeof(r.support));
            char line[1450];
            observation(line,sizeof(line),&r,rec.boot,r.capture.last_ms,rec.tests,0);
            board_print("SXLOG "); board_print(line); board_print("\n");
        } else if (rec.length && !rec.evidence) {
            Card c={};
            c.boot=rec.boot; c.ms=rec.capture.last_ms; c.length=rec.length; c.test_count=rec.tests;
            memcpy(c.word,rec.word,rec.length);
            if (card_valid(&c)) history_add(c);
        }
    }
    return lfs_file_close(fs,&f)==0 && bytes==0;
}

bool journal_begin(uint32_t *hz) {
    /* The stock begin() formats after a mount error. Only offer it a blank
     * region: an unreadable filesystem may contain somebody's field logs. */
    if (!InternalFS.Adafruit_LittleFS::begin()) {
        const volatile uint32_t *flash=(volatile uint32_t *)0xed000;
        for (unsigned i=0;i<7*4096/sizeof(*flash);i++) if (flash[i]!=UINT32_MAX) return false;
        if (!InternalFS.begin()) return false;
    }
    fs=InternalFS._getFS();
    struct lfs_info info;
    int rc=lfs_stat(fs,marker,&info);
    if (rc!=LFS_ERR_NOENT && (rc<0 || !finish_clear())) return false;
    lfs_file_t file;
    if (!lfs_file_open(fs,&file,dial,LFS_O_RDONLY)) {
        uint32_t value;
        if (lfs_file_read(fs,&file,&value,sizeof(value))==sizeof(value) &&
            value>=MIN_HZ && value<=MAX_HZ) *hz=value;
        lfs_file_close(fs,&file);
    }
    return read_log(packets.previous,false) && read_log(packets.file,false);
}

bool journal_save(const Result *r, uint32_t boot, unsigned tests) {
    const Log &log=r->length?packets:power;
    struct lfs_info info;
    int rc=lfs_stat(fs,log.file,&info);
    if (rc!=LFS_ERR_NOENT && rc<0) return false;
    if (!rc && info.size>=log.limit*sizeof(Record)) {
        if (lfs_rename(fs,log.file,log.previous)<0) return false;
    }
    Record rec={};
    rec.format=format; rec.boot=boot; rec.tests=tests; rec.capture=r->capture;
    rec.length=r->length; rec.anchor=r->anchor; rec.score=r->score; rec.rank=r->rank;
    rec.evidence=r->evidence_index; rec.count=r->evidence_count; rec.supports=r->support_count;
    memcpy(rec.word,r->word,sizeof(rec.word)); memcpy(rec.blocks,r->support,sizeof(rec.blocks));
    snprintf(rec.method,sizeof(rec.method),"%s",r->method?r->method:"");
    return write_file(log.file,&rec,sizeof(rec),LFS_O_APPEND);
}

bool journal_dump(void) {
    const char *files[]={packets.previous,packets.file,power.previous,power.file};
    for (const char *file : files) {
        if (!read_log(file,true)) return false;
    }
    return true;
}
