#include <assert.h>
#include <stdio.h>
#include "protocol/modbus_master.h"
#include "protocol/modbus_rtu.h"
typedef struct { uint64_t now, sent; unsigned writes, index; int mode; uint8_t reply[8]; } Fake;
static uint64_t now(void *p) { return ((Fake *)p)->now; }
static void idle(void *p) { ((Fake *)p)->now+=100; }
static int write_bytes(void *p,const uint8_t *b,size_t n,uint32_t budget,size_t *done)
{
    Fake *f=p; (void)budget;
    ++f->writes; f->sent=f->now; f->index=0; *done=n;
    if (f->mode==4) { *done=3; return -1; }
    for (unsigned i=0;i<8;++i) f->reply[i]=b[i];
    return 0;
}
static int poll_byte(void *p,uint8_t *b,uint64_t *stamp)
{
    Fake *f=p;
    if (!f->writes || f->mode==0 || (f->mode==2 && f->writes==1) || f->index==8) return 0;
    if (f->mode==5) return -1;
    uint64_t due=f->sent+1000+f->index*100;
    if (f->mode==3 && f->index>=4) due+=3000;
    if (f->now<due) return 0;
    *stamp=due; *b=f->reply[f->index++]; return 1;
}
int main(void)
{
    Fake f={0}; AmpMbIo io={&f,now,poll_byte,write_bytes,idle};
    AmpMbConfig c={9600,20000,10000,11,1}; AmpMbTransaction result;
    uint8_t request[8];
    assert(!amp_mb_build_request(1,6,3,42,request));
    f.mode=1;
    assert(!amp_mb_master_execute(&io,&c,request,&result));
    assert(result.length==8 && f.writes==1);
    f=(Fake){0};
    assert(amp_mb_master_execute(&io,&c,request,&result)==AMP_MB_WRITE_UNKNOWN);
    assert(f.writes==1);
    f=(Fake){.mode=4};
    assert(amp_mb_master_execute(&io,&c,request,&result)==AMP_MB_WRITE_UNKNOWN);
    assert(result.transmitted==3 && f.writes==1);
    f=(Fake){.mode=3};
    assert(amp_mb_master_execute(&io,&c,request,&result)==AMP_MB_GAP);
    f=(Fake){.mode=5};
    assert(amp_mb_master_execute(&io,&c,request,&result)==AMP_MB_WRITE_UNKNOWN);
    assert(!amp_mb_build_request(1,3,3,1,request));
    f=(Fake){0};
    assert(amp_mb_master_execute(&io,&c,request,&result)==AMP_MB_TIMEOUT);
    assert(f.writes==2);
    f=(Fake){.mode=2};
    assert(amp_mb_master_execute(&io,&c,request,&result)==AMP_MB_FORMAT);
    assert(f.writes==2); /* Fake echoes read request: invalid response, no third replay. */
    request[7]^=1;
    f=(Fake){0};
    assert(amp_mb_master_execute(&io,&c,request,&result)==AMP_MB_ARGUMENT);
    assert(f.writes==0);
    puts("PASS: master success, write ambiguity, partial TX, gap, I/O failure, bounded read retry, CRC rejection");
}
