#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "amp_ipc.h"
#include "../common/amp_motor_protocol.h"

static volatile sig_atomic_t interrupted;
static uint64_t token;
static void on_signal(int signal_number) { (void)signal_number; interrupted=1; }

static int number(const char *text, long minimum, long maximum, long *out)
{
    char *end;
    long value;
    if (!text || !*text || (*text!='-' && (*text<'0' || *text>'9'))) return -1;
    errno=0; value=strtol(text,&end,10);
    if (errno || *end || value<minimum || value>maximum) return -1;
    *out=value; return 0;
}

static uint64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static void pause_ms(unsigned ms)
{
    struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000};
    while (nanosleep(&t,&t) && errno==EINTR && !interrupted) {}
}

/* Separate controller lock; never keep the shared IPC lock between calls.
 * Token is cooperative ownership, not authentication. Preserve a nonzero token
 * across short IPC sessions. Never follow symlinks or accept shared key files. */
static int open_token(const char *path)
{
    int fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    struct stat st;
    uint8_t bytes[8];
    ssize_t n;
    if (fd<0) { perror("token file"); return -1; }
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1 || st.st_uid!=geteuid() ||
        (st.st_mode&077) || flock(fd,LOCK_EX|LOCK_NB) || fstat(fd,&st)) {
        fprintf(stderr,"Token file is unsafe or another motor controller owns it\n");
        close(fd); return -1;
    }
    if (st.st_size==0) {
        int random_fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
        if (random_fd<0) { perror("urandom"); close(fd); return -1; }
        n=read(random_fd,bytes,sizeof(bytes)); close(random_fd);
        if (n!=8) { fprintf(stderr,"Cannot generate controller token\n"); close(fd); return -1; }
        if (!(amp_motor_wire_get32(bytes)|amp_motor_wire_get32(bytes+4))) bytes[0]=1;
        if (pwrite(fd,bytes,8,0)!=8 || fsync(fd)) {
            perror("save token"); close(fd); return -1;
        }
    } else if (st.st_size!=8 || pread(fd,bytes,8,0)!=8) {
        fprintf(stderr,"Invalid token file; do not replace it while a motor is active\n");
        close(fd); return -1;
    }
    token=(uint64_t)amp_motor_wire_get32(bytes)|((uint64_t)amp_motor_wire_get32(bytes+4)<<32);
    if (!token) { fprintf(stderr,"Invalid zero token\n"); close(fd); return -1; }
    return fd;
}

static int exchange(uint32_t operation, int32_t target, uint32_t load,
                     uint32_t injections, AMPMessage *response, int display)
{
    AMPClient client;
    uint8_t payload[AMP_MOTOR_WIRE_REQUEST_SIZE]={0};
    uint32_t opcode=operation ? AMP_OP_MOTOR_EXEC : AMP_OP_MOTOR_STATUS;
    static const char *const states[]={"DISABLED","READY","RUNNING","STOPPING","FAULT"};
    uint32_t state;
    int32_t result;
    uint64_t lock_deadline;
    if (operation) {
        amp_motor_wire_put32(payload,1); amp_motor_wire_put32(payload+4,operation);
        amp_motor_wire_put32(payload+8,(uint32_t)target);
        amp_motor_wire_put32(payload+12,load); amp_motor_wire_put32(payload+16,injections);
        amp_motor_wire_put32(payload+24,(uint32_t)token);
        amp_motor_wire_put32(payload+28,(uint32_t)(token>>32));
    }
    lock_deadline=now_ms()+200;
    while (amp_client_open(&client)) {
        if ((errno!=EAGAIN && errno!=EWOULDBLOCK) || now_ms()>=lock_deadline) {
            perror("amp_client_open"); return -1;
        }
        /* Retry only lock acquisition, never a command with unknown outcome. */
        pause_ms(10);
    }
    if (amp_client_call(&client,opcode,operation ? payload : NULL,
                         operation ? sizeof(payload) : 0,response)) {
        perror("amp_client_call: outcome may be unknown");
        amp_client_close(&client); return -1;
    }
    amp_client_close(&client);
    if (response->type!=AMP_MSG_RESPONSE || response->opcode!=opcode ||
        response->status!=AMP_RESULT_OK) {
        fprintf(stderr,"Motor IPC rejected: status=%u (old firmware, busy, or ownership mismatch)\n",response->status);
        return -1;
    }
    if (response->payload_length!=AMP_MOTOR_WIRE_STATUS_SIZE ||
        amp_motor_wire_get32(response->payload)!=1 ||
        amp_motor_wire_get32(response->payload+4)!=1 ||
        amp_motor_wire_get32(response->payload+8)>4 ||
        amp_motor_wire_get32(response->payload+44)>1) {
        fprintf(stderr,"Invalid motor response or non-SIM mode\n"); return -1;
    }
    state=amp_motor_wire_get32(response->payload+8);
    result=amp_motor_wire_geti32(response->payload+16);
    if (display || result) {
        printf("MODE=SIM STATE=%s RESULT=%d FAULTS=0x%08x TARGET=%d DEMAND=%d RPM=%d "
               "CURRENT_MA=%d TEMP_DECIC=%d AGE_MS=%u VALID=%u CYCLES=%u OVERRUNS=%u\n",
            states[state],result,amp_motor_wire_get32(response->payload+12),
            amp_motor_wire_geti32(response->payload+20),amp_motor_wire_geti32(response->payload+24),
            amp_motor_wire_geti32(response->payload+28),amp_motor_wire_geti32(response->payload+32),
            amp_motor_wire_geti32(response->payload+36),amp_motor_wire_get32(response->payload+40),
            amp_motor_wire_get32(response->payload+44),amp_motor_wire_get32(response->payload+48),
            amp_motor_wire_get32(response->payload+52));
        fflush(stdout);
    }
    return result ? -1 : 0;
}

