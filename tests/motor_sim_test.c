#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../rtos/motor/motor_sim.h"

static const AmpMotorSimConfig sim_config = {3000, 250, 100, 1000, 100, 6000, 800};
static const AmpMotorConfig control_config = {3000, 1000, 2000, 5000, 800, 100, 1000, 5};

static void cycle(AmpMotorControl *m, AmpMotorSim *s)
{
    AmpMotorFeedback f;
    assert(!amp_motor_sim_feedback(s, &f));
    assert(!amp_motor_step(m, 10, &f));
    assert(!amp_motor_sim_step(s, 10, amp_motor_demand_rpm(m),
        m->state == AMP_MOTOR_RUNNING || m->state == AMP_MOTOR_STOPPING));
}

static void start(AmpMotorControl *m, AmpMotorSim *s)
{
    AmpMotorFeedback f;
    assert(!amp_motor_init(m, &control_config));
    assert(!amp_motor_sim_init(s, &sim_config));
    assert(!amp_motor_sim_feedback(s, &f));
    assert(!amp_motor_command(m, AMP_MOTOR_ENABLE, 0, &f));
    assert(!amp_motor_command(m, AMP_MOTOR_TARGET, 1000, &f));
    assert(!amp_motor_command(m, AMP_MOTOR_START, 0, &f));
}

int main(void)
{
    AmpMotorSim s, twin;
    AmpMotorControl m;
    AmpMotorFeedback f, frozen;
    unsigned i;
    assert(!amp_motor_sim_init(&s, &sim_config));
    twin = s;
    assert(amp_motor_sim_step(&s, 0, 1000, 1) == -1);
    assert(amp_motor_sim_step(&s, 10, INT32_MIN, 1) == -1);
    assert(amp_motor_sim_inject(&s, 1001, 0) == -1);
    assert(amp_motor_sim_inject(&s, 0, 8) == -1);
    assert(!s.speed_millirpm && !s.injections);
    for (i = 0; i < 100; ++i) {
        assert(!amp_motor_sim_step(&s, 10, 1000, 1));
        assert(!amp_motor_sim_step(&twin, 10, 1000, 1));
        assert(s.speed_millirpm == twin.speed_millirpm);
        assert(s.temperature_millidecic == twin.temperature_millidecic);
    }
    assert(!amp_motor_sim_feedback(&s, &f));
    assert(f.rpm >= 999 && f.rpm <= 1000);
    assert(!amp_motor_sim_inject(&s, 500, 0));
    for (i = 0; i < 100; ++i) assert(!amp_motor_sim_step(&s, 10, 1000, 1));
    assert(!amp_motor_sim_feedback(&s, &f));
    assert(f.rpm >= 749 && f.rpm <= 751 && f.current_ma > 100);
    assert(f.temperature_decic > 250);
    for (i = 0; i < 1000; ++i) assert(!amp_motor_sim_step(&s, 10, 0, 0));
    assert(!amp_motor_sim_feedback(&s, &f));
    assert(f.rpm == 0 && f.current_ma == 0 && f.temperature_decic <= 251);

    assert(!amp_motor_sim_inject(&s, 0, AMP_MOTOR_SIM_FEEDBACK_LOSS));
    frozen = f;
    for (i = 0; i < 10; ++i) assert(!amp_motor_sim_step(&s, 10, -1000, 1));
    assert(!amp_motor_sim_feedback(&s, &f));
    assert(f.age_ms == 100 && f.rpm == frozen.rpm && f.current_ma == frozen.current_ma);
    assert(!amp_motor_sim_inject(&s, 0, 0));
    assert(!amp_motor_sim_step(&s, 10, -1000, 1));
    assert(!amp_motor_sim_feedback(&s, &f));
    assert(f.age_ms == 0 && f.rpm < 0);

    start(&m, &s);
    for (i = 0; i < 200; ++i) {
        if (i % 20 == 0) assert(!amp_motor_command(&m, AMP_MOTOR_HEARTBEAT, 0, NULL));
        cycle(&m, &s);
    }
    assert(m.state == AMP_MOTOR_RUNNING && s.feedback.rpm >= 999);
    assert(!amp_motor_command(&m, AMP_MOTOR_STOP, 0, &s.feedback));
    for (i = 0; i < 200; ++i) {
        assert(!amp_motor_command(&m, AMP_MOTOR_HEARTBEAT, 0, NULL));
        cycle(&m, &s);
    }
    assert(m.state == AMP_MOTOR_READY && s.feedback.rpm == 0);

    start(&m, &s);
    assert(!amp_motor_sim_inject(&s, 0, AMP_MOTOR_SIM_STALL));
    cycle(&m, &s); cycle(&m, &s);
    assert(m.state == AMP_MOTOR_FAULT && (m.faults & AMP_MOTOR_FAULT_CURRENT));
    assert(!amp_motor_demand_rpm(&m) && s.feedback.current_ma == 0);
    /* Physical-feedback recovery never automatically clears a latched fault. */
    assert(!amp_motor_sim_inject(&s, 0, 0));
    cycle(&m, &s);
    assert(m.state == AMP_MOTOR_FAULT);
    assert(!amp_motor_command(&m, AMP_MOTOR_RESET_FAULT, 0, &s.feedback));
    assert(m.state == AMP_MOTOR_DISABLED);

    start(&m, &s);
    assert(!amp_motor_sim_inject(&s, 0, AMP_MOTOR_SIM_OVERTEMPERATURE));
    cycle(&m, &s); cycle(&m, &s);
    assert(m.faults & AMP_MOTOR_FAULT_TEMPERATURE);
    assert(amp_motor_command(&m, AMP_MOTOR_RESET_FAULT, 0, &s.feedback) == -2);

    start(&m, &s);
    assert(!amp_motor_sim_inject(&s, 0, AMP_MOTOR_SIM_FEEDBACK_LOSS));
    for (i = 0; i < 11; ++i) cycle(&m, &s);
    assert(m.faults & AMP_MOTOR_FAULT_FEEDBACK);
    puts("PASS: SIM response/load/heating/cooling, determinism, feedback loss, control integration and faults");
    return 0;
}
