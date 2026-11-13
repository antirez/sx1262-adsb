/* The Nordic has one core. Give radio polling first choice of CPU and keep
 * e-paper refreshes and trellis searches in their own preemptible tasks. */
#include "board.h"
#include "radio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static QueueHandle_t captures, results, pages;
static atomic_uint requested=DEFAULT_HZ, applied, failed, bursts, polls, retries, dropped;
static atomic_int radio_error, decoder_ok;
static atomic_bool ready, watching, painted;
static Scope scope;
static uint32_t boot, saved=DEFAULT_HZ;
static bool mounted, trace_ok=true;
static Menu menu;
static const char *notice;
static unsigned test_count, cleaning;
static bool redraw;
static int64_t cleared=-1;
typedef struct { Screen rows; CardView card; bool graph; ScopeView scope; unsigned cleaning; } Page;

static int arm(Capture *c) {
    uint32_t hz=atomic_load(&requested);
    if (hz!=c->dial_hz) {
        int err=radio_tune(hz);
        if (err) {
            atomic_store(&failed,hz);
            if (!c->dial_hz) return err;
            atomic_compare_exchange_strong(&requested,&hz,c->dial_hz);
            if ((err=radio_tune(c->dial_hz))) return err;
        } else c->dial_hz=hz;
    }
    int err=radio_calibrate(&c->floor,&c->threshold);
    if (!err) err=radio_arm(c->threshold);
    if (!err) atomic_store(&applied,c->dial_hz);
    return err;
}

static int receive(void) {
    Capture c={.dial_hz=DEFAULT_HZ};
    int err=radio_start(false);
    if (err || (err=arm(&c))) return err;
    atomic_store(&ready,true);
    int64_t last=uptime_ms(), calibrated=last;
    for (;;) {
        vTaskDelay(1);
        uint8_t state;
        if ((err=radio_state(&state))) return err;
        atomic_fetch_add(&polls,1);
        int64_t now=uptime_ms();
        if (state==SCAN_COMPLETE) {
            c.first_ms=last; c.last_ms=now;
            if ((err=radio_read(c.raw))) return err;
            c.block=atomic_fetch_add(&bursts,1)+1;
            if (!xQueueSend(captures,&c,0)) atomic_fetch_add(&dropped,1);
        } else if (state!=SCAN_ARMED) return 0x103;
        if (atomic_load(&watching)) {
            uint8_t level;
            if ((err=radio_rssi(&level))) return err;
            taskENTER_CRITICAL();
            scope_sample(&scope,c.dial_hz,now,level,state==SCAN_COMPLETE?&c:NULL);
            taskEXIT_CRITICAL();
        }
        if (c.dial_hz!=atomic_load(&requested) || now-calibrated>=60000) {
            if (state==SCAN_ARMED && (err=radio_finish())) return err;
            if ((err=arm(&c))) return err;
            calibrated=uptime_ms();
        } else if (state==SCAN_COMPLETE && (err=radio_arm(c.threshold))) return err;
        last=uptime_ms();
    }
}

