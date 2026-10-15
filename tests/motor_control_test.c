#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../rtos/motor/motor_control.h"

static const AmpMotorConfig config = {
    3000, 1000, 2000, 5000, 800, 100, 1000, 5
};
static AmpMotorFeedback feedback = {0, 0, 250, 0, 1};

static void running(AmpMotorControl *m)
{
    assert(!amp_motor_init(m, &config));
    assert(!amp_motor_command(m, AMP_MOTOR_ENABLE, 0, &feedback));
    assert(!amp_motor_command(m, AMP_MOTOR_TARGET, 500, &feedback));
    assert(!amp_motor_command(m, AMP_MOTOR_START, 0, &feedback));
}

int main(void)
{
    AmpMotorControl m;
    AmpMotorConfig bad = config;
    AmpMotorFeedback f;
    unsigned i;
    assert(amp_motor_init(NULL, &config) == -1);
    bad.stopped_rpm = 3000;
    assert(amp_motor_init(&m, &bad) == -1);
    assert(!amp_motor_init(&m, &config));
    assert(m.state == AMP_MOTOR_DISABLED && !amp_motor_demand_rpm(&m));
    assert(amp_motor_command(&m, AMP_MOTOR_START, 0, &feedback) == -2);
    assert(amp_motor_command(&m, AMP_MOTOR_ENABLE, 0, NULL) == -2);
    running(&m);
    assert(amp_motor_command(&m, AMP_MOTOR_TARGET, INT32_MIN, &feedback) == -1);
    assert(m.target_rpm == 500);
    for (i = 0; i < 50; ++i) assert(!amp_motor_step(&m, 10, &feedback));
    assert(amp_motor_demand_rpm(&m) == 500);
    assert(!amp_motor_command(&m, AMP_MOTOR_TARGET, -500, &feedback));
    assert(!amp_motor_step(&m, 100, &feedback));
    assert(amp_motor_demand_rpm(&m) == 300);
    assert(!amp_motor_step(&m, 100, &feedback));
    assert(amp_motor_demand_rpm(&m) == 100);
    assert(!amp_motor_step(&m, 50, &feedback));
    assert(amp_motor_demand_rpm(&m) == 0);
    assert(!amp_motor_step(&m, 100, &feedback));
    assert(amp_motor_demand_rpm(&m) == -100);
    assert(!amp_motor_command(&m, AMP_MOTOR_HEARTBEAT, 0, &feedback));
    assert(!amp_motor_command(&m, AMP_MOTOR_STOP, 0, &feedback));
    f = feedback; f.rpm = 100;
    assert(!amp_motor_step(&m, 50, &f));
    assert(m.state == AMP_MOTOR_STOPPING); /* Zero demand is not zero feedback. */
    assert(!amp_motor_step(&m, 10, &feedback));
    assert(m.state == AMP_MOTOR_READY);

    running(&m);
    for (i = 0; i < 9; ++i) assert(!amp_motor_step(&m, 100, &feedback));
    assert(m.state == AMP_MOTOR_RUNNING);
    assert(!amp_motor_step(&m, 100, &feedback));
    assert(m.state == AMP_MOTOR_FAULT && (m.faults & AMP_MOTOR_FAULT_LEASE));
    assert(amp_motor_command(&m, AMP_MOTOR_START, 0, &feedback) == -2);
    assert(!amp_motor_command(&m, AMP_MOTOR_DISABLE, 0, &feedback));
    assert(m.state == AMP_MOTOR_FAULT);
    f = feedback; f.rpm = 6;
    assert(amp_motor_command(&m, AMP_MOTOR_RESET_FAULT, 0, &f) == -2);
    assert(!amp_motor_command(&m, AMP_MOTOR_RESET_FAULT, 0, &feedback));
    assert(m.state == AMP_MOTOR_DISABLED && !m.faults && !m.target_rpm);

    running(&m);
    f = feedback; f.current_ma = INT32_MIN; f.temperature_decic = 800;
    assert(!amp_motor_step(&m, 10, &f));
    assert(m.faults == (AMP_MOTOR_FAULT_CURRENT | AMP_MOTOR_FAULT_TEMPERATURE));
    assert(!amp_motor_step(&m, 10, &feedback));
    assert(m.state == AMP_MOTOR_FAULT); /* Healthy sample cannot auto-reset. */
    assert(amp_motor_command(&m, AMP_MOTOR_RESET_FAULT, 0, &f) == -2);

    running(&m);
    f = feedback; f.age_ms = 100;
    assert(!amp_motor_step(&m, 10, &f));
    assert(m.faults == AMP_MOTOR_FAULT_FEEDBACK);
    running(&m);
    assert(!amp_motor_step(&m, 10, NULL));
    assert(m.faults == AMP_MOTOR_FAULT_FEEDBACK);
    running(&m);
    assert(amp_motor_step(&m, 101, &feedback) == -1);
    assert(m.faults == AMP_MOTOR_FAULT_PERIOD && !amp_motor_demand_rpm(&m));

    /* Fractional demand accumulates even at 1 rpm/s, 1 ms per step. */
    bad = config; bad.acceleration_rpm_s = 1; bad.lease_timeout_ms = UINT32_MAX;
    assert(!amp_motor_init(&m, &bad));
    assert(!amp_motor_command(&m, AMP_MOTOR_ENABLE, 0, &feedback));
    assert(!amp_motor_command(&m, AMP_MOTOR_TARGET, 1, &feedback));
    assert(!amp_motor_command(&m, AMP_MOTOR_START, 0, &feedback));
    for (i = 0; i < 1000; ++i) assert(!amp_motor_step(&m, 1, &feedback));
    assert(amp_motor_demand_rpm(&m) == 1);
    puts("PASS: motor state, ramp/reversal, feedback stop, lease, fault latch/reset, boundaries");
    return 0;
}
