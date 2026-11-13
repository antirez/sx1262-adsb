/* Read the field receiver without resetting it. On the S3 the USB line
 * controls double as boot and reset pins. Leave DTR and RTS alone. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static long long milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}

static int open_receiver(const char *port) {
    int fd=open(port,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if (fd<0) return -1;
    struct termios t;
    if (tcgetattr(fd,&t)<0) goto fail;
    cfmakeraw(&t);
    cfsetispeed(&t,B115200);
    cfsetospeed(&t,B115200);
    t.c_cflag|=CLOCAL|CREAD;
    t.c_cflag&=~HUPCL;
    if (tcsetattr(fd,TCSANOW,&t)<0) goto fail;
    return fd;
fail:
    close(fd);
    return -1;
}

/* Live observations may appear during a dump. Only SXLOG lines belong to
 * the saved journal. Seeing SXEND distinguishes a complete dump from a cable
 * that came loose after a perfectly good JSON line. */
static bool command(int fd, bool dump, bool quiet) {
    const char *request=dump?"dump\n":"status\n";
    char line[2048];
    size_t used=0;
    bool truncated=false;
    long long deadline=milliseconds()+(dump?120000:15000), sent=0;
    while (milliseconds()<deadline) {
        long long now=milliseconds();
        if (!sent || (!dump && now-sent>=1500)) {
            if (write(fd,request,strlen(request))!=(ssize_t)strlen(request)) return false;
            sent=now;
        }
        struct pollfd p={.fd=fd,.events=POLLIN};
        int ready=poll(&p,1,250);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0 || (p.revents&(POLLHUP|POLLERR|POLLNVAL))) return false;
        if (!ready) continue;
        char buf[2048];
        ssize_t n=read(fd,buf,sizeof(buf));
        if (n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if (n<=0) return false;
        for (ssize_t i=0; i<n; i++) {
            if (buf[i]=='\r') continue;
            if (buf[i]!='\n') {
                if (used+1<sizeof(line)) line[used++]=buf[i];
                else truncated=true;
                continue;
            }
            line[used]=0;
            if (dump && !strncmp(line,"SXLOG ",6)) {
                if (truncated || puts(line+6)==EOF) return false;
            }
            if (dump && !strcmp(line,"SXEND")) return true;
            if (!dump && !strncmp(line,"SXSTATUS ",9)) {
                if (!quiet) puts(line);
                return true;
            }
            used=0;
            truncated=false;
        }
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc!=3 || (strcmp(argv[2],"status") && strcmp(argv[2],"dump"))) {
        fputs("usage: receiverctl PORT status|dump\n",stderr);
        return 2;
    }
    int fd=open_receiver(argv[1]);
    if (fd<0) { perror(argv[1]); return 1; }
    bool dump=!strcmp(argv[2],"dump");
    bool ok=command(fd,false,dump);
    if (ok && dump) ok=command(fd,true,false);
    close(fd);
    if (!ok) fputs("Receiver disconnected or did not finish the command.\n",stderr);
    return ok && fflush(stdout)==0 ? 0 : 1;
}
