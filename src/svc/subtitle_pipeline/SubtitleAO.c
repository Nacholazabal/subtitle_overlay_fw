/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file SubtitleAO.c
/// @brief Subtitle pipeline orchestration active object
///

// === Headers files inclusions ==================================================================================== //

#include "qpc.h"

#include "app.h"
#include "log.h"
#include "SubtitleAO.h"
#include "subtitle_pipeline.h"

// === Macros definitions ========================================================================================== //

#define SUBTITLE_AO_DONE_WIDTH         (32)
#define SUBTITLE_AO_DONE_HEIGHT        (8)
#define SUBTITLE_AO_DONE_X             ((SUBTITLE_BRAM_MASK_WIDTH - SUBTITLE_AO_DONE_WIDTH) / 2)
#define SUBTITLE_AO_DONE_Y             ((SUBTITLE_BRAM_MASK_HEIGHT - SUBTITLE_AO_DONE_HEIGHT) / 2)
#define SUBTITLE_AO_DONE_BYTES_PER_ROW (SUBTITLE_AO_DONE_WIDTH / 8)

// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;

    subtitle_pipeline_t pipeline;
    uint8_t running;
} subtitle_ao_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState subtitle_ao_initial(subtitle_ao_t* const me, void const* const par);
static QState subtitle_ao_idle(subtitle_ao_t* const me, QEvt const* const e);
static QState subtitle_ao_ready(subtitle_ao_t* const me, QEvt const* const e);
static QState subtitle_ao_error(subtitle_ao_t* const me, QEvt const* const e);

static void post_ready(subtitle_ao_t* const me);
static void post_error(subtitle_ao_t* const me, int32_t code);
static int on_component_init(subtitle_ao_t* const me, component_init_evt_t const* const e);
static int on_subtitle_text(subtitle_ao_t* const me, subtitle_text_evt_t const* const e);
static int draw_startup_marker(subtitle_ao_t* const me);
static void enter_error(subtitle_ao_t* const me, int32_t code);

// === Private variable definitions ================================================================================ //

static subtitle_ao_t subtitle_ao_inst;

static uint8_t const done_bitmap[SUBTITLE_AO_DONE_HEIGHT][SUBTITLE_AO_DONE_BYTES_PER_ROW] = {
    {0xF0U, 0x70U, 0x88U, 0xF8U},
    {0x88U, 0x88U, 0xC8U, 0x80U},
    {0x84U, 0x88U, 0xA8U, 0x80U},
    {0x84U, 0x88U, 0x98U, 0xF0U},
    {0x84U, 0x88U, 0x88U, 0x80U},
    {0x88U, 0x88U, 0x88U, 0x80U},
    {0xF0U, 0x70U, 0x88U, 0xF8U},
    {0x00U, 0x00U, 0x00U, 0x00U},
};

// === Public variable definitions ================================================================================= //

QActive* const AO_Subtitle = Q_ACTIVE_UPCAST(&subtitle_ao_inst);

// === Private function implementation ============================================================================= //

/**
 * @brief Post a component-ready report to the system active object.
 * @param me Subtitle active object sending the report.
 * @return None.
 */
static void post_ready(subtitle_ao_t* const me)
{
    component_ready_evt_t* const ready_evt = Q_NEW(component_ready_evt_t, COMPONENT_READY_SIG);

    ready_evt->source = COMPONENT_SUBTITLE_PIPELINE;
    ready_evt->width = me->pipeline.display_width;
    ready_evt->height = me->pipeline.display_height;
    QACTIVE_POST(AO_System, &ready_evt->super, &me->super);
}

/**
 * @brief Post a component-error report to the system active object.
 * @param me Subtitle active object sending the report.
 * @param code Negative errorno_e value to include in the report.
 * @return None.
 */
static void post_error(subtitle_ao_t* const me, int32_t code)
{
    app_error_evt_t* const error_evt = Q_NEW(app_error_evt_t, COMPONENT_ERROR_SIG);

    Q_UNUSED_PAR(me);

    error_evt->source = COMPONENT_SUBTITLE_PIPELINE;
    error_evt->code = code;
    QACTIVE_POST(AO_System, &error_evt->super, &me->super);
}

