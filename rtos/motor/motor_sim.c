#include "motor_sim.h"
#include <stddef.h>

static int64_t absolute(int64_t value)
{
    return value < 0 ? -value : value;
}

int amp_motor_sim_init(AmpMotorSim *s, const AmpMotorSimConfig *c)
{
    if (!s || !c || c->max_rpm <= 0 || !c->response_ms || !c->thermal_ms ||
        !c->max_current_ma || c->max_current_ma > INT32_MAX ||
        c->idle_current_ma > c->max_current_ma || c->ambient_decic < -1000 ||
        c->ambient_decic > 2000 || !c->max_rise_decic || c->max_rise_decic > 2000)
        return -1;
    *s = (AmpMotorSim){0};
    s->config = *c;
    s->temperature_millidecic = (int64_t)c->ambient_decic * 1000;
    s->feedback.temperature_decic = c->ambient_decic;
    s->feedback.valid = 1;
    return 0;
}

int amp_motor_sim_inject(AmpMotorSim *s, uint32_t load, uint32_t injections)
{
    if (!s || load > 1000 || (injections & ~7U)) return -1;
    s->load_permille = load;
    s->injections = injections;
    return 0;
}

int amp_motor_sim_step(AmpMotorSim *s, uint32_t dt, int32_t demand, int enabled)
{
    int64_t goal, current, effort, equilibrium;
    if (!s || !dt || dt > 100 || (enabled != 0 && enabled != 1) ||
        absolute(demand) > s->config.max_rpm) return -1;

    goal = enabled ? (int64_t)demand * 1000 : 0;
    /* Intentional speed droop under load. A stall overrides inertia. */
    goal = goal * (1000 - s->load_permille / 2) / 1000;
    if (s->injections & AMP_MOTOR_SIM_STALL) s->speed_millirpm = 0;
    else s->speed_millirpm += (goal - s->speed_millirpm) * dt /
        ((int64_t)s->config.response_ms + dt);

    current = 0;
    if (enabled) {
        effort = absolute((int64_t)demand * 1000 - s->speed_millirpm) /
            s->config.max_rpm + s->load_permille;
        if ((s->injections & AMP_MOTOR_SIM_STALL) && demand) effort = 1000;
        if (effort > 1000) effort = 1000;
        current = s->config.idle_current_ma +
            ((int64_t)s->config.max_current_ma - s->config.idle_current_ma) * effort / 1000;
    }
    equilibrium = (int64_t)s->config.ambient_decic * 1000 +
        (int64_t)s->config.max_rise_decic * 1000 * current / s->config.max_current_ma;
    s->temperature_millidecic += (equilibrium - s->temperature_millidecic) * dt /
        ((int64_t)s->config.thermal_ms + dt);

    if (s->injections & AMP_MOTOR_SIM_FEEDBACK_LOSS) {
        uint32_t age = s->feedback.age_ms;
        s->feedback.age_ms = dt > UINT32_MAX - age ? UINT32_MAX : age + dt;
        return 0;
    }
    s->feedback.rpm = (int32_t)(s->speed_millirpm / 1000);
    s->feedback.current_ma = (int32_t)current;
    s->feedback.temperature_decic = (s->injections & AMP_MOTOR_SIM_OVERTEMPERATURE) ?
        s->config.ambient_decic + (int32_t)s->config.max_rise_decic :
        (int32_t)(s->temperature_millidecic / 1000);
    s->feedback.age_ms = 0;
    s->feedback.valid = 1;
    return 0;
}

int amp_motor_sim_feedback(const AmpMotorSim *s, AmpMotorFeedback *f)
{
    if (!s || !f) return -1;
    *f = s->feedback;
    return 0;
}
