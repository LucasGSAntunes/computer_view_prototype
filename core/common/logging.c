#include "logging.h"

VPLogLevel g_vp_log_level = VP_LOG_INFO;

void vp_log_set_level(VPLogLevel level) {
    g_vp_log_level = level;
}