static int run_motor(int32_t rpm, unsigned seconds)
{
    AMPMessage response;
    uint64_t deadline;
    int failed=1;
    /* Do not auto-reset a fault or resume a previous run. */
    if (exchange(AMP_MOTOR_WIRE_ENABLE,0,0,0,&response,1)) return -1;
    if (interrupted || exchange(AMP_MOTOR_WIRE_TARGET,rpm,0,0,&response,0)) goto cleanup;
    if (interrupted || exchange(AMP_MOTOR_WIRE_START,0,0,0,&response,1)) goto cleanup;
    deadline=now_ms()+(uint64_t)seconds*1000;
    while (!interrupted && now_ms()<deadline) {
        if (exchange(AMP_MOTOR_WIRE_HEARTBEAT,0,0,0,&response,1) ||
            amp_motor_wire_get32(response.payload+8)!=2) goto cleanup;
        pause_ms(200);
    }
    if (exchange(AMP_MOTOR_WIRE_STOP,0,0,0,&response,1)) goto cleanup;
    deadline=now_ms()+5000;
    for (;;) {
        if (exchange(AMP_MOTOR_WIRE_HEARTBEAT,0,0,0,&response,0)) goto cleanup;
        if (amp_motor_wire_get32(response.payload+8)==1) break;
        if (amp_motor_wire_get32(response.payload+8)!=3 || now_ms()>=deadline) goto cleanup;
        /* Even after Ctrl-C, permit controlled deceleration with a lease. */
        pause_ms(100);
    }
    failed=interrupted ? 1 : 0;
cleanup:
    /* Best effort only. Failed communications cannot prove the drive stopped;
     * no automatic replay of start/target; RTOS lease remains the fallback. */
    if (exchange(AMP_MOTOR_WIRE_DISABLE,0,0,0,&response,1)) failed=1;
    return failed ? -1 : 0;
}

static int usage(const char *name)
{
    fprintf(stderr,"usage: %s [--token-file PATH] status|enable|disable|start|stop|heartbeat|reset\n"
        "       %s [--token-file PATH] target RPM\n"
        "       %s [--token-file PATH] inject LOAD_PERMILLE FLAGS\n"
        "       %s [--token-file PATH] run RPM SECONDS\n"
        "SIM only, RPM -3000..3000, SECONDS 1..3600; flags: stall=1 feedback-loss=2 overtemp=4\n"
        "Use run for automatic lease renewal. Standalone start needs explicit heartbeat.\n",
        name,name,name,name);
    return 2;
}

int main(int argc, char **argv)
{
    const char *token_path="/run/amp-motor.token", *command;
    uint32_t operation=0;
    long a=0,b=0;
    int index=1, fd=-1, result, is_run=0;
    AMPMessage response;
    struct sigaction action;
    if (argc>2 && !strcmp(argv[1],"--token-file")) { token_path=argv[2]; index=3; }
    if (index>=argc) return usage(argv[0]);
    command=argv[index++];
    if (!strcmp(command,"status") && index==argc) operation=0;
    else if (!strcmp(command,"target") && index+1==argc) {
        operation=AMP_MOTOR_WIRE_TARGET;
        if (number(argv[index],-3000,3000,&a)) return usage(argv[0]);
    } else if (!strcmp(command,"inject") && index+2==argc) {
        operation=AMP_MOTOR_WIRE_INJECT;
        if (number(argv[index],0,1000,&a) || number(argv[index+1],0,7,&b)) return usage(argv[0]);
    } else if (!strcmp(command,"run") && index+2==argc) {
        is_run=1; operation=AMP_MOTOR_WIRE_START;
        if (number(argv[index],-3000,3000,&a) || number(argv[index+1],1,3600,&b)) return usage(argv[0]);
    } else if (index==argc) {
        if (!strcmp(command,"enable")) operation=AMP_MOTOR_WIRE_ENABLE;
        else if (!strcmp(command,"disable")) operation=AMP_MOTOR_WIRE_DISABLE;
        else if (!strcmp(command,"start")) operation=AMP_MOTOR_WIRE_START;
        else if (!strcmp(command,"stop")) operation=AMP_MOTOR_WIRE_STOP;
        else if (!strcmp(command,"heartbeat")) operation=AMP_MOTOR_WIRE_HEARTBEAT;
        else if (!strcmp(command,"reset")) operation=AMP_MOTOR_WIRE_RESET;
        else return usage(argv[0]);
    } else return usage(argv[0]);
    memset(&action,0,sizeof(action)); action.sa_handler=on_signal; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL)) { perror("sigaction"); return 1; }
    if (operation) { fd=open_token(token_path); if (fd<0) return 1; }
    result=is_run ? run_motor((int32_t)a,(unsigned)b) :
        exchange(operation,operation==AMP_MOTOR_WIRE_TARGET ? (int32_t)a : 0,
            operation==AMP_MOTOR_WIRE_INJECT ? (uint32_t)a : 0,
            operation==AMP_MOTOR_WIRE_INJECT ? (uint32_t)b : 0,&response,1);
    if (fd>=0) close(fd);
    return result ? 1 : 0;
}
