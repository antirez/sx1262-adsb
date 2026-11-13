/* LilyGO T3-S3 / SX1262 passive Mode S receiver.
 *
 * Core 0 owns the radio. It polls for a finished capture, copies it, and
 * rearms. Core 1 runs the decoder. Neither side shares a mutable waveform:
 * FreeRTOS copies each Capture into the queue. The third, low-priority task
 * owns the screen, button, flash and USB. It can sleep through every bit. */
#include "receiver.h"
#include "radio.h"
#include "display.h"
#include "storage.h"
#include "tuning.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { QUEUE_DEPTH=96, UNCHECKED=8, BOOT_BUTTON=0 };
/* Advances only when both the fresh queue and the deferred passes are idle. */
static atomic_uint idle_polls;
static QueueHandle_t captures, results;
static uint32_t boot_id;
static atomic_uint bursts, polls, restarts, full_queue, high_water, max_rearm_us, deferred, filtered;
static atomic_bool ready;
static int64_t now_ms(void) { return esp_timer_get_time()/1000; }

static void wake_radio(void *task) { xTaskNotifyGive((TaskHandle_t)task); }

/* Frequency changes cross the cores as requests. Only the radio task touches
 * the synthesizer. "Applied" means calibration and rearming both succeeded. */
static atomic_uint requested_hz=DEFAULT_HZ, applied_hz, failed_hz;
static atomic_bool watching;
static Menu menu;
static uint32_t saved_hz=DEFAULT_HZ;
static bool settings_ok;
static Scope scope;
static ScopeView view;
static portMUX_TYPE scope_lock=portMUX_INITIALIZER_UNLOCKED;

static void load_dial(void) {
    nvs_handle_t handle;
    settings_ok=nvs_flash_init()==ESP_OK;
    if (!settings_ok || nvs_open("adsb",NVS_READONLY,&handle)!=ESP_OK) return;
    uint32_t hz;
    if (nvs_get_u32(handle,"dial",&hz)==ESP_OK && hz>=MIN_HZ && hz<=MAX_HZ) saved_hz=hz;
    nvs_close(handle);
    atomic_store(&requested_hz,saved_hz);
}

