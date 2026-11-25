#include "theme/metrics.h"

static w_panel_metrics current = {button_size_default, panel_spacing_default};

bool w_theme_metrics_set(w_panel_metrics metrics) {
    if (metrics.button_size < button_size_min || metrics.button_size > button_size_max ||
        metrics.spacing < panel_spacing_min || metrics.spacing > panel_spacing_max ||
        (current.button_size == metrics.button_size && current.spacing == metrics.spacing))
        return false;
    current = metrics;
    return true;
}

int w_theme_button_size(UINT dpi) {
    return MulDiv(current.button_size, dpi ? dpi : 96, 96);
}

int w_theme_icon_size(UINT dpi) {
    return max(1, MulDiv(w_theme_button_size(dpi), 17, 32));
}

int w_theme_spacing(UINT dpi) {
    return MulDiv(current.spacing, dpi ? dpi : 96, 96);
}
