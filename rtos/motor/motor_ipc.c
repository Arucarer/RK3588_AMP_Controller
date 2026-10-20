#include "motor_ipc.h"
#include "motor_task.h"
#include "../../common/amp_motor_protocol.h"

static int pending;
static uint64_t owner;
static AMPMessage saved_request;
static uint32_t saved_operation;
static uint64_t saved_token;

static void begin(const AMPMessage *r, AMPMessage *out)
{
    out->type=AMP_MSG_RESPONSE; out->session=r->session;
    out->sequence=r->sequence; out->opcode=r->opcode;
    out->status=AMP_RESULT_OK; out->payload_length=0;
}

static void encode(AMPMessage *out, const AmpMotorSnapshot *s, int32_t status)
{
    const uint32_t words[16]={AMP_MOTOR_WIRE_VERSION, s->simulation,
        (uint32_t)s->state, s->faults, (uint32_t)status,
        (uint32_t)s->target_rpm, (uint32_t)s->demand_rpm,
        (uint32_t)s->feedback.rpm, (uint32_t)s->feedback.current_ma,
        (uint32_t)s->feedback.temperature_decic, s->feedback.age_ms,
        s->feedback.valid, s->cycles, s->overruns,
        (uint32_t)s->tick, (uint32_t)(s->tick>>32)};
    unsigned i;
    out->payload_length=AMP_MOTOR_WIRE_STATUS_SIZE;
    for (i=0;i<16;i++) amp_motor_wire_put32(out->payload+4*i,words[i]);
}

void amp_motor_ipc_init(void) { pending=0; owner=0; saved_operation=0; }
int amp_motor_ipc_pending(void) { return pending; }

int amp_motor_ipc_handle(const AMPMessage *r, AMPMessage *out)
{
    AmpMotorSnapshot snapshot;
    AmpMotorRequest command={0};
    uint32_t op, value, load, injections;
    uint64_t token;
    static const AmpMotorCommand operations[7]={AMP_MOTOR_ENABLE, AMP_MOTOR_DISABLE,
        AMP_MOTOR_START, AMP_MOTOR_STOP, AMP_MOTOR_TARGET,
        AMP_MOTOR_HEARTBEAT, AMP_MOTOR_RESET_FAULT};
    begin(r,out);
    if (pending) { out->status=AMP_RESULT_BUSY; return 0; }
    if (!r->session || !r->sequence || r->status) goto bad;
    if (r->opcode!=AMP_OP_MOTOR_EXEC && r->opcode!=AMP_OP_MOTOR_STATUS) {
        out->status=AMP_RESULT_UNSUPPORTED; return 0;
    }
    if (r->opcode==AMP_OP_MOTOR_STATUS) {
        if (r->payload_length) goto bad;
    } else {
        if (r->payload_length!=AMP_MOTOR_WIRE_REQUEST_SIZE ||
            amp_motor_wire_get32(r->payload)!=AMP_MOTOR_WIRE_VERSION ||
            amp_motor_wire_get32(r->payload+20)) goto bad;
        op=amp_motor_wire_get32(r->payload+4);
        value=amp_motor_wire_get32(r->payload+8);
        load=amp_motor_wire_get32(r->payload+12);
        injections=amp_motor_wire_get32(r->payload+16);
        if (op<1 || op>8 || (op!=AMP_MOTOR_WIRE_TARGET && value) ||
            (op!=AMP_MOTOR_WIRE_INJECT && (load || injections)) ||
            load>1000 || (injections&~7U)) goto bad;
        command.kind=op==AMP_MOTOR_WIRE_INJECT ? AMP_MOTOR_REQUEST_INJECT : AMP_MOTOR_REQUEST_CONTROL;
        command.command=op<=7 ? operations[op-1] : AMP_MOTOR_ENABLE;
        command.value=amp_motor_wire_geti32(r->payload+8);
        command.load_permille=load; command.injections=injections;
    }
    if (amp_motor_task_snapshot(&snapshot)!=1 || snapshot.simulation!=1) {
        out->status=AMP_RESULT_INTERNAL; return 0;
    }
    if (r->opcode==AMP_OP_MOTOR_STATUS) { encode(out,&snapshot,0); return 0; }

    op=amp_motor_wire_get32(r->payload+4);
    token=(uint64_t)amp_motor_wire_get32(r->payload+24) |
        ((uint64_t)amp_motor_wire_get32(r->payload+28)<<32);
    if (!token) goto bad;
    /* Read-only clients never acquire control. A new controller token can explicitly
     * enable an idle controller or reset a fault, but cannot take a running
     * controller's ownership or refresh its lease. */
    if (token!=owner &&
        !(op==AMP_MOTOR_WIRE_ENABLE && snapshot.state==AMP_MOTOR_DISABLED) &&
        !(op==AMP_MOTOR_WIRE_DISABLE &&
          (snapshot.state==AMP_MOTOR_READY || snapshot.state==AMP_MOTOR_DISABLED)) &&
        !(op==AMP_MOTOR_WIRE_RESET && snapshot.state==AMP_MOTOR_FAULT)) {
        out->status=AMP_RESULT_BAD_SESSION; return 0;
    }
    command.session=r->session; command.sequence=r->sequence;
    if (amp_motor_task_submit(&command)) { out->status=AMP_RESULT_BUSY; return 0; }
    saved_request=*r; saved_operation=op; saved_token=token; pending=1;
    return 1;
bad:
    out->status=AMP_RESULT_BAD_PAYLOAD;
    return 0;
}

int amp_motor_ipc_collect(AMPMessage *original, AMPMessage *out)
{
    AmpMotorReply reply;
    if (!pending || amp_motor_task_receive(&reply)!=1) return 0;
    *original=saved_request; begin(original,out); pending=0;
    if (reply.session!=original->session || reply.sequence!=original->sequence ||
        reply.snapshot.simulation!=1) {
        out->status=AMP_RESULT_INTERNAL; return 1;
    }
    if (!reply.status) {
        if (saved_operation==AMP_MOTOR_WIRE_ENABLE) owner=saved_token;
        if (saved_operation==AMP_MOTOR_WIRE_DISABLE || saved_operation==AMP_MOTOR_WIRE_RESET)
            owner=0;
    }
    encode(out,&reply.snapshot,reply.status);
    return 1;
}
