#include "motor_control.h"
#include <stddef.h>

static int64_t magnitude(int64_t value)
{
    return value < 0 ? -value : value;
}

static uint32_t feedback_faults(const AmpMotorControl *m,
                               const AmpMotorFeedback *f)
{
    uint32_t faults = 0;
    if (!f || !f->valid || f->age_ms >= m->config.feedback_timeout_ms)
        return AMP_MOTOR_FAULT_FEEDBACK;
    if (magnitude(f->current_ma) >= m->config.max_current_ma)
        faults |= AMP_MOTOR_FAULT_CURRENT;
    if (f->temperature_decic >= m->config.max_temperature_decic)
        faults |= AMP_MOTOR_FAULT_TEMPERATURE;
    return faults;
}

static void latch_fault(AmpMotorControl *m, uint32_t faults)
{
    m->faults |= faults;
    m->state = AMP_MOTOR_FAULT;
    m->target_rpm = 0;
    m->demand_millirpm = 0;
}

int amp_motor_init(AmpMotorControl *m, const AmpMotorConfig *c)
{
    if (!m || !c || c->max_rpm <= 0 || !c->acceleration_rpm_s ||
        !c->deceleration_rpm_s || !c->max_current_ma ||
        c->max_current_ma > INT32_MAX || c->max_temperature_decic <= 0 ||
        !c->feedback_timeout_ms || !c->lease_timeout_ms ||
        c->stopped_rpm >= (uint32_t)c->max_rpm)
        return -1;
    *m = (AmpMotorControl){0};
    m->config = *c;
    m->state = AMP_MOTOR_DISABLED;
    return 0;
}

int amp_motor_command(AmpMotorControl *m, AmpMotorCommand command,
                      int32_t value, const AmpMotorFeedback *f)
{
    if (!m || command < AMP_MOTOR_ENABLE || command > AMP_MOTOR_RESET_FAULT)
        return -1;
    if (command == AMP_MOTOR_DISABLE) {
        m->target_rpm = 0;
        m->demand_millirpm = 0;
        /* Disabling is always allowed but cannot clear a latched fault. */
        if (m->state != AMP_MOTOR_FAULT) m->state = AMP_MOTOR_DISABLED;
        return 0;
    }
    if (command == AMP_MOTOR_RESET_FAULT) {
        if (m->state != AMP_MOTOR_FAULT || feedback_faults(m, f) ||
            magnitude(f->rpm) > m->config.stopped_rpm)
            return -2;
        m->faults = 0;
        m->state = AMP_MOTOR_DISABLED;
        m->lease_age_ms = 0;
        return 0;
    }
    if (m->state == AMP_MOTOR_FAULT) return -2;
    switch (command) {
    case AMP_MOTOR_ENABLE:
        if (m->state != AMP_MOTOR_DISABLED || feedback_faults(m, f) ||
            magnitude(f->rpm) > m->config.stopped_rpm) return -2;
        m->state = AMP_MOTOR_READY;
        m->lease_age_ms = 0;
        break;
    case AMP_MOTOR_START:
        if (m->state != AMP_MOTOR_READY || feedback_faults(m, f)) return -2;
        m->state = AMP_MOTOR_RUNNING;
        m->lease_age_ms = 0;
        break;
    case AMP_MOTOR_STOP:
        if (m->state != AMP_MOTOR_RUNNING) return -2;
        m->target_rpm = 0;
        m->state = AMP_MOTOR_STOPPING;
        break;
    case AMP_MOTOR_TARGET:
        if (m->state != AMP_MOTOR_READY && m->state != AMP_MOTOR_RUNNING) return -2;
        if (magnitude(value) > m->config.max_rpm) return -1;
        m->target_rpm = value;
        break;
    case AMP_MOTOR_HEARTBEAT:
        if (m->state == AMP_MOTOR_DISABLED) return -2;
        m->lease_age_ms = 0;
        break;
    default: return -1;
    }
    return 0;
}

int amp_motor_step(AmpMotorControl *m, uint32_t dt,
                   const AmpMotorFeedback *f)
{
    uint32_t faults;
    int64_t goal, difference, amount;
    uint32_t rate;
    if (!m) return -1;
    if (!dt || dt > 100) {
        if (m->state != AMP_MOTOR_DISABLED)
            latch_fault(m, AMP_MOTOR_FAULT_PERIOD);
        return -1;
    }
    if (m->state == AMP_MOTOR_DISABLED) return 0;
    faults = feedback_faults(m, f);
    if (m->state == AMP_MOTOR_RUNNING || m->state == AMP_MOTOR_STOPPING) {
        uint32_t limit = m->config.lease_timeout_ms;
        m->lease_age_ms = dt >= limit - m->lease_age_ms ?
            limit : m->lease_age_ms + dt;
        if (m->lease_age_ms >= limit) faults |= AMP_MOTOR_FAULT_LEASE;
    }
    if (faults || m->state == AMP_MOTOR_FAULT) {
        latch_fault(m, faults);
        return 0;
    }
    if (m->state == AMP_MOTOR_READY) return 0;
    goal = (int64_t)m->target_rpm * 1000;
    /* Reverse direction by decelerating to zero before accelerating. */
    if ((goal > 0 && m->demand_millirpm < 0) ||
        (goal < 0 && m->demand_millirpm > 0)) goal = 0;
    rate = magnitude(goal) < magnitude(m->demand_millirpm) ?
        m->config.deceleration_rpm_s : m->config.acceleration_rpm_s;
    amount = (int64_t)rate * dt;
    difference = goal - m->demand_millirpm;
    if (magnitude(difference) <= amount) m->demand_millirpm = goal;
    else m->demand_millirpm += difference > 0 ? amount : -amount;
    if (m->state == AMP_MOTOR_STOPPING && !m->demand_millirpm &&
        magnitude(f->rpm) <= m->config.stopped_rpm) m->state = AMP_MOTOR_READY;
    return 0;
}

int32_t amp_motor_demand_rpm(const AmpMotorControl *m)
{
    return m ? (int32_t)(m->demand_millirpm / 1000) : 0;
}