static void radio_task(void *arg) {
    (void)arg;
    for (;;) {
        int err=radio_bus_init();
        if (!err) err=radio_self_test();
        if (!err) err=receive();
        atomic_store(&ready,false); atomic_store(&radio_error,err);
        atomic_fetch_add(&retries,1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void publish(const Result *r) {
    static int64_t last_power=-30000;
    if (!r->length && r->capture.last_ms-last_power<30000) return;
    if (!r->length) last_power=r->capture.last_ms;
    xQueueSend(results,r,r->length?portMAX_DELAY:0);
}
static void decoder_task(void *arg) {
    (void)arg;
    bool ok=decoder_init(publish) && decoder_self_test();
    atomic_store(&decoder_ok,ok?1:-1);
    if (!ok) { vTaskDelete(NULL); return; }
    Capture c;
    for (;;) {
        if (xQueueReceive(captures,&c,0)) decoder_capture(&c);
        else if (!decoder_idle(uptime_ms()) && xQueueReceive(captures,&c,pdMS_TO_TICKS(10)))
            decoder_capture(&c);
    }
}

static void display_task(void *arg) {
    (void)arg;
    Page page;
    unsigned cleaned=0;
    TickType_t wait=portMAX_DELAY;
    for (;;) {
        /* On timeout the old page is still here. A failed BUSY handshake
         * invalidates the panel shadow and the next attempt starts clean. */
        xQueueReceive(pages,&page,wait);
        bool ok=board_paint(page.rows,page.card,page.graph?&page.scope:NULL,page.cleaning!=cleaned);
        if (ok) cleaned=page.cleaning;
        atomic_store(&painted,ok);
        wait=ok?portMAX_DELAY:pdMS_TO_TICKS(2500);
    }
}

static void key(Key k) {
    if (k==KEY_NONE) return;
    redraw=true;
    if (!menu.open) {
        if (k==KEY_TAP) history_older();
        else if (k==KEY_HOLD) history_live();
        else if (k==KEY_DOUBLE) {
            menu_open(&menu,atomic_load(&requested)); notice=NULL;
            taskENTER_CRITICAL(); memset(&scope,0,sizeof(scope)); taskEXIT_CRITICAL();
            atomic_store(&watching,true);
        }
        return;
    }
    if (k==KEY_REPEAT && menu.action!=TUNE_UP && menu.action!=TUNE_DOWN) return;
    notice=NULL;
    if (k==KEY_TAP) { menu_next(&menu); return; }
    if (k==KEY_DOUBLE) {
        if (!atomic_load(&ready) || atomic_load(&applied)!=menu.hz) { notice="Tuning..."; return; }
        if (saved!=menu.hz && (!mounted || !journal_dial(menu.hz))) { notice="Save failed"; return; }
        saved=menu.hz;
    } else if (menu.action==CLEAR_HISTORY) {
        int64_t cutoff=uptime_ms();
        if (!mounted || !journal_clear()) { notice="Erase failed"; return; }
        history_clear(); cleared=cutoff; test_count=0; notice="History cleared"; return;
    } else if (menu.action==DISCARD) {
        atomic_store(&requested,saved);
    } else {
        menu_adjust(&menu); atomic_store(&failed,0); atomic_store(&requested,menu.hz); return;
    }
    menu.open=false; atomic_store(&watching,false);
}

static void console(const Page *last_page) {
    static char line[48];
    static unsigned used;
    for (int ch;(ch=board_getchar())>=0;) {
        if (ch!='\r' && ch!='\n') { if (used<sizeof(line)-1) line[used++]=(char)ch; continue; }
        if (!used) continue;
        line[used]=0; used=0;
        if (!strcmp(line,"status")) {
            char msg[320];
            snprintf(msg,sizeof(msg),"SXSTATUS %08"PRIx32" ready=%d selftest=%d bursts=%u polls=%u queue_full=%u retries=%u error=%d log=%d oled=%d dial=%u saved=%u menu=%d heap=%d\n",
                boot,(int)atomic_load(&ready),atomic_load(&decoder_ok),atomic_load(&bursts),
                atomic_load(&polls),atomic_load(&dropped),atomic_load(&retries),atomic_load(&radio_error),
                mounted && trace_ok,(int)atomic_load(&painted),atomic_load(&applied),(unsigned)saved,menu.open,board_free_heap());
            board_print(msg);
        } else if (!strcmp(line,"dump")) { board_print(mounted && journal_dump() ? "SXEND\n" : "SXERROR reading journal\n"); }
        else if (!strcmp(line,"paper")) board_paper_status();
        else if (!strcmp(line,"paper clean")) { cleaning++; redraw=true; }
        else if (!strcmp(line,"bootloader")) board_bootloader();
        else if (!strcmp(line,"screen")) {
            for (int i=0;i<LINES;i++) {
                board_print("SXROW "); board_print(last_page->rows[i]); board_print("\n");
            }
            board_print("SXEND\n");
        } else if (!strncmp(line,"tune ",5)) {
            char *end;
            unsigned long hz=strtoul(line+5,&end,10);
            if (!menu.open && !*end && hz>=MIN_HZ && hz<=MAX_HZ)
                atomic_store(&requested,(uint32_t)hz); /* USB bench setting lasts until reset. */
        } else if (!strcmp(line,"key double")) key(KEY_DOUBLE);
        else if (!strcmp(line,"key tap")) key(KEY_TAP);
        else if (!strcmp(line,"key hold")) key(KEY_HOLD);
    }
}

static void field_task(void *arg) {
    (void)arg;
    static Page page, previous;
    static Scope snapshot;
    Button button={0};
    bool allow_clean=false;
    int64_t drawn=-1000;
    for (;;) {
        Result r;
        if (xQueueReceive(results,&r,pdMS_TO_TICKS(10)) && r.capture.last_ms>cleared) {
            if (r.length && !r.evidence_index) {
                Card c={.boot=boot,.ms=r.capture.last_ms,.length=r.length};
                memcpy(c.word,r.word,r.length);
                if ((r.word[0]>>3)==2) c.test_count=++test_count;
                history_add(c); redraw=true;
            }
            if (mounted) trace_ok=journal_save(&r,boot,test_count);
        }
        int64_t now=uptime_ms();
        bool was_down=button.down;
        Key event=button_update(&button,board_button(),now);
        if (!was_down && button.down) allow_clean=!menu.open;
        key(event);
        /* Holding still returns to LIVE after 0.8 s. Keep holding for three
         * seconds to wash out a ghost image without restarting the receiver. */
        if (allow_clean && button.down && button.held && now-button.pressed>=3000) {
            cleaning++; redraw=true; allow_clean=false;
        }
        console(&previous);
        if (!redraw && now-drawn<1000) continue;
        redraw=false;
        drawn=now;
        memset(&page,0,sizeof(page));
        page.cleaning=cleaning;
        if (menu.open) {
            if (atomic_load(&failed)==menu.hz) {
                menu.hz=atomic_load(&requested); notice="PLL did not lock";
            }
            taskENTER_CRITICAL(); snapshot=scope; taskEXIT_CRITICAL();
            scope_view(&snapshot,menu.hz,now,&page.scope);
            menu_screen(&menu,&page.scope,menu.hz!=atomic_load(&applied)?"Tuning...":notice,page.rows);
            page.graph=true;
        } else {
            page.card=history_screen(page.rows,boot,now);
            const char *problem=NULL;
            if (!atomic_load(&ready)) problem=atomic_load(&retries)?"Radio retry":"Starting radio";
            if (!mounted || !trace_ok) problem="Log unavailable";
            if (atomic_load(&decoder_ok)<0) problem="Decoder failed";
            if (problem) strcpy(page.rows[7],problem);
        }
        if (memcmp(&previous,&page,sizeof(page))) {
            xQueueOverwrite(pages,&page); previous=page;
        }
    }
}

void techo_start(void) {
    boot=board_random(); history_set_boot(boot);
    mounted=journal_begin(&saved); atomic_store(&requested,saved);
    captures=xQueueCreate(16,sizeof(Capture));
    results=xQueueCreate(6,sizeof(Result));
    pages=xQueueCreate(1,sizeof(Page));
    if (!captures || !results || !pages ||
        xTaskCreate(display_task,"paper",1100,NULL,1,NULL)!=pdPASS ||
        xTaskCreate(field_task,"field",2000,NULL,2,NULL)!=pdPASS ||
        xTaskCreate(decoder_task,"decode",4500,NULL,1,NULL)!=pdPASS ||
        xTaskCreate(radio_task,"radio",1100,NULL,3,NULL)!=pdPASS) {
        for (;;) { board_print("SXERROR task memory\n"); vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
}