/**
 * @brief Draw the temporary startup marker into subtitle BRAM.
 * @param me Subtitle active object owning the pipeline.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int draw_startup_marker(subtitle_ao_t* const me)
{
    int status;

    LOG_INFO("subtitle: drawing startup marker");

    status = subtitle_pipeline_clear(&me->pipeline);
    if (status != 0)
    {
        LOG_ERROR("subtitle: clear failed, code=%ld", (long)status);
        return status;
    }

    status = subtitle_pipeline_write_bitmap(&me->pipeline,
                                            &done_bitmap[0][0],
                                            SUBTITLE_AO_DONE_X,
                                            SUBTITLE_AO_DONE_Y,
                                            SUBTITLE_AO_DONE_WIDTH,
                                            SUBTITLE_AO_DONE_HEIGHT);
    if (status != 0)
    {
        LOG_ERROR("subtitle: bitmap write failed, code=%ld", (long)status);
        return status;
    }

    status = subtitle_pipeline_enable(&me->pipeline, 1U);
    if (status != 0)
    {
        LOG_ERROR("subtitle: enable failed, code=%ld", (long)status);
        return status;
    }

    return 0;
}

/**
 * @brief Initialize the subtitle pipeline and display a temporary DONE marker.
 * @param me Subtitle active object receiving COMPONENT_INIT_SIG.
 * @param e Initialization event carrying the active video dimensions.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int on_component_init(subtitle_ao_t* const me, component_init_evt_t const* const e)
{
    int status;

    if ((e == NULL) || (e->width == 0U) || (e->height == 0U))
    {
        status = -EINVAL;
    }
    else
    {
        LOG_INFO("subtitle: initializing pipeline for %lux%lu",
                 (unsigned long)e->width,
                 (unsigned long)e->height);

        status = subtitle_pipeline_init(&me->pipeline, e->width, e->height);
        if (status == 0)
        {
            status = draw_startup_marker(me);
        }
    }

    if (status == 0)
    {
        me->running = 1U;
        post_ready(me);
        LOG_INFO("subtitle: pipeline ready");
    }
    else
    {
        LOG_ERROR("subtitle: initialization failed, code=%ld", (long)status);
        enter_error(me, status);
    }

    return status;
}

/**
 * @brief Render one subtitle text event to the overlay.
 * @param me Subtitle active object owning the pipeline.
 * @param e Subtitle text event.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int on_subtitle_text(subtitle_ao_t* const me, subtitle_text_evt_t const* const e)
{
    int status;

    if ((e == NULL) || (e->text[0] == '\0'))
    {
        return -EINVAL;
    }

    LOG_INFO("subtitle: rendering %s seq=%lu text=\"%s\"",
             (e->is_final != 0U) ? "final" : "partial",
             (unsigned long)e->seq,
             e->text);

    status = subtitle_pipeline_clear(&me->pipeline);
    if (status != 0)
    {
        LOG_ERROR("subtitle: clear failed, code=%ld", (long)status);
        return status;
    }

    status = subtitle_pipeline_write_text(&me->pipeline, e->text);
    if (status != 0)
    {
        LOG_ERROR("subtitle: text render failed, code=%ld", (long)status);
        return status;
    }

    status = subtitle_pipeline_enable(&me->pipeline, 1U);
    if (status != 0)
    {
        LOG_ERROR("subtitle: enable failed, code=%ld", (long)status);
    }

    return status;
}

/**
 * @brief Clean up the subtitle pipeline and report a subtitle AO error.
 * @param me Subtitle active object entering its terminal error state.
 * @param code Negative errorno_e value to post to system_ao_t.
 * @return None.
 */
static void enter_error(subtitle_ao_t* const me, int32_t code)
{
    LOG_WARNING("subtitle: cleaning up after error code %ld", (long)code);
    subtitle_pipeline_cleanup(&me->pipeline);
    me->running = 0U;

    post_error(me, code);
}

/**
 * @brief Run the initial transition for subtitle_ao_t.
 * @param me Subtitle active object instance.
 * @param par Optional initial transition parameter supplied by QP/C.
 * @return QP/C transition result.
 */
static QState subtitle_ao_initial(subtitle_ao_t* const me, void const* const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    return Q_TRAN(&subtitle_ao_idle);
}

/**
 * @brief Handle component initialization before the subtitle pipeline is running.
 * @param me Subtitle active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState subtitle_ao_idle(subtitle_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case COMPONENT_INIT_SIG:
        if (on_component_init(me, Q_EVT_CAST(component_init_evt_t)) == 0)
        {
            status = Q_TRAN(&subtitle_ao_ready);
        }
        else
        {
            status = Q_TRAN(&subtitle_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Hold the initialized subtitle pipeline until future subtitle events arrive.
 * @param me Subtitle active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState subtitle_ao_ready(subtitle_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case SUBTITLE_TEXT_SIG:
        if (on_subtitle_text(me, Q_EVT_CAST(subtitle_text_evt_t)) == 0)
        {
            status = Q_HANDLED();
        }
        else
        {
            enter_error(me, -EIO);
            status = Q_TRAN(&subtitle_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Terminal state reached after subtitle pipeline initialization fails.
 * @param me Subtitle active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState subtitle_ao_error(subtitle_ao_t* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(e);

    return Q_SUPER(&QHsm_top);
}

// === Public function implementation ============================================================================== //

/**
 * @brief Construct the subtitle active object.
 * @param None.
 * @return None.
 */
void subtitle_ao_ctor(void)
{
    subtitle_ao_t* const me = &subtitle_ao_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&subtitle_ao_initial));
    me->running = 0U;
}

// === End of documentation ======================================================================================== //
