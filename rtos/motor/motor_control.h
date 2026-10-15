#ifndef AMP_MOTOR_CONTROL_H
#define AMP_MOTOR_CONTROL_H

#include <stdint.h>

/* Single-owner, integer-only supervisory logic. No FOC or hardware output. */
typedef enum {
    AMP_MOTOR_DISABLED, AMP_MOTOR_READY, AMP_MOTOR_RUNNING,
    AMP_MOTOR_STOPPING, AMP_MOTOR_FAULT
} AmpMotorState;

enum {
    AMP_MOTOR_FAULT_FEEDBACK = 1U,
    AMP_MOTOR_FAULT_CURRENT = 2U,
    AMP_MOTOR_FAULT_TEMPERATURE = 4U,
    AMP_MOTOR_FAULT_LEASE = 8U,
    AMP_MOTOR_FAULT_PERIOD = 16U
};

typedef struct {
    int32_t max_rpm;
    uint32_t acceleration_rpm_s, deceleration_rpm_s;
    uint32_t max_current_ma;
    int32_t max_temperature_decic;
    uint32_t feedback_timeout_ms, lease_timeout_ms;
    uint32_t stopped_rpm;
} AmpMotorConfig;

typedef struct {
    int32_t rpm, current_ma, temperature_decic;
    uint32_t age_ms;
    uint8_t valid;
} AmpMotorFeedback;

typedef enum {
    AMP_MOTOR_ENABLE, AMP_MOTOR_DISABLE, AMP_MOTOR_START,
    AMP_MOTOR_STOP, AMP_MOTOR_TARGET, AMP_MOTOR_HEARTBEAT,
    AMP_MOTOR_RESET_FAULT
} AmpMotorCommand;

typedef struct {
    AmpMotorConfig config;
    AmpMotorState state;
    uint32_t faults, lease_age_ms;
    int32_t target_rpm;
    int64_t demand_millirpm;
} AmpMotorControl;

/* 0 success, -1 argument/configuration error, -2 rejected state/feedback.
 * No mutation on rejected commands. Feedback must use consistent snapshots.
 * Runtime calls belong to one task; other tasks must submit queued commands. */
int amp_motor_init(AmpMotorControl *motor, const AmpMotorConfig *config);
int amp_motor_command(AmpMotorControl *motor, AmpMotorCommand command,
                      int32_t value, const AmpMotorFeedback *feedback);
/* dt_ms is actual elapsed time, 1..100 ms. Invalid periods fault an enabled
 * controller. Caller must separately diagnose timing while disabled. */
int amp_motor_step(AmpMotorControl *motor, uint32_t dt_ms,
                   const AmpMotorFeedback *feedback);
int32_t amp_motor_demand_rpm(const AmpMotorControl *motor);

#endif