static bool save_dial(uint32_t hz) {
    if (saved_hz==hz) return true;
    nvs_handle_t h;
    if (!settings_ok || nvs_open("adsb",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t err=nvs_set_u32(h,"dial",hz);
    if (err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    if (err==ESP_OK) saved_hz=hz;
    return err==ESP_OK;
}

static esp_err_t arm_channel(Capture *c) {
    uint32_t requested=atomic_load(&requested_hz);
    if (requested!=c->dial_hz) {
        esp_err_t err=radio_tune(requested);
        if (err==ESP_OK) c->dial_hz=requested;
        else {
            atomic_store(&failed_hz,requested);
            /* A rejected dial must not strand the receiver out of lock. */
            unsigned failed=requested;
            atomic_compare_exchange_strong(&requested_hz,&failed,c->dial_hz);
            err=radio_tune(c->dial_hz);
            if (err!=ESP_OK) return err;
        }
    }
    esp_err_t err=radio_calibrate(&c->floor,&c->threshold);
    if (err==ESP_OK) err=radio_arm(c->threshold);
    if (err==ESP_OK) atomic_store(&applied_hz,c->dial_hz);
    return err;
}

static esp_err_t sample_scope(uint32_t hz, int64_t now, const Capture *burst) {
    static int64_t last=-1;
    if (!atomic_load(&watching) || (now==last && !burst)) return ESP_OK;
    uint8_t code;
    esp_err_t err=radio_rssi(&code);
    if (err!=ESP_OK) return err;
    last=now;
    portENTER_CRITICAL(&scope_lock);
    scope_sample(&scope,hz,now,code,burst);
    portEXIT_CRITICAL(&scope_lock);
    return ESP_OK;
}

static esp_err_t listen(void) {
    Capture c={.dial_hz=DEFAULT_HZ};
    esp_err_t err=radio_start(false);
    if (err==ESP_OK) err=arm_channel(&c);
    if (err!=ESP_OK) return err;
    atomic_store(&ready,true);
    printf("SXREADY %08"PRIx32"\n",boot_id);
    int64_t last_armed=now_ms(), calibrated=last_armed;
    while (true) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        uint8_t state;
        err=radio_state(&state);
        if (err!=ESP_OK) return err;
        atomic_fetch_add(&polls,1);
        int64_t now=now_ms(), detected=esp_timer_get_time();
        if (state==SCAN_COMPLETE) {
            c.first_ms=last_armed;
            c.last_ms=now;
            err=radio_read(c.raw);
            if (err!=ESP_OK) return err;
            c.block=atomic_fetch_add(&bursts,1)+1;
        } else if (state!=SCAN_ARMED) return ESP_ERR_INVALID_RESPONSE;

        err=sample_scope(c.dial_hz,now,state==SCAN_COMPLETE ? &c : NULL);
        if (err!=ESP_OK) return err;
        bool recalibrate=c.dial_hz!=atomic_load(&requested_hz) || now-calibrated>=60000;
        /* Start the next capture before spending time on this one. At a
         * frequency change preserve the old capture's dial and floor until
         * the queue has copied them. */
        if (state==SCAN_COMPLETE && !recalibrate) {
            if ((err=radio_arm(c.threshold))!=ESP_OK) return err;
        }
        int64_t rearmed=esp_timer_get_time();
        if (state==SCAN_COMPLETE) {
            if (uxQueueMessagesWaiting(captures)>=UNCHECKED && !decoder_candidate(&c)) {
                atomic_fetch_add(&filtered,1);
            } else if (xQueueSend(captures,&c,0)!=pdTRUE) {
                atomic_fetch_add(&full_queue,1);
            } else {
                unsigned depth=uxQueueMessagesWaiting(captures);
                if (depth>atomic_load(&high_water)) atomic_store(&high_water,depth);
            }
        }
        if (recalibrate) {
            if (state==SCAN_ARMED && (err=radio_finish())!=ESP_OK) return err;
            err=arm_channel(&c);
            if (err!=ESP_OK) return err;
            rearmed=esp_timer_get_time();
            calibrated=rearmed/1000;
        }
        last_armed=rearmed/1000;
        if (state==SCAN_COMPLETE) {
            unsigned us=rearmed-detected;
            if (us>atomic_load(&max_rearm_us)) atomic_store(&max_rearm_us,us);
        }
    }
}

static void radio_task(void *unused) {
    ESP_ERROR_CHECK(radio_bus_init());
    esp_timer_handle_t timer;
    esp_timer_create_args_t args={.callback=wake_radio,.arg=xTaskGetCurrentTaskHandle(),
                                  .name="poll radio",.skip_unhandled_events=true};
    ESP_ERROR_CHECK(esp_timer_create(&args,&timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer,250));
    while (true) {
        esp_err_t err=radio_self_test();
        if (err==ESP_OK) err=listen();
        atomic_store(&ready,false);
        atomic_fetch_add(&restarts,1);
        printf("SXERROR radio %s\n",esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* A window of noise can wait for the next periodic trace. An accepted
 * aircraft word always goes through to the log and the display. */
static void report(const Result *r) {
    static int64_t last_report=-1000;
    int64_t now=now_ms();
    if (!r->length && !r->score && now-last_report<250) return;
    if (xQueueSend(results,r,r->length?portMAX_DELAY:0)==pdTRUE) last_report=now;
}

static void decode_task(void *unused) {
    Capture c;
    while (true) {
        if (xQueueReceive(captures,&c,0)) decoder_capture(&c);
        else if (!decoder_idle(now_ms())) {
            atomic_fetch_add(&idle_polls,1);
            if (xQueueReceive(captures,&c,pdMS_TO_TICKS(10))) decoder_capture(&c);
        }
        atomic_store(&deferred,decoder_evictions());
        vTaskDelay(1);  /* Let this core's idle task run even in dense traffic. */
    }
}

static const char *menu_message;
static bool save_pending, mounted, have_trace;
static unsigned test_count;
static int64_t cleared_through_ms=-1;

static void close_menu(void) {
    menu.open=false;
    save_pending=false;
    atomic_store(&watching,false);
}

static void finish_save(void) {
    if (!save_pending || !atomic_load(&ready) || menu.hz!=atomic_load(&applied_hz)) return;
    save_pending=false;
    if (save_dial(menu.hz)) close_menu();
    else menu_message="Save failed";
}

static void handle_key(Key key) {
    if (key==KEY_NONE) return;
    if (!menu.open) {
        if (key==KEY_TAP) history_older();
        if (key==KEY_HOLD) history_live();
        if (key==KEY_DOUBLE) {
            menu_open(&menu,atomic_load(&requested_hz));
            portENTER_CRITICAL(&scope_lock);
            memset(&scope,0,sizeof(scope));
            portEXIT_CRITICAL(&scope_lock);
            menu_message=NULL;
            atomic_store(&watching,true);
        }
        return;
    }
    if (key==KEY_REPEAT && menu.action!=TUNE_UP && menu.action!=TUNE_DOWN) return;
    menu_message=NULL;
    save_pending=false;
    if (key==KEY_DOUBLE) {
        save_pending=true;
        finish_save();
    } else if (key==KEY_TAP) menu_next(&menu);
    else if (key==KEY_HOLD || key==KEY_REPEAT) {
        if (menu.action==DISCARD) {
            atomic_store(&requested_hz,saved_hz);
            close_menu();
        } else if (menu.action==CLEAR_HISTORY) {
            int64_t cutoff=now_ms();
            EraseResult result=mounted ? storage_clear_packets() : ERASE_FAILED;
            if (result==ERASE_FAILED) { menu_message="Erase failed"; return; }
            history_clear();
            cleared_through_ms=cutoff;
            test_count=0;
            have_trace=false;
            menu_message=result==ERASE_DONE ? "History cleared" : "Erase incomplete";
        } else {
            menu_adjust(&menu);
            atomic_store(&failed_hz,0);
            atomic_store(&requested_hz,menu.hz);
        }
    }
}

static void button(void) {
    static Button b;
    handle_key(button_update(&b,!gpio_get_level(BOOT_BUTTON),now_ms()));
}

/* USB stays useful in the field without a Python REPL. Commands are lines.
 * An imported card retains its old boot and time, so it appears as saved. */
static void console(bool screen_ok) {
    static char line[1536];
    static size_t used;
    static bool overlong;
    char buffer[128];
    int n=read(STDIN_FILENO,buffer,sizeof(buffer));
    for (int i=0; i<n; i++) {
        char ch=buffer[i];
        if (ch!='\r' && ch!='\n') {
            if (used+1<sizeof(line)) line[used++]=ch;
            else overlong=true;
            continue;
        }
        line[used]=0;
        if (!overlong && used) {
            if (!strcmp(line,"dump") && mounted) storage_dump();
            else if (!strcmp(line,"screen")) display_dump();
            else if (!strncmp(line,"key ",4)) {
                const char *key=line+4;
                if (!strcmp(key,"tap")) handle_key(KEY_TAP);
                if (!strcmp(key,"hold")) handle_key(KEY_HOLD);
                if (!strcmp(key,"double")) handle_key(KEY_DOUBLE);
            }
            else if (!strcmp(line,"status"))
                printf("SXSTATUS %08"PRIx32" ready=%u bursts=%u polls=%u queue_peak=%u queue_full=%u filtered=%u idle_polls=%u "
                       "rearm_max_us=%u retries=%u deferred_drops=%u log=%u oled=%u dial=%u saved=%u menu=%u scope_cols=%u scope_samples=%u rssi_code=%d peak_code=%d scope_hits=%u wave_block=%u zoom=%u\n",boot_id,atomic_load(&ready),
                       atomic_load(&bursts),atomic_load(&polls),atomic_load(&high_water),atomic_load(&full_queue),atomic_load(&filtered),atomic_load(&idle_polls),
                       atomic_load(&max_rearm_us),atomic_load(&restarts),atomic_load(&deferred),mounted,screen_ok,atomic_load(&applied_hz),
                       (unsigned)saved_hz,menu.open,menu.open?view.columns:0,menu.open?view.samples:0,
                       menu.open?view.level:-1,menu.open?view.strongest:-1,menu.open?view.triggers:0,
                       menu.open?(unsigned)view.block:0,menu.open && view.zoom);
            else if (!strncmp(line,"import ",7) && mounted) {
                bool ok=storage_import(line+7);
                puts(ok?"SXIMPORT OK":"SXIMPORT ERROR");
            }
        }
        used=0; overlong=false;
    }
}

static void field_task(void *unused) {
    fcntl(STDIN_FILENO,F_SETFL,O_NONBLOCK);
    mounted=storage_init()==ESP_OK;
    bool log_ok=mounted;
    bool oled=display_init()==ESP_OK, screen_ok=oled;
    history_set_boot(boot_id);
    if (mounted) storage_restore();
    gpio_config_t pin={.pin_bit_mask=1ULL<<BOOT_BUTTON,.mode=GPIO_MODE_INPUT,.pull_up_en=true};
    ESP_ERROR_CHECK(gpio_config(&pin));
    int64_t last_save=-30000, last_draw=-1000, last_clock=now_ms();
    uint8_t trace[SAMPLES];
    while (true) {
        Result r;
        bool redraw=false;
        /* A waveform captured before Clear can finish decoding afterwards. */
        if (xQueueReceive(results,&r,pdMS_TO_TICKS(10)) && r.capture.last_ms>cleared_through_ms) {
            int64_t now=now_ms();
            if (r.length && !r.evidence_index) {
                Card c={.boot=boot_id,.ms=r.capture.last_ms,.length=r.length};
                memcpy(c.word,r.word,r.length);
                if (r.word[0]>>3==2) c.test_count=++test_count;
                history_add(c);
                redraw=true;
            }
            char line[1280];
            observation(line,sizeof(line),&r,boot_id,now,test_count,atomic_load(&deferred));
            printf("%s %"PRId64" %s\n",r.evidence_index?"SXSUPPORT":"SXOBS",now,line);
            if (r.length==7 && !r.evidence_index) {
                printf("SXDF11 %08"PRIx32" %"PRId64" ",boot_id,r.capture.last_ms);
                for (int i=0; i<7; i++) printf("%02x",r.word[i]);
                printf(" %u\n",r.evidence_count);
            }
            if (r.length || now-last_save>=30000) {
                log_ok=mounted && storage_save(line,r.length!=0);
                memcpy(trace,r.capture.raw,SAMPLES);
                have_trace=true;
                last_save=now;
            }
        }
        button();
        console(screen_ok);
        finish_save();
        int64_t now=now_ms();
        if (oled && (redraw || now-last_draw>=100)) {
            Screen s;
            if (menu.open) {
                Scope snapshot;
                portENTER_CRITICAL(&scope_lock);
                memcpy(&snapshot,&scope,sizeof(snapshot));
                portEXIT_CRITICAL(&scope_lock);
                if (atomic_load(&failed_hz)==menu.hz) {
                    save_pending=false;
                    menu.hz=atomic_load(&requested_hz);
                    menu_message="PLL did not lock";
                }
                scope_view(&snapshot,menu.hz,now,&view);
                const char *message=menu_message;
                if (menu.hz!=atomic_load(&applied_hz)) message="Tuning...";
                menu_screen(&menu,&view,message,s);
                screen_ok=display_draw(s,NO_CARD,NULL,&view)==ESP_OK;
            } else {
                CardView card=history_screen(s,boot_id,now);
                char *notice=s[card==POSITION_CARD ? 7 : 6];
                if (!atomic_load(&ready)) strcpy(notice,atomic_load(&restarts)?"Radio retry":"Starting radio");
                if (!log_ok) strcpy(notice,"Log unavailable");
                screen_ok=display_draw(s,card,card==NO_CARD && have_trace && !s[6][0]?trace:NULL,NULL)==ESP_OK;
            }
            last_draw=now;
        }
        if (now-last_clock>=60000) {
            printf("SXCLOCK %08"PRIx32" %"PRId64" %u %u %u %u %u 0 %u\n",boot_id,now,
                   atomic_load(&bursts),atomic_load(&polls),atomic_load(&restarts),atomic_load(&high_water),
                   atomic_load(&full_queue),atomic_load(&deferred));
            last_clock=now;
        }
    }
}

void app_main(void) {
    boot_id=esp_random();
    load_dial();
    captures=xQueueCreate(QUEUE_DEPTH,sizeof(Capture));
    results=xQueueCreate(16,sizeof(Result));
    ESP_ERROR_CHECK(captures && results && decoder_init(report) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(decoder_self_test() ? ESP_OK : ESP_ERR_INVALID_RESPONSE);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(field_task,"field",12288,NULL,3,NULL,0)==pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(decode_task,"decoder",24576,NULL,5,NULL,1)==pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(radio_task,"radio",8192,NULL,8,NULL,0)==pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}
