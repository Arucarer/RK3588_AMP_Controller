#ifndef AMP_MOTOR_SIM_H
#define AMP_MOTOR_SIM_H

#include "motor_control.h"

/* Deterministic behavioural model, not a PMSM electrical/FOC plant model. */
typedef struct {
    int32_t max_rpm, ambient_decic;
    uint32_t response_ms, thermal_ms;
    uint32_t idle_current_ma, max_current_ma, max_rise_decic;
} AmpMotorSimConfig;

enum {
    AMP_MOTOR_SIM_STALL = 1U,
    AMP_MOTOR_SIM_FEEDBACK_LOSS = 2U,
    AMP_MOTOR_SIM_OVERTEMPERATURE = 4U
};

typedef struct {
    AmpMotorSimConfig config;
    int64_t speed_millirpm, temperature_millidecic;
    uint32_t load_permille, injections;
    AmpMotorFeedback feedback;
} AmpMotorSim;

/* All operations have one owner. Config/state must not be directly mutated.
 * Rejected arguments leave the model unchanged. No hardware access. */
int amp_motor_sim_init(AmpMotorSim *sim, const AmpMotorSimConfig *config);
int amp_motor_sim_inject(AmpMotorSim *sim, uint32_t load_permille,
                         uint32_t injections);
/* enabled=0 removes simulated drive torque/current; speed coasts towards zero.
 * enabled=1 follows a bounded demand. dt_ms must be actual elapsed time 1..100.
 * Feedback loss freezes the last published snapshot and increases its age.
 * Overtemperature injects a synthetic sensor value, not actual heating. */
int amp_motor_sim_step(AmpMotorSim *sim, uint32_t dt_ms,
                       int32_t demand_rpm, int enabled);
int amp_motor_sim_feedback(const AmpMotorSim *sim, AmpMotorFeedback *feedback);
/* A scheduling gap is not a fresh measurement or evidence of physical stop. */
int amp_motor_sim_invalidate(AmpMotorSim *sim);

#endif
