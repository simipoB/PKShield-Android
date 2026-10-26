// Darwin stand-in (dm/android/compat): QoS classes map to nothing (Android schedules by its own hints)
#pragma once
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0 } qos_class_t;
typedef void *pthread_override_t;
static inline int pthread_set_qos_class_self_np(qos_class_t q, int p) { (void)q; (void)p; return 0; }
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *p) { (void)t; if (q) *q = QOS_CLASS_DEFAULT; if (p) *p = 0; return 0; }
static inline int pthread_attr_set_qos_class_np(void *a, qos_class_t q, int p) { (void)a; (void)q; (void)p; return 0; }
static inline pthread_override_t pthread_override_qos_class_start_np(pthread_t t, qos_class_t q, int p) { (void)t; (void)q; (void)p; return 0; }
static inline int pthread_override_qos_class_end_np(pthread_override_t o) { (void)o; return 0; }
static inline qos_class_t qos_class_self(void) { return QOS_CLASS_DEFAULT; }
