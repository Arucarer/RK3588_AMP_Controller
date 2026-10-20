/* IPC double backed by real bridge/state machine/SIM. No /dev/mem access.
 * Clock advances only through fake nanosleep; this is not an RTOS scheduler. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../linux/amp_ipc.h"
#include "../../common/amp_motor_protocol.h"
#include "../../rtos/motor/motor_task.h"
#include "../../rtos/motor/motor_ipc.h"

static AmpMotorControl motor;
static AmpMotorSim sim;
static AmpMotorReply reply;
static uint64_t milliseconds, sessions;
static unsigned cycles;
static int initialized, open_client, reply_ready, signal_sent, busy_once;

static void trace(const char *label, uint64_t value)
{
    const char *path=getenv("AMP_MOTOR_TEST_TRACE");
    FILE *f;
    if (!path) return;
    f=fopen(path,"a"); assert(f);
    fprintf(f,"%s %llu\n",label,(unsigned long long)value); fclose(f);
}
static AmpMotorSnapshot snapshot(void)
{
    AmpMotorSnapshot s={0};
    s.simulation=1; s.state=motor.state; s.faults=motor.faults;
    s.target_rpm=motor.target_rpm; s.demand_rpm=amp_motor_demand_rpm(&motor);
    s.feedback=sim.feedback; s.tick=milliseconds/10; s.cycles=cycles;
    return s;
}
int amp_motor_task_snapshot(AmpMotorSnapshot *out) { *out=snapshot(); return 1; }
int amp_motor_task_submit(const AmpMotorRequest *r)
{
    assert(!reply_ready);
    reply=(AmpMotorReply){0}; reply.session=r->session; reply.sequence=r->sequence;
    reply.status=r->kind==AMP_MOTOR_REQUEST_INJECT ?
        amp_motor_sim_inject(&sim,r->load_permille,r->injections) :
        amp_motor_command(&motor,r->command,r->value,&sim.feedback);
    reply.snapshot=snapshot(); reply_ready=1; return 0;
}
int amp_motor_task_receive(AmpMotorReply *out)
{ if (!reply_ready) return 0; *out=reply; reply_ready=0; return 1; }

int amp_client_open(AMPClient *client)
{
    const AmpMotorConfig c={3000,1000,2000,5000,800,100,1000,5};
    const AmpMotorSimConfig s={3000,250,100,1000,100,6000,800};
    assert(!open_client);
    if (!initialized) {
        assert(!amp_motor_init(&motor,&c)); assert(!amp_motor_sim_init(&sim,&s));
        amp_motor_ipc_init(); initialized=1;
    }
    if (getenv("AMP_MOTOR_TEST_BUSY") && !busy_once) {
        busy_once=1; errno=EWOULDBLOCK; trace("busy",1); return -1;
    }
    memset(client,0,sizeof(*client)); client->session=++sessions; client->connected=1;
    open_client=1; trace("open",sessions); return 0;
}
void amp_client_close(AMPClient *client)
{ assert(open_client); open_client=0; client->connected=0; trace("close",sessions); }

int amp_client_call(AMPClient *client, uint32_t opcode, const void *payload,
                    uint32_t length, AMPMessage *out)
{
    AMPMessage request={0}, decoded, original;
    uint8_t frame[AMP_MSG_FRAME_SIZE];
    const char *malformed=getenv("AMP_MOTOR_TEST_BAD");
    assert(open_client);
    request.type=AMP_MSG_REQUEST; request.session=client->session;
    request.sequence=++client->sequence; request.opcode=opcode; request.payload_length=length;
    if (length) memcpy(request.payload,payload,length);
    assert(!amp_message_encode(frame,&request) && !amp_message_decode(frame,&decoded));
    if (amp_motor_ipc_handle(&decoded,out)) assert(amp_motor_ipc_collect(&original,out));
    trace("operation",opcode==AMP_OP_MOTOR_EXEC ? amp_motor_wire_get32(request.payload+4) : 0);
    if (length) trace("token",(uint64_t)amp_motor_wire_get32(request.payload+24)|
                               ((uint64_t)amp_motor_wire_get32(request.payload+28)<<32));
    if (getenv("AMP_MOTOR_TEST_FAIL_OP") && length &&
        amp_motor_wire_get32(request.payload+4)==5) {
        /* Target was accepted, but response was lost. Never retry the target. */
        errno=ETIMEDOUT; return -1;
    }
    if (malformed) {
        if (!strcmp(malformed,"mode")) amp_motor_wire_put32(out->payload+4,2);
        if (!strcmp(malformed,"length")) out->payload_length=4;
        if (!strcmp(malformed,"old")) out->status=AMP_RESULT_UNSUPPORTED;
        if (!strcmp(malformed,"transport")) { errno=ETIMEDOUT; return -1; }
    }
    return 0;
}

int motor_test_clock_gettime(clockid_t clock, struct timespec *out)
{
    assert(clock==CLOCK_MONOTONIC);
    out->tv_sec=(time_t)(milliseconds/1000);
    out->tv_nsec=(long)(milliseconds%1000)*1000000; return 0;
}
int motor_test_nanosleep(const struct timespec *req, struct timespec *remaining)
{
    uint64_t count=(uint64_t)req->tv_sec*1000+(uint64_t)req->tv_nsec/1000000;
    const char *fault=getenv("AMP_MOTOR_TEST_FAULT");
    (void)remaining;
    assert(!open_client); /* IPC lock must be released during every sleep. */
    while (count) {
        unsigned dt=count>=10 ? 10 : (unsigned)count;
        milliseconds+=dt; count-=dt;
        if (fault && milliseconds>=500) {
            if (!strcmp(fault,"temperature")) assert(!amp_motor_sim_inject(&sim,0,4));
            if (!strcmp(fault,"signal") && !signal_sent) { signal_sent=1; raise(SIGINT); }
        }
        assert(!amp_motor_step(&motor,dt,&sim.feedback));
        assert(!amp_motor_sim_step(&sim,dt,amp_motor_demand_rpm(&motor),
            motor.state==AMP_MOTOR_RUNNING || motor.state==AMP_MOTOR_STOPPING));
        ++cycles;
    }
    return 0;
}
