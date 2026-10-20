#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../common/amp_motor_protocol.h"
#include "../rtos/motor/motor_ipc.h"
#include "../rtos/motor/motor_task.h"

static AmpMotorSnapshot state;
static AmpMotorRequest submitted;
static AmpMotorReply completed;
static int queue_busy, reply_ready, snapshot_ready=1;
static unsigned submits;
int amp_motor_task_snapshot(AmpMotorSnapshot *out)
{ *out=state; return snapshot_ready; }
int amp_motor_task_submit(const AmpMotorRequest *r)
{ if (queue_busy) return -1; submitted=*r; ++submits; return 0; }
int amp_motor_task_receive(AmpMotorReply *r)
{ if (!reply_ready) return 0; *r=completed; reply_ready=0; return 1; }

static AMPMessage request(uint32_t op, uint64_t token, uint64_t session)
{
    AMPMessage r={0};
    r.type=AMP_MSG_REQUEST; r.session=session; r.sequence=9;
    r.opcode=AMP_OP_MOTOR_EXEC; r.payload_length=AMP_MOTOR_WIRE_REQUEST_SIZE;
    amp_motor_wire_put32(r.payload,1);
    amp_motor_wire_put32(r.payload+4,op);
    amp_motor_wire_put32(r.payload+24,(uint32_t)token);
    amp_motor_wire_put32(r.payload+28,(uint32_t)(token>>32));
    return r;
}
static AMPMessage finish(int32_t status, AmpMotorState next)
{
    AMPMessage original, out;
    state.state=next;
    completed=(AmpMotorReply){0}; completed.session=submitted.session;
    completed.sequence=submitted.sequence; completed.status=status;
    completed.snapshot=state; reply_ready=1;
    assert(amp_motor_ipc_collect(&original,&out)==1);
    assert(!amp_motor_ipc_pending() && original.session==submitted.session);
    assert(out.type==AMP_MSG_RESPONSE && out.sequence==9);
    assert(out.payload_length==64 && amp_motor_wire_geti32(out.payload+16)==status);
    return out;
}
int main(void)
{
    AMPMessage r, out, decoded;
    uint8_t frame[AMP_MSG_FRAME_SIZE];
    uint64_t token=UINT64_C(0x123456789abcdef0);
    unsigned i, before;
    amp_motor_ipc_init(); state.simulation=1;
    state.feedback.rpm=-50; state.feedback.current_ma=-100;
    state.feedback.temperature_decic=-5; state.feedback.valid=1;
    state.tick=UINT64_C(0x12345678abcdef00);
    r=request(AMP_MOTOR_WIRE_ENABLE,token,1);
    assert(!amp_message_encode(frame,&r) && !amp_message_decode(frame,&decoded));
    assert(!memcmp(r.payload,decoded.payload,r.payload_length));
    for (i=0;i<AMP_MOTOR_WIRE_REQUEST_SIZE;i++) {
        AMPMessage bad=r; bad.payload_length=i;
        assert(!amp_motor_ipc_handle(&bad,&out) && out.status==AMP_RESULT_BAD_PAYLOAD);
    }
    r.payload[20]=1;
    assert(!amp_motor_ipc_handle(&r,&out) && out.status==AMP_RESULT_BAD_PAYLOAD);
    r=request(1,0,1);
    assert(!amp_motor_ipc_handle(&r,&out) && out.status==AMP_RESULT_BAD_PAYLOAD);
    r=request(1,token,1); r.payload[0]=2;
    assert(!amp_motor_ipc_handle(&r,&out) && out.status==AMP_RESULT_BAD_PAYLOAD);
    r=request(1,token,1); queue_busy=1;
    assert(!amp_motor_ipc_handle(&r,&out) && out.status==AMP_RESULT_BUSY);
    queue_busy=0;
    assert(amp_motor_ipc_handle(&r,&out)==1 && amp_motor_ipc_pending());
    assert(submitted.command==AMP_MOTOR_ENABLE && submitted.session==1);
    before=submits;
    assert(!amp_motor_ipc_handle(&r,&out) && out.status==AMP_RESULT_BUSY && submits==before);
    assert(!amp_motor_ipc_collect(&decoded,&out));
    finish(0,AMP_MOTOR_READY);
    /* Short IPC sessions may change; logical controller token remains owner. */
    r=request(AMP_MOTOR_WIRE_TARGET,token,2);
    amp_motor_wire_put32(r.payload+8,(uint32_t)-250);
    assert(amp_motor_ipc_handle(&r,&out)==1 && submitted.value==-250);
    finish(0,AMP_MOTOR_READY);
    r=request(AMP_MOTOR_WIRE_START,token,3);
    assert(amp_motor_ipc_handle(&r,&out)==1); finish(0,AMP_MOTOR_RUNNING);
    r=request(AMP_MOTOR_WIRE_HEARTBEAT,token+1,4);
    assert(!amp_motor_ipc_handle(&r,&out) && out.status==AMP_RESULT_BAD_SESSION);
    r=request(AMP_MOTOR_WIRE_HEARTBEAT,token,5);
    assert(amp_motor_ipc_handle(&r,&out)==1); finish(0,AMP_MOTOR_RUNNING);

    r=(AMPMessage){0}; r.session=99; r.sequence=1; r.opcode=AMP_OP_MOTOR_STATUS;
    before=submits;
    assert(!amp_motor_ipc_handle(&r,&out) && out.status==AMP_RESULT_OK && submits==before);
    assert(amp_motor_wire_geti32(out.payload+28)==-50);
    assert(amp_motor_wire_geti32(out.payload+32)==-100);
    assert(amp_motor_wire_geti32(out.payload+36)==-5);
    assert(amp_motor_wire_get32(out.payload+56)==0xabcdef00U);
    assert(amp_motor_wire_get32(out.payload+60)==0x12345678U);
    assert(!amp_message_encode(frame,&out) && !amp_message_decode(frame,&decoded));
    assert(decoded.payload_length==64);
    snapshot_ready=0;
    assert(!amp_motor_ipc_handle(&r,&out) && out.status==AMP_RESULT_INTERNAL);
    snapshot_ready=1;

    /* Fault reset is explicit; fresh controller may recover after lost owner. */
    state.state=AMP_MOTOR_FAULT;
    r=request(AMP_MOTOR_WIRE_RESET,token+1,6);
    assert(amp_motor_ipc_handle(&r,&out)==1); finish(-2,AMP_MOTOR_FAULT);
    assert(amp_motor_ipc_handle(&r,&out)==1); finish(0,AMP_MOTOR_DISABLED);
    r=request(AMP_MOTOR_WIRE_ENABLE,token+2,7);
    assert(amp_motor_ipc_handle(&r,&out)==1); finish(0,AMP_MOTOR_READY);
    /* A dead client's READY state cannot permanently lock the controller. */
    r=request(AMP_MOTOR_WIRE_DISABLE,token+3,8);
    assert(amp_motor_ipc_handle(&r,&out)==1); finish(0,AMP_MOTOR_DISABLED);
    r=request(AMP_MOTOR_WIRE_ENABLE,token+3,9);
    assert(amp_motor_ipc_handle(&r,&out)==1); finish(0,AMP_MOTOR_READY);
    r=request(AMP_MOTOR_WIRE_INJECT,token+3,10);
    amp_motor_wire_put32(r.payload+12,1001);
    assert(!amp_motor_ipc_handle(&r,&out) && out.status==AMP_RESULT_BAD_PAYLOAD);
    amp_motor_wire_put32(r.payload+12,500); amp_motor_wire_put32(r.payload+16,7);
    assert(amp_motor_ipc_handle(&r,&out)==1 && submitted.kind==AMP_MOTOR_REQUEST_INJECT);
    finish(0,AMP_MOTOR_READY);
    r=request(AMP_MOTOR_WIRE_TARGET,token+3,11);
    amp_motor_wire_put32(r.payload+8,0x80000000U);
    assert(amp_motor_ipc_handle(&r,&out)==1 && submitted.value==INT32_MIN);
    finish(-1,AMP_MOTOR_READY);
    puts("PASS: motor wire codec, strict payloads, async replies, owner token/session separation, fault recovery");
    return 0;
}
