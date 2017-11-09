/*
 * Copyright © 2015 Christian Storm <Christian.Storm at tngtech dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <stdbool.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/screensaver.h>
#include <xcb/dpms.h>
#include <xcb/randr.h>


#ifdef DEBUGLOG
    #define DEBUG(...) do {                                       \
        (void)fprintf(stderr, "%s", gs_color.green);              \
        (void)fprintf(stderr, "["PROGNAME"::DEBUG]" __VA_ARGS__); \
        (void)fprintf(stderr, "%s", gs_color.reset);              \
    } while (0)
#else
    #define DEBUG(...) {}
#endif
#ifdef TRACELOG
    #define TRACE(...) do {                                       \
        (void)fprintf(stderr, "%s", gs_color.gray);               \
        (void)fprintf(stderr, "["PROGNAME"::TRACE]" __VA_ARGS__); \
        (void)fprintf(stderr, "%s", gs_color.reset);              \
    } while (0)
#else
    #define TRACE(...) {}
#endif
#define ERROR(...) do {                                 \
    (void)fprintf(stderr, "%s", gs_color.red);          \
    (void)fprintf(stderr, "["PROGNAME"] " __VA_ARGS__); \
    (void)fprintf(stderr, "%s", gs_color.reset);        \
} while (0)
#define WARN(...)  do {                                 \
    (void)fprintf(stderr, "%s", gs_color.yellow);       \
    (void)fprintf(stderr, "["PROGNAME"] " __VA_ARGS__); \
    (void)fprintf(stderr, "%s", gs_color.reset);        \
} while (0)


#ifndef USE_SYSFS_BACKLIGHT_CONTROL
#define CC_IGNORE_WARNING_CAST_ALIGN                     \
    _Pragma("clang diagnostic push"                    ) \
    _Pragma("clang diagnostic ignored \"-Wcast-align\"") \
    _Pragma("GCC diagnostic push"                      ) \
    _Pragma("GCC diagnostic ignored \"-Wcast-align\""  )

#define CC_RESTORE_WARNINGS          \
    _Pragma("clang diagnostic push") \
    _Pragma("GCC diagnostic push"  )
#endif


#define NO_BRIGHTNESS -1
#define BRN_PRIORSCRSVR_UNDEFINED 0xff
#define RET_OK 0

///////////////////////////////////////////////////////////////////////////////
// configuration
///////////////////////////////////////////////////////////////////////////////
static uint8_t DIM_PERCENT_INTERVAL = 20;
static uint8_t DIM_PERCENT_TIMEOUT = 40;


///////////////////////////////////////////////////////////////////////////////
// types
///////////////////////////////////////////////////////////////////////////////
typedef enum {
    STATE_DPMS_STANDBY,
    STATE_DPMS_SUSPEND,
    STATE_DPMS_OFF,
    STATE_SCREENSAVER_ON_TIMEOUT,
    STATE_SCREENSAVER_ON_INTERVAL,
    STATE_SCREENSAVER_OFF,
    STATE_SCREENSAVER_CYCLE,
    STATE_SCREENSAVER_DISABLED,
    STATE_UNKNOWN,
} state_t;

static struct Tcolor {
    char* yellow;
    char* red;
    char* gray;
    char* green;
    char* reset;
} gs_color = {
    .yellow = "\x1b[33m",
    .red    = "\x1b[1;31m",
    .gray   = "\x1b[1;30m",
    .green  = "\x1b[32m",
    .reset  = "\x1b[0m"
};

static struct Tglobalstate {
    xcb_window_t  screensaver_window;
    uint16_t      screensaver_timeout;
    uint16_t      screensaver_interval;
    uint16_t      dpms_standby_timeout;
    uint16_t      dpms_suspend_timeout;
    uint16_t      dpms_off_timeout;
    uint16_t      dpms_power_level;
    uint32_t      screensaver_idlesecuser;
    uint32_t      screensaver_idlesecserver;
    uint8_t       state;
    uint8_t       screensaver_blanking;
    uint8_t       screensaver_allow_exposures;
    uint8_t       screensaver_state;
    uint8_t       screensaver_kind;
    uint8_t       dpms_state;
    char         _padding[2];
} gs_globalstate;

static struct Teventstate {
    uint8_t  brn_cur_perc;
    uint8_t  brn_old_perc;
    uint8_t  brn_priorscrsvr_perc;
    bool     brn_interval_set;
} gs_eventstate = {
    .brn_cur_perc         = 0,
    .brn_old_perc         = 0,
    .brn_priorscrsvr_perc = BRN_PRIORSCRSVR_UNDEFINED,
    .brn_interval_set     = false,
};

static struct Txcb {
    xcb_connection_t        *connection;
    xcb_screen_t            *screen;
    xcb_window_t             window;
    xcb_pixmap_t             pixmap;
    xcb_atom_t               backlight_atom;
    xcb_atom_t               backlight_new_atom;
    xcb_atom_t               backlight_legacy_atom;
    int                      screen_nr;
    xcb_intern_atom_reply_t *screensaver_id_atom;
    uint8_t                  screensaver_id;
    char                     _padding[7];
} gs_xcb = {
    .connection            = NULL,
    .screen                = NULL,
    .screen_nr             = 0,
    .window                = 0,
    .pixmap                = 0,
    .screensaver_id_atom   = NULL,
    .screensaver_id        = 0,
    .backlight_atom        = 0,
    .backlight_new_atom    = 0,
    .backlight_legacy_atom = 0
};

typedef enum {
    OPERATION_GETBRIGHTNESS,
    OPERATION_SETBRIGHTNESS,
    OPERATION_INCBRIGHTNESS,
    OPERATION_DECBRIGHTNESS
} operations_t;

typedef enum {
    OPERATION_SHUTDOWN_CONN,
    OPERATION_SHUTDOWN_DEREGEVENT
} setup_operations_t;


///////////////////////////////////////////////////////////////////////////////
// forward declarations
///////////////////////////////////////////////////////////////////////////////
static void print_usage(void);
static void signal_handler(const int sig) __attribute__((noreturn));
static inline bool operation_handler(const operations_t operation, struct Txcb *pxcb, const uint8_t brn_percent, uint8_t *brn_cur_perc, uint8_t *brn_new_perc) __attribute__((always_inline));
void shutdown(const setup_operations_t operation);
bool query_state(struct Tglobalstate *state, const struct Txcb *pxcb);
bool query_state_screensaver(struct Tglobalstate *pglobalstate, const struct Txcb *pxcb);
bool query_state_dpms(struct Tglobalstate *pglobalstate, const struct Txcb *pxcb);
static int parse_uint8_t(char* input, uint8_t* output);
static int parse_args(int len, char** args);
#ifndef USE_SYSFS_BACKLIGHT_CONTROL
bool _operation_handler_randr(const operations_t operation, struct Txcb *pxcb, const uint8_t brn_percent, uint8_t *brn_cur_perc, uint8_t *brn_new_perc);
int32_t _get_brightness_randr(struct Txcb *pxcb, const xcb_randr_output_t output, const xcb_atom_t *backlight_atom);
int32_t get_brightness_randr(struct Txcb *pxcb, const xcb_randr_output_t output);
int8_t set_brightness_randr(const struct Txcb *pxcb, xcb_randr_output_t output, int32_t value);
#endif
#ifdef USE_SYSFS_BACKLIGHT_CONTROL
bool _operation_handler_file(const operations_t operation, const uint8_t brn_percent, uint8_t *brn_cur_perc, uint8_t *brn_new_perc);
int32_t get_brightness_file(const char* filename);
int8_t set_brightness_file(const char* filename, const int32_t value_abs);
static inline bool is_file_accessible(const char* filename, const int mode) __attribute__((always_inline));
#endif


///////////////////////////////////////////////////////////////////////////////
// is_file_accessible()
///////////////////////////////////////////////////////////////////////////////
/** Test whether a given file is accessible with a given access mode.

    @param filename         the absolute path to the file to be tested
    @param mode             R_OK, W_OK, X_OK, or F_OK
    @return                 true on success or false on failure

    @see man 3P access
*/
#ifdef USE_SYSFS_BACKLIGHT_CONTROL
static inline bool is_file_accessible(const char* filename, const int mode) {
    if (access(filename, mode) == -1) {
        ERROR("Error: cannot access file %s: %s", filename, strerror(errno) );
        return false;
    }
    return true;
}
#endif


///////////////////////////////////////////////////////////////////////////////
// shutdown()
///////////////////////////////////////////////////////////////////////////////
/** Perform cleanup and shutdown operations.

    @param operation        which shutdown operation to perform

    @see setup_operations_t
*/
void shutdown(const setup_operations_t operation) {
    switch(operation) {
        case OPERATION_SHUTDOWN_CONN:
            if (xcb_connection_has_error(gs_xcb.connection) > 0) {
                ERROR("Error: xcb connection error while releasing xcb connection\n");
                return;
            }
            DEBUG("[shutdown] releasing xcb connection\n");
            (void)xcb_screensaver_unset_attributes(gs_xcb.connection, gs_xcb.screen->root);
            if (gs_xcb.pixmap != 0) {
                (void)xcb_free_pixmap(gs_xcb.connection, gs_xcb.pixmap);
            }
            if (gs_xcb.screensaver_id_atom) {
                xcb_delete_property(gs_xcb.connection, gs_xcb.screen->root, gs_xcb.screensaver_id_atom->atom);
                free(gs_xcb.screensaver_id_atom);
            }
            (void)xcb_flush(gs_xcb.connection);
            xcb_disconnect(gs_xcb.connection);
            (void)unsetenv("XSS_WINDOW");
            (void)unsetenv("XSCREENSAVER_WINDOW");
            return;
        case OPERATION_SHUTDOWN_DEREGEVENT:
            if (xcb_connection_has_error(gs_xcb.connection) > 0) {
                ERROR("Error: xcb connection error while de-registering from screensaver events\n");
                return;
            }
            DEBUG("[shutdown] unsubscribing from screensaver events\n");
            (void)xcb_screensaver_select_input(gs_xcb.connection, gs_xcb.screen->root, 0);
            return;
    }
}
// callables for atexit() registration wrapping shutdown() with the appropriate operation arguments
static void shutdown_connection()        { shutdown(OPERATION_SHUTDOWN_CONN);       }
static void shutdown_deregister_events() { shutdown(OPERATION_SHUTDOWN_DEREGEVENT); }


///////////////////////////////////////////////////////////////////////////////
// query_state_screensaver()
///////////////////////////////////////////////////////////////////////////////
/** Query the current screensaver state.

    @param pglobalstate     state container struct
    @param pxcb             xcb container struct
    @return                 true on successful screensaver query, false otherwise

    @see Tglobalstate
    @see Txcb
*/
bool query_state_screensaver(struct Tglobalstate *pglobalstate, const struct Txcb *pxcb) {
    xcb_screensaver_query_info_cookie_t  screensaver_query_info_cookie;
    xcb_screensaver_query_info_reply_t  *screensaver_query_info_reply;
    screensaver_query_info_cookie = xcb_screensaver_query_info(pxcb->connection, pxcb->screen->root);
    screensaver_query_info_reply  = xcb_screensaver_query_info_reply(pxcb->connection, screensaver_query_info_cookie, NULL);
    if (!screensaver_query_info_reply) { return false; }

    pglobalstate->screensaver_idlesecuser   = screensaver_query_info_reply->ms_since_user_input / 1000;
    pglobalstate->screensaver_idlesecserver = screensaver_query_info_reply->ms_until_server / 1000;
    pglobalstate->screensaver_state         = screensaver_query_info_reply->state;
    pglobalstate->screensaver_kind          = screensaver_query_info_reply->kind;
    pglobalstate->screensaver_window        = screensaver_query_info_reply->saver_window;
    free(screensaver_query_info_reply);

    xcb_get_screen_saver_reply_t    *get_screensaver_reply;
    xcb_get_screen_saver_cookie_t    get_screen_saver_cookie;
    get_screen_saver_cookie = xcb_get_screen_saver(pxcb->connection);
    get_screensaver_reply   = xcb_get_screen_saver_reply(pxcb->connection, get_screen_saver_cookie, NULL);
    if (!get_screensaver_reply) { return false; }

    pglobalstate->screensaver_timeout         = get_screensaver_reply->timeout;
    pglobalstate->screensaver_interval        = get_screensaver_reply->interval;
    pglobalstate->screensaver_blanking        = get_screensaver_reply->prefer_blanking;
    pglobalstate->screensaver_allow_exposures = get_screensaver_reply->allow_exposures;
    free(get_screensaver_reply);

    TRACE("[query_state] scrsvr :: timeout=%us interval=%us idlesecUser=%ds idlesecSrv=%ds\n",
        pglobalstate->screensaver_timeout,
        pglobalstate->screensaver_interval,
        pglobalstate->screensaver_idlesecuser,
        pglobalstate->screensaver_idlesecserver
    );
    TRACE("[query_state] scrsvr :: blank=%s allow_exposure=%s kind=%s%s%s\n",
        pglobalstate->screensaver_blanking        ? "yes" : "no",
        pglobalstate->screensaver_allow_exposures ? "yes" : "no",
        pglobalstate->screensaver_kind == 0 ? "blanked"  : "",
        pglobalstate->screensaver_kind == 1 ? "internal" : "",
        pglobalstate->screensaver_kind == 2 ? "external" : ""
    );
    if (!pglobalstate->screensaver_blanking) {
        WARN("Warning: screensaver's prefer blanking mode is not enabled, blanking won't kick in!\n");
    }
    return true;
}


///////////////////////////////////////////////////////////////////////////////
// query_state_dpms()
///////////////////////////////////////////////////////////////////////////////
/** Query the current dpms state.

    @param pglobalstate     state container struct
    @param pxcb             xcb container struct
    @return                 true on successful dpms query, false otherwise

    @see Tglobalstate
    @see Txcb
*/
bool query_state_dpms(struct Tglobalstate *pglobalstate, const struct Txcb *pxcb) {
    xcb_dpms_get_timeouts_cookie_t  dpms_get_timeouts_cookie;
    xcb_dpms_get_timeouts_reply_t  *dpms_get_timeouts_reply;
    xcb_generic_error_t            *error = NULL;

    dpms_get_timeouts_cookie = xcb_dpms_get_timeouts_unchecked(pxcb->connection);
    dpms_get_timeouts_reply  = xcb_dpms_get_timeouts_reply(pxcb->connection, dpms_get_timeouts_cookie, &error);
    if (!dpms_get_timeouts_reply) { return false; }

    pglobalstate->dpms_standby_timeout = dpms_get_timeouts_reply->standby_timeout;
    pglobalstate->dpms_suspend_timeout = dpms_get_timeouts_reply->suspend_timeout;
    pglobalstate->dpms_off_timeout     = dpms_get_timeouts_reply->off_timeout;
    free(dpms_get_timeouts_reply);

    xcb_dpms_info_cookie_t  dpms_info_cookie;
    xcb_dpms_info_reply_t  *dpms_info_reply;
    dpms_info_cookie = xcb_dpms_info(pxcb->connection);
    dpms_info_reply  = xcb_dpms_info_reply(pxcb->connection, dpms_info_cookie, &error);
    if (!dpms_info_reply) { return false; }

    pglobalstate->dpms_state       = dpms_info_reply->state;
    pglobalstate->dpms_power_level = dpms_info_reply->power_level;
    free(dpms_info_reply);

    if (pglobalstate->dpms_standby_timeout == 0) {
        WARN("Warning: dpms's standby timeout is 0 (=disabled), won't go into dpms standby mode!\n");
    }
    if (pglobalstate->dpms_standby_timeout == 0) {
        WARN("Warning: dpms's suspend timeout is 0 (=disabled), won't go into dpms suspend mode!\n");
    }
    if (pglobalstate->dpms_standby_timeout == 0) {
        WARN("Warning: dpms's off timeout is 0 (=disabled), won't go into dpms off mode!\n");
    }

    TRACE("[query_state] dpms   :: status=%s standby=%us suspend=%us off=%us\n",
        pglobalstate->dpms_state ? "on" : "off",
        pglobalstate->dpms_standby_timeout,
        pglobalstate->dpms_suspend_timeout,
        pglobalstate->dpms_off_timeout
    );
    return true;
}


///////////////////////////////////////////////////////////////////////////////
// query_state()
///////////////////////////////////////////////////////////////////////////////
/** Aggregate the current screensaver and dpms state.

    @param pglobalstate     state container struct
    @param pxcb             xcb container struct
    @return                 true on successful query aggregation, false otherwise

    @see Tglobalstate
    @see Txcb
    @see query_state_dpms
    @see query_state_screensaver
*/
bool query_state(struct Tglobalstate *pglobalstate, const struct Txcb *pxcb) {
    if (xcb_connection_has_error(pxcb->connection) > 0) {
        ERROR("Error: xcb connection error while querying screensaver and dpms state\n");
        return false;
    }

    if (!query_state_dpms(pglobalstate, pxcb)       ) { return false; }
    if (!query_state_screensaver(pglobalstate, pxcb)) { return false; }

    #define SET_STATE(STATE)                             \
        do {                                             \
            pglobalstate->state = STATE;                 \
            TRACE("[query_state] state  :: "#STATE"\n"); \
        } while (0)

    switch (pglobalstate->screensaver_state) {
        case XCB_SCREENSAVER_STATE_OFF:
            SET_STATE(STATE_SCREENSAVER_OFF); break;
        case XCB_SCREENSAVER_STATE_ON:
            switch (pglobalstate->dpms_power_level) {
                case XCB_DPMS_DPMS_MODE_ON:
                    if (pglobalstate->screensaver_idlesecuser == pglobalstate->screensaver_timeout) {
                        SET_STATE(STATE_SCREENSAVER_ON_TIMEOUT);
                    } else {
                        SET_STATE(STATE_SCREENSAVER_ON_INTERVAL);
                    }
                    break;
                case XCB_DPMS_DPMS_MODE_STANDBY:
                    SET_STATE(STATE_DPMS_STANDBY); break;
                case XCB_DPMS_DPMS_MODE_SUSPEND:
                    SET_STATE(STATE_DPMS_SUSPEND); break;
                case XCB_DPMS_DPMS_MODE_OFF:
                    SET_STATE(STATE_DPMS_OFF); break;
                default:
                    SET_STATE(STATE_UNKNOWN); break;
            }
            break;
        case XCB_SCREENSAVER_STATE_CYCLE:
            SET_STATE(STATE_SCREENSAVER_CYCLE); break;
        case XCB_SCREENSAVER_STATE_DISABLED:
            SET_STATE(STATE_SCREENSAVER_DISABLED); break;
        default:
            SET_STATE(STATE_UNKNOWN); break;
    }

    #undef SET_STATE

    return true;
}


///////////////////////////////////////////////////////////////////////////////
// _get_brightness_randr()
///////////////////////////////////////////////////////////////////////////////
/** Helper function returning the device-specific brightness of a given output.

    @param pxcb             xcb container struct
    @param output           the output to get the brightness for
    @param backlight_atom   the backlight property atom to read the brightness from
    @return                 the *absolute* brightness value in the output's device-specific range, or NO_BRIGHTNESS on error

    @see Txcb
    @see NO_BRIGHTNESS
    @see get_brightness_randr
*/
#ifndef USE_SYSFS_BACKLIGHT_CONTROL
int32_t _get_brightness_randr(struct Txcb *pxcb, const xcb_randr_output_t output, const xcb_atom_t *backlight_atom) {
    xcb_randr_get_output_property_reply_t *output_poperty_reply = NULL;
    xcb_randr_get_output_property_cookie_t output_poperty_cookie;
    xcb_generic_error_t *error = NULL;

    pxcb->backlight_atom = *backlight_atom;
    if (pxcb->backlight_atom != XCB_ATOM_NONE) {
        output_poperty_cookie = xcb_randr_get_output_property(pxcb->connection, output, pxcb->backlight_atom, XCB_ATOM_NONE, 0, 4, 0, 0);
        output_poperty_reply  = xcb_randr_get_output_property_reply(pxcb->connection, output_poperty_cookie, &error);
        if (error != NULL || output_poperty_reply == NULL) {
            TRACE("[get_brightness_randr] error %u while querying brightness of output %d on backlight %d\n", error->error_code, output, pxcb->backlight_atom);
            return NO_BRIGHTNESS;
        }
        if (output_poperty_reply->type != XCB_ATOM_INTEGER || output_poperty_reply->num_items != 1 || output_poperty_reply->format != 32) {
            free(output_poperty_reply);
            return NO_BRIGHTNESS;
        }
        CC_IGNORE_WARNING_CAST_ALIGN
        int32_t value_abs = *((int32_t *) xcb_randr_get_output_property_data(output_poperty_reply));
        CC_RESTORE_WARNINGS
        free(output_poperty_reply);
        TRACE("[get_brightness_randr] brightness_abs=%d [output: %d][backlight: %d]\n", value_abs, output, pxcb->backlight_atom);
        return value_abs;
    }
    TRACE("[get_brightness_randr] backlight is XCB_ATOM_NONE [output: %d][backlight: %d]\n", output, pxcb->backlight_atom);
    return NO_BRIGHTNESS;
}
#endif


///////////////////////////////////////////////////////////////////////////////
// get_brightness_randr()
///////////////////////////////////////////////////////////////////////////////
/** Get the brightness of a given output as device-specific absolute value.

    @param pxcb             xcb container struct
    @param output           the output to get the brightness for
    @return                 the *absolute* brightness value in the output's device-specific range, or NO_BRIGHTNESS on error

    @see Txcb
    @see NO_BRIGHTNESS
    @see _get_brightness_randr
*/
#ifndef USE_SYSFS_BACKLIGHT_CONTROL
int32_t get_brightness_randr(struct Txcb *pxcb, const xcb_randr_output_t output) {
    int32_t value = _get_brightness_randr(pxcb, output, &pxcb->backlight_new_atom);
    if (value == NO_BRIGHTNESS) {
        value = _get_brightness_randr(pxcb, output, &pxcb->backlight_legacy_atom);
    }
    return value;
}
#endif


///////////////////////////////////////////////////////////////////////////////
// set_brightness_randr()
///////////////////////////////////////////////////////////////////////////////
/** Set the brightness of a given output to a device-specific absolute value.

    @param pxcb             xcb container struct
    @param output           the output to set the brightness for
    @param value_abs        the *absolute* brightness value in the output's device-specific range
    @return                 RET_OK, or NO_BRIGHTNESS on error

    @see Txcb
    @see NO_BRIGHTNESS
    @see RET_OK
*/
#ifndef USE_SYSFS_BACKLIGHT_CONTROL
int8_t set_brightness_randr(const struct Txcb *pxcb, const xcb_randr_output_t output, int32_t value_abs) {
    xcb_void_cookie_t    xcb_void_cookie;
    xcb_generic_error_t *xcb_generic_error;
    TRACE("[set_brightness_randr] setting brightness_abs to %d [output: %d]\n", value_abs, output);
    xcb_void_cookie = xcb_randr_change_output_property(pxcb->connection, output, pxcb->backlight_atom, XCB_ATOM_INTEGER, 32, XCB_PROP_MODE_REPLACE, 1, (unsigned char *)&value_abs);
    if ( (xcb_generic_error = xcb_request_check(pxcb->connection, xcb_void_cookie)) ) {
        ERROR("Error: cannot set brightness. Exiting.\n");
        return NO_BRIGHTNESS;
    }
    return RET_OK;
}
#endif


///////////////////////////////////////////////////////////////////////////////
// get_brightness_file()
///////////////////////////////////////////////////////////////////////////////
/** Get the brightness of an output from a file as device-specific absolute value.

    @param filename         the absolute path to the file the brightness value is read from
    @return                 the *absolute* brightness value in the output's device-specific range, or NO_BRIGHTNESS on error

    @see NO_BRIGHTNESS
*/
#ifdef USE_SYSFS_BACKLIGHT_CONTROL
int32_t get_brightness_file(const char* filename) {
	FILE *file;
	int32_t brightness;
	if ((file = fopen(filename, "r"))) {
		if (EOF == fscanf(file, "%u", &brightness)) {
            ERROR("Error: cannot read file %s (%s)\n", filename, strerror(errno));
            return NO_BRIGHTNESS;
        }
		if (fclose(file) != 0) {
            ERROR("Error: cannot close file %s (%s)\n", filename, strerror(errno));
            return NO_BRIGHTNESS;
        }
        TRACE("[get_brightness_file] brightness_abs=%d\n", brightness);
		return brightness;
	}
	ERROR("Error: cannot open file %s for reading (%s)\n", filename, strerror(errno));
	return NO_BRIGHTNESS;
}
#endif


///////////////////////////////////////////////////////////////////////////////
// set_brightness_file()
///////////////////////////////////////////////////////////////////////////////
/** Set the brightness of an output by a file to a device-specific absolute value.

    @param filename         the absolute path to the file the brightness value is read from
    @param value_abs        the *absolute* brightness value in the output's device-specific range
    @return                 RET_OK, or NO_BRIGHTNESS on error

    @see NO_BRIGHTNESS
    @see RET_OK
*/
#ifdef USE_SYSFS_BACKLIGHT_CONTROL
int8_t set_brightness_file(const char* filename, const int32_t value_abs) {
	FILE *file;
	if ((file = fopen(filename, "w"))) {
        int8_t retcode = RET_OK;
		if (fprintf(file, "%u", value_abs) < 0) {
            ERROR("Error: cannot write file %s (%s)\n", filename, strerror(errno));
            retcode = NO_BRIGHTNESS;
        }
		if (fclose(file) != 0) {
            ERROR("Error: cannot close file %s (%s)\n", filename, strerror(errno));
            retcode = NO_BRIGHTNESS;
        }
        return retcode;
	}
	ERROR("Error: cannot open file %s for writing (%s)", filename, strerror(errno));
    return NO_BRIGHTNESS;
}
#endif


///////////////////////////////////////////////////////////////////////////////
// _operation_handler_randr()
///////////////////////////////////////////////////////////////////////////////
/** Provides set/get/increase/decrease brightness operations using xrandr.

    @param operation        the brightness operation to perform
    @param pxcb             the global xcb container struct
    @param brn_percent      brightness percentage to set/increase/decrease depending on `operation`
    @param brn_cur_perc     the current brightness as percentage
    @param brn_new_perc     the new brightness as percentage
    @return                 true if operation could be performed, false on an unrecoverable error

    @see operations_t
    @see Txcb
*/
#ifndef USE_SYSFS_BACKLIGHT_CONTROL
bool _operation_handler_randr(const operations_t operation, struct Txcb *pxcb, const uint8_t brn_percent, uint8_t *brn_cur_perc, uint8_t *brn_new_perc) {
    bool output_found = false;
    xcb_generic_error_t *error;
    xcb_randr_output_t  *outputs;
    xcb_randr_get_screen_resources_reply_t *resources_reply;
    xcb_randr_get_screen_resources_cookie_t resources_cookie;

    resources_cookie = xcb_randr_get_screen_resources(pxcb->connection, pxcb->screen->root);
    resources_reply  = xcb_randr_get_screen_resources_reply(pxcb->connection, resources_cookie, &error);
    if (error != NULL || resources_reply == NULL) {
        ERROR("Error: randr Get Screen Resources returned error %d\n", error ? error->error_code : -1);
        return false;
    }

    outputs = xcb_randr_get_screen_resources_outputs(resources_reply);
    for (uint16_t o = 0; o < resources_reply->num_outputs; o++) {
        int32_t brn_cur_abs = get_brightness_randr(pxcb, outputs[o]);
        if (brn_cur_abs != NO_BRIGHTNESS) {
            output_found = true;

            xcb_randr_query_output_property_cookie_t prop_cookie;
            xcb_randr_query_output_property_reply_t *prop_reply;

            prop_cookie = xcb_randr_query_output_property(pxcb->connection, outputs[o], pxcb->backlight_atom);
            prop_reply  = xcb_randr_query_output_property_reply(pxcb->connection, prop_cookie, &error);

            if (error != NULL || prop_reply == NULL) {
                TRACE("[operation_handler] error %u while querying output property, continuing to next display\n", error->error_code);
                continue;
            }

            if (prop_reply->range && xcb_randr_query_output_property_valid_values_length(prop_reply) == 2 ) {
                int32_t *values = xcb_randr_query_output_property_valid_values(prop_reply);
                free(prop_reply);

                int32_t brn_min_abs = values[0];
                int32_t brn_max_abs = values[1];
                int32_t brn_new_abs = brn_percent * (brn_max_abs - brn_min_abs) / 100;
                *brn_cur_perc = (uint8_t) ((brn_cur_abs - brn_min_abs) * 100 / (brn_max_abs - brn_min_abs));
                *brn_new_perc = *brn_cur_perc;

                switch (operation) {
                    case OPERATION_GETBRIGHTNESS:
                        TRACE("[operation_handler] OPERATION_GETBRIGHTNESS\n");
                        TRACE("[operation_handler] min_abs:%d <= cur_abs:%d <= max_abs:%d\n", brn_min_abs, brn_cur_abs, brn_max_abs);
                        free(resources_reply);
                        return true;
                    case OPERATION_SETBRIGHTNESS:
                        brn_new_abs = brn_min_abs + brn_new_abs;
                        TRACE("[operation_handler] OPERATION_SETBRIGHTNESS -> %d (abs)\n", brn_new_abs);
                        break;
                    case OPERATION_INCBRIGHTNESS:
                        brn_new_abs = brn_cur_abs + brn_new_abs;
                        TRACE("[operation_handler] OPERATION_INCBRIGHTNESS -> %d (abs)\n", brn_new_abs);
                        break;
                    case OPERATION_DECBRIGHTNESS:
                        brn_new_abs = brn_cur_abs - brn_new_abs;
                        TRACE("[operation_handler] OPERATION_DECBRIGHTNESS -> %d (abs)\n", brn_new_abs);
                }
                if (brn_new_abs > brn_max_abs) { brn_new_abs = brn_max_abs; }
                if (brn_new_abs < brn_min_abs) { brn_new_abs = brn_min_abs; }
                *brn_new_perc = (uint8_t) (brn_new_abs * 100 / (brn_max_abs - brn_min_abs));

                TRACE("[operation_handler] min_abs:%d <= cur_abs:%d -> new_abs:%d <= max_abs:%d\n", brn_min_abs, brn_cur_abs, brn_new_abs, brn_max_abs);
                TRACE("[operation_handler] cur_perc:%d -> new_perc:%d\n", *brn_cur_perc, *brn_new_perc);

                (void)set_brightness_randr(pxcb, outputs[o], brn_new_abs);
                xcb_flush(pxcb->connection);
            } else {
                free(prop_reply);
            }
        }
    }
    free(resources_reply);
    if (!output_found) {
        ERROR("Error: Couldn't get brightness for any output.\n");
    }
    return output_found;
}
#endif


///////////////////////////////////////////////////////////////////////////////
// _operation_handler_file()
///////////////////////////////////////////////////////////////////////////////
/** Provides set/get/increase/decrease brightness operations using sysfs files.

    @param operation        the brightness operation to perform
    @param brn_percent      brightness percentage to set/increase/decrease depending on `operation`
    @param brn_cur_perc     the current brightness as percentage
    @param brn_new_perc     the new brightness as percentage
    @return                 true if operation could be performed, false on an unrecoverable error

    @see operations_t
*/
#ifdef USE_SYSFS_BACKLIGHT_CONTROL
bool _operation_handler_file(const operations_t operation, const uint8_t brn_percent, uint8_t *brn_cur_perc, uint8_t *brn_new_perc) {
    int32_t brn_min_abs = 0;
    int32_t brn_max_abs = 0;
    int32_t brn_cur_abs = 0;

    if ( NO_BRIGHTNESS == (brn_cur_abs = get_brightness_file(SYSFS_BACKLIGHT_PATH "brightness")) ) {
        ERROR("Error: Couldn't get current brightness for output.\n");
        return false;
    }
    if ( NO_BRIGHTNESS == (brn_max_abs = get_brightness_file(SYSFS_BACKLIGHT_PATH "max_brightness")) ) {
        ERROR("Error: Couldn't get maximal brightness for output.\n");
        return false;
    }
    int32_t brn_new_abs = brn_percent * (brn_max_abs - brn_min_abs) / 100;
    *brn_cur_perc = (uint8_t) ((brn_cur_abs - brn_min_abs) * 100 / (brn_max_abs - brn_min_abs));
    *brn_new_perc = *brn_cur_perc;

    switch (operation) {
        case OPERATION_GETBRIGHTNESS:
            TRACE("[operation_handler] OPERATION_GETBRIGHTNESS\n");
            TRACE("[operation_handler] min_abs:%d <= cur_abs:%d <= max_abs:%d\n", brn_min_abs, brn_cur_abs, brn_max_abs);
            return true;
        case OPERATION_SETBRIGHTNESS:
            brn_new_abs = brn_min_abs + brn_new_abs;
            TRACE("[operation_handler] OPERATION_SETBRIGHTNESS -> %d (abs)\n", brn_new_abs);
            break;
        case OPERATION_INCBRIGHTNESS:
            brn_new_abs = brn_cur_abs + brn_new_abs;
            TRACE("[operation_handler] OPERATION_INCBRIGHTNESS -> %d (abs)\n", brn_new_abs);
            break;
        case OPERATION_DECBRIGHTNESS:
            brn_new_abs = brn_cur_abs - brn_new_abs;
            TRACE("[operation_handler] OPERATION_DECBRIGHTNESS -> %d (abs)\n", brn_new_abs);
    }
    if (brn_new_abs > brn_max_abs) { brn_new_abs = brn_max_abs; }
    if (brn_new_abs < brn_min_abs) { brn_new_abs = brn_min_abs; }
    *brn_new_perc = (uint8_t) (brn_new_abs * 100 / (brn_max_abs - brn_min_abs));

    TRACE("[operation_handler] min_abs:%d <= cur_abs:%d -> new_abs:%d <= max_abs:%d\n", brn_min_abs, brn_cur_abs, brn_new_abs, brn_max_abs);
    TRACE("[operation_handler] cur_perc:%d -> new_perc:%d\n", *brn_cur_perc, *brn_new_perc);

    (void)set_brightness_file(SYSFS_BACKLIGHT_PATH "brightness", brn_new_abs);

    return true;
}
#endif


///////////////////////////////////////////////////////////////////////////////
// operation_handler()
///////////////////////////////////////////////////////////////////////////////
/** Wrapper function calling the sysfs files backend or the xrandr backend variant.

    @param operation        the brightness operation to perform
    @param pxcb             the global xcb container struct
    @param brn_percent      brightness percentage to set/increase/decrease depending on `operation`
    @param brn_cur_perc     the current brightness as percentage
    @param brn_new_perc     the new brightness as percentage
    @return                 true if operation could be performed, false on an unrecoverable error

    @see _operation_handler_file
    @see _operation_handler_randr
*/
static inline bool operation_handler(const operations_t operation, struct Txcb *pxcb, const uint8_t brn_percent, uint8_t *brn_cur_perc, uint8_t *brn_new_perc){
#ifdef USE_SYSFS_BACKLIGHT_CONTROL
    (void)pxcb;
    return _operation_handler_file(operation, brn_percent, brn_cur_perc, brn_new_perc);
#else
    return _operation_handler_randr(operation, pxcb, brn_percent, brn_cur_perc, brn_new_perc);
#endif
}


///////////////////////////////////////////////////////////////////////////////
// signal_handler()
///////////////////////////////////////////////////////////////////////////////
/** Signal Handler initiating a proper shutdown when being interrupted or killed.

    @param sig              the signal number received
*/
static void signal_handler(const int sig) {
    switch(sig){
        case SIGTERM:
        case SIGINT:
        case SIGQUIT:
            DEBUG("[signal_handler] received SIG_TERM/SIG_QUIT, exiting\n");
            exit(EXIT_SUCCESS);
    }
    DEBUG("[signal_handler] received unhandled signal %d.\n", sig);
    exit(EXIT_FAILURE);
}


///////////////////////////////////////////////////////////////////////////////
// _event_loop_scrsvr_on_timeout()
///////////////////////////////////////////////////////////////////////////////
/** Helper function to `event_loop()` handling the screensaver `timeout` event.

    @param pxcb             the global xcb container struct
    @param peventstate      event loop brightness state container struct
    @return                 RET_OK on success, failure exit code on error (e.g, EXIT_FAILURE)

    @see event_loop
    @see RET_OK
*/
static uint8_t _event_loop_scrsvr_on_timeout(struct Txcb *pxcb, struct Teventstate *peventstate) {
    if (!operation_handler(OPERATION_GETBRIGHTNESS, pxcb, 0, &peventstate->brn_priorscrsvr_perc , &peventstate->brn_cur_perc)) {
        ERROR("Error: Failed to get brightness on screensaver timeout. Exiting.\n");
        return EXIT_FAILURE;
    }
    if (peventstate->brn_cur_perc == 0) {
        DEBUG("[eventloop] brightness is 0%% on timeout, setting 100%% brightness\n");
        if (!operation_handler(OPERATION_SETBRIGHTNESS, pxcb, 100, &peventstate->brn_old_perc, &peventstate->brn_cur_perc)) {
            ERROR("Error: Failed to set initial brightness to 100%% on screensaver timeout. Exiting.\n");
            return EXIT_FAILURE;
        }
        DEBUG("[eventloop] backlight reports brightness %d%% set\n", peventstate->brn_cur_perc);
        if (peventstate->brn_cur_perc == 0) {
            DEBUG("[eventloop] brightness is still 0%%, setting 100%% brightness\n");
            if (!operation_handler(OPERATION_SETBRIGHTNESS, pxcb, 100, &peventstate->brn_old_perc, &peventstate->brn_cur_perc)) {
                ERROR("Error: Failed to set brightness to 100%% on screensaver timeout again. Exiting.\n");
                return EXIT_FAILURE;
            }
            DEBUG("[eventloop] backlight reports brightness %d%% set\n", peventstate->brn_cur_perc);
        }
    }
    if (peventstate->brn_cur_perc < DIM_PERCENT_TIMEOUT) {
        DEBUG("[eventloop] current brightness %d%% is below target brightness of %d%%, doing nothing.\n", peventstate->brn_cur_perc, DIM_PERCENT_TIMEOUT);
        return RET_OK;
    }
    if (!operation_handler(OPERATION_SETBRIGHTNESS, pxcb, DIM_PERCENT_TIMEOUT, &peventstate->brn_old_perc, &peventstate->brn_cur_perc)) {
        ERROR("Error: Failed to decrease brightness on screensaver timeout. Exiting.\n");
        return EXIT_FAILURE;
    }
    DEBUG("[eventloop] brightness %d%% -> %d%%\n", peventstate->brn_priorscrsvr_perc, peventstate->brn_cur_perc);
    return RET_OK;
}


///////////////////////////////////////////////////////////////////////////////
// _event_loop_scrsvr_on_interval()
///////////////////////////////////////////////////////////////////////////////
/** Helper function to `event_loop()` handling the screensaver `interval` event.

    @param pxcb             the global xcb container struct
    @param peventstate      event loop brightness state container struct
    @return                 RET_OK on success, failure exit code on error (e.g, EXIT_FAILURE)

    @see event_loop
    @see RET_OK
*/
static uint8_t _event_loop_scrsvr_on_interval(struct Txcb *pxcb, struct Teventstate *peventstate) {
    if (!peventstate->brn_interval_set) {
        peventstate->brn_interval_set = true;
        if (peventstate->brn_cur_perc < DIM_PERCENT_INTERVAL) {
            DEBUG("[eventloop] current brightness %d%% is below target brightness of %d%%, doing nothing.\n", peventstate->brn_cur_perc, DIM_PERCENT_INTERVAL);
            return RET_OK;
        }
        if (!operation_handler(OPERATION_SETBRIGHTNESS, pxcb, DIM_PERCENT_INTERVAL, &peventstate->brn_old_perc, &peventstate->brn_cur_perc)) {
            ERROR("Error: Failed to decrease brightness on screensaver interval. Exiting.\n");
            return EXIT_FAILURE;
        }
        DEBUG("[eventloop] brightness %d%% -> %d%%\n", peventstate->brn_old_perc, peventstate->brn_cur_perc);
        return RET_OK;
    }
    DEBUG("[eventloop] brightness already set to %d%%\n", peventstate->brn_cur_perc);
    return RET_OK;
}


///////////////////////////////////////////////////////////////////////////////
// _event_loop_scrsvr_off()
///////////////////////////////////////////////////////////////////////////////
/** Helper function to `event_loop()` handling the screensaver turn off event.

    @param pxcb             the global xcb container struct
    @param peventstate      event loop brightness state container struct
    @return                 RET_OK on success, failure exit code on error (e.g, EXIT_FAILURE)

    @see event_loop
    @see RET_OK
*/
static uint8_t _event_loop_scrsvr_off(struct Txcb *pxcb, struct Teventstate *peventstate) {
    peventstate->brn_interval_set = false;
    if (peventstate->brn_priorscrsvr_perc == BRN_PRIORSCRSVR_UNDEFINED) {
        DEBUG("[eventloop] event: OFF received without being called on timeout or interval, not setting brightness\n");
        DEBUG("[eventloop] getting current brightness\n");
        if (!operation_handler(OPERATION_GETBRIGHTNESS, pxcb, 0, &peventstate->brn_old_perc, &peventstate->brn_cur_perc)) {
            ERROR("Error: Failed to get brightness while setting screensaver OFF. Exiting.\n");
            return EXIT_FAILURE;
        }
        return RET_OK;
    }
    if (!operation_handler(OPERATION_GETBRIGHTNESS, pxcb, 0, &peventstate->brn_old_perc, &peventstate->brn_cur_perc)) {
        ERROR("Error: Failed to get brightness while setting screensaver OFF. Exiting.\n");
        return EXIT_FAILURE;
    }
    if (peventstate->brn_cur_perc == 0) {
        DEBUG("[eventloop] brightness is 0%% on setting OFF screensaver, setting to sane 100%% brightness\n");
        peventstate->brn_priorscrsvr_perc = 100;
    }
    if (operation_handler(OPERATION_SETBRIGHTNESS, pxcb, peventstate->brn_priorscrsvr_perc, &peventstate->brn_old_perc, &peventstate->brn_cur_perc)) {
        DEBUG("[eventloop] set to previous brightness %d%% from %d%%\n", peventstate->brn_priorscrsvr_perc, peventstate->brn_old_perc);
    } else {
        ERROR("Error: Failed to set prior brightness while setting screensaver OFF. Exiting.\n");
        return EXIT_FAILURE;
    }
    peventstate->brn_priorscrsvr_perc = BRN_PRIORSCRSVR_UNDEFINED;
    return RET_OK;
}


///////////////////////////////////////////////////////////////////////////////
// event_loop()
///////////////////////////////////////////////////////////////////////////////
/** The event loop handling screensaver-related events.

    The event loop is an indefinite loop only interrupted by errors or signals,
    hence the return code is propagated to exit() upon returning.
    For brevity, it calls several helper functions to do the actual work, namely
    * _event_loop_scrsvr_on_timeout     called when getting the `timeout` event
    * _event_loop_scrsvr_on_interval    called when getting the `interval` event
    * _event_loop_scrsvr_off            called when the screensaver should turn off

    @param pglobalstate     state container struct
    @param pxcb             xcb container struct
    @param peventstate      event loop brightness state  container struct
    @return                 failure code on error (e.g, EXIT_FAILURE) propagated to exit()

    @see Tglobalstate
    @see Txcb
    @see Teventstate
    @see signal_handler
    @see _event_loop_scrsvr_on_timeout
    @see _event_loop_scrsvr_on_interval
    @see _event_loop_scrsvr_off
*/
static uint8_t event_loop(struct Tglobalstate *pglobalstate, struct Txcb *pxcb, struct Teventstate *peventstate) {
    xcb_generic_event_t *event_generic;
    uint8_t result;

    while (true) {
        if (xcb_connection_has_error(pxcb->connection)) {
            ERROR("Error: xcb connection error while waiting for events\n");
            return EXIT_FAILURE;
        }

        event_generic = xcb_wait_for_event(pxcb->connection);
        if (XCB_EVENT_RESPONSE_TYPE(event_generic) != pxcb->screensaver_id) {
            free(event_generic);
            continue;
        }
        free(event_generic);

        if (!query_state(pglobalstate, pxcb)) {
            ERROR("Error: cannot query screensaver/dpms settings. Exiting.\n");
            return EXIT_FAILURE;
        }

        switch (pglobalstate->state) {
            case STATE_SCREENSAVER_ON_TIMEOUT:
                DEBUG("[eventloop] handling event: ON (timeout)      [idle=%ds]\n", pglobalstate->screensaver_idlesecuser);
                if ( RET_OK != (result = _event_loop_scrsvr_on_timeout(pxcb, peventstate))  ) { return result; }
                break;
            case STATE_SCREENSAVER_ON_INTERVAL:
                DEBUG("[eventloop] handling event: ON (interval)     [idle=%ds]\n", pglobalstate->screensaver_idlesecuser);
                if ( RET_OK != (result = _event_loop_scrsvr_on_interval(pxcb, peventstate)) ) { return result; }
                break;
            case STATE_SCREENSAVER_OFF:
                DEBUG("[eventloop] handling event: OFF               [idle=%ds]\n", pglobalstate->screensaver_idlesecuser);
                if ( RET_OK != (result = _event_loop_scrsvr_off(pxcb, peventstate))         ) { return result; }
                break;
            case STATE_SCREENSAVER_CYCLE:
                DEBUG("[eventloop] handling event: CYCLE             [idle=%ds]\n", pglobalstate->screensaver_idlesecuser);
                break;
            case STATE_SCREENSAVER_DISABLED:
                DEBUG("[eventloop] handling event: DISABLED          [idle=%ds]\n", pglobalstate->screensaver_idlesecuser);
                break;
            case STATE_DPMS_STANDBY:
                DEBUG("[eventloop] handling event: ON (dpms_standby) [idle=%ds]\n", pglobalstate->screensaver_idlesecuser);
                break;
            case STATE_DPMS_SUSPEND:
                DEBUG("[eventloop] handling event: ON (dpms_suspend) [idle=%ds]\n", pglobalstate->screensaver_idlesecuser);
                break;
            case STATE_DPMS_OFF:
                DEBUG("[eventloop] handling event: ON (dpms_off)     [idle=%ds]\n", pglobalstate->screensaver_idlesecuser);
                break;
            case STATE_UNKNOWN:
            default:
                DEBUG("[eventloop] unknown event %d received!        [idle=%ds]\n", pglobalstate->state, pglobalstate->screensaver_idlesecuser);
                break;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// parse_uint8_t()
///////////////////////////////////////////////////////////////////////////////
/** Converts a string to an uint8_t.

    @param input            the string which should be converted
    @param output           a pointer in which the conversion result will be written
    @return                 a non-zero value means the conversion has failed
*/
static int parse_uint8_t(char* input, uint8_t* output) {
    char *end = NULL;
    errno = 0;

    long temp = strtol(input, &end, 10);
    if (end != input && errno != ERANGE && temp >= 0 && temp <= UINT8_MAX) {
        *output = (uint8_t)temp;
        return 0;
    }
    ERROR("[parse_uint8_t] Unable to convert %s to uint8_t\n", input);
    return 1;
}

///////////////////////////////////////////////////////////////////////////////
// print_usage()
///////////////////////////////////////////////////////////////////////////////
void print_usage(void) {
    printf("Usage: brightnessd [options...]\n"
           "\n"
           "Available options:\n"
           "  --cycle-brightness   PERCENTAGE               Screen brightness percentage on cycle event (X11)\n"
           "  --timeout-brightness PERCENTAGE               Screen brightness percentage on timeout event (X11)\n"
           );
}

///////////////////////////////////////////////////////////////////////////////
// parse_args()
///////////////////////////////////////////////////////////////////////////////
/** Parses a string array (e.g. command-line arguments) and modifies the global
    state (configuration).

    @param len           the length of the string array
    @param args          an array of strings which should be parsed
    @return              a non-zero return value indicates an error
*/
static int parse_args(int len, char** args) {

    int err = 0;
    int opt = 0;
    static struct option long_options[] = {
        {"cycle-brightness",   required_argument,       0,  'c' },
        {"timeout-brightness", required_argument,       0,  't' },
        {"help",               no_argument,             0,  'h' },
        {0,                    0,                       0,  0   }
    };

    int long_index = 0;
    while ((opt = getopt_long(len, args, "c:t:h",
                              long_options, &long_index)) != -1) {
        switch (opt) {
        case 'c':
            err = parse_uint8_t(optarg, &DIM_PERCENT_INTERVAL);
            break;
        case 't':
            err = parse_uint8_t(optarg, &DIM_PERCENT_TIMEOUT);
            break;
        case 'h':
            print_usage();
            exit(EXIT_SUCCESS);
        default:
            print_usage();
            err = 1;
        }
    }
    return err;
}

///////////////////////////////////////////////////////////////////////////////
// main()
///////////////////////////////////////////////////////////////////////////////
/** Setup the screensaver and call the eventloop.

    @return                 failure exit code on error (e.g, EXIT_FAILURE)

    @see event_loop
*/
int main(int argc, char** argv) {

    if (parse_args(argc, argv)) {
        ERROR("[main] Error parsing command-line arguments.\n");
        exit(EXIT_FAILURE);
    }
    DEBUG("[main] Configuration: DIM_PERCENT_INTERVAL=%d, DIM_PERCENT_TIMEOUT=%d\n", DIM_PERCENT_INTERVAL, DIM_PERCENT_TIMEOUT);

    xcb_generic_error_t               *xcb_generic_error;
    xcb_void_cookie_t                  xcb_void_cookie;
    const xcb_query_extension_reply_t *query_ext_reply;

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Color Output
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    if (!isatty(STDOUT_FILENO)) {
        gs_color.yellow = "";
        gs_color.red    = "";
        gs_color.gray   = "";
        gs_color.green  = "";
        gs_color.reset  = "";
    }

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Test SysFS Brightness File(s)
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    #ifdef USE_SYSFS_BACKLIGHT_CONTROL
    DEBUG("[init] testing availability of brightness files\n");
    if ( !is_file_accessible(SYSFS_BACKLIGHT_PATH "brightness",        R_OK | W_OK) ) { exit(EX_UNAVAILABLE); }
    if ( !is_file_accessible(SYSFS_BACKLIGHT_PATH "max_brightness",    R_OK       ) ) { exit(EX_UNAVAILABLE); }
    if ( !is_file_accessible(SYSFS_BACKLIGHT_PATH "actual_brightness", R_OK       ) ) { exit(EX_UNAVAILABLE); }
    #endif

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // xcb
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    DEBUG("[init] getting xcb connection\n");
    gs_xcb.connection = xcb_connect(NULL, &gs_xcb.screen_nr);
    if (!gs_xcb.connection || xcb_connection_has_error(gs_xcb.connection)) {
        ERROR("Error: cannot open xcb connection\n");
        exit(EX_UNAVAILABLE);
    }
    TRACE("[init] running on screen #%u\n", gs_xcb.screen_nr);
    xcb_screen_iterator_t screen_iterator = xcb_setup_roots_iterator(xcb_get_setup(gs_xcb.connection));
    for (; screen_iterator.rem; --gs_xcb.screen_nr, xcb_screen_next(&screen_iterator)) {
        if (gs_xcb.screen_nr == 0) {
          gs_xcb.screen = screen_iterator.data;
          break;
        }
    }
    TRACE("[init] screen #%u's dimensions: %ux%u\n",
            gs_xcb.screen_nr,
            gs_xcb.screen->width_in_pixels,
            gs_xcb.screen->height_in_pixels
    );

    atexit(shutdown_connection);

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // randr
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    DEBUG("[init] querying randr extension\n");
    xcb_randr_query_version_cookie_t gs_xcb_randr_query_version_cookie = xcb_randr_query_version(gs_xcb.connection, 1, 2);
    xcb_randr_query_version_reply_t *gs_xcb_randr_query_version_reply  = xcb_randr_query_version_reply(gs_xcb.connection, gs_xcb_randr_query_version_cookie, &xcb_generic_error);
    if (xcb_generic_error != NULL || gs_xcb_randr_query_version_reply == NULL) {
        ERROR("Error: cannot query randr extension\n");
        exit(EX_UNAVAILABLE);
    }
    if (gs_xcb_randr_query_version_reply->major_version != 1 || gs_xcb_randr_query_version_reply->minor_version < 2) {
        ERROR("Error: randr version %d.%d too old\n", gs_xcb_randr_query_version_reply->major_version, gs_xcb_randr_query_version_reply->minor_version);
        free(gs_xcb_randr_query_version_reply);
        exit(EX_UNAVAILABLE);
    }
    free(gs_xcb_randr_query_version_reply);

    xcb_intern_atom_cookie_t  gs_xcb_intern_atom_cookie_backlight[2];
    xcb_intern_atom_reply_t  *gs_xcb_intern_atom_reply_backlight;
    gs_xcb_intern_atom_cookie_backlight[0] = xcb_intern_atom(gs_xcb.connection, 1, strlen("Backlight"), "Backlight");
    gs_xcb_intern_atom_reply_backlight     = xcb_intern_atom_reply(gs_xcb.connection, gs_xcb_intern_atom_cookie_backlight[0], &xcb_generic_error);
    if (xcb_generic_error != NULL || gs_xcb_intern_atom_reply_backlight == NULL) {
        ERROR("Error: Intern Atom returned error %d while querying backlight property\n", xcb_generic_error ? xcb_generic_error->error_code : -1);
        exit(EX_UNAVAILABLE);
    }
    gs_xcb.backlight_new_atom = gs_xcb_intern_atom_reply_backlight->atom;
    free(gs_xcb_intern_atom_reply_backlight);

    gs_xcb_intern_atom_cookie_backlight[1] = xcb_intern_atom(gs_xcb.connection, 1, strlen("BACKLIGHT"), "BACKLIGHT");
    gs_xcb_intern_atom_reply_backlight     = xcb_intern_atom_reply(gs_xcb.connection, gs_xcb_intern_atom_cookie_backlight[1], &xcb_generic_error);
    if (xcb_generic_error != NULL || gs_xcb_intern_atom_reply_backlight == NULL) {
        ERROR("Error: Intern Atom returned error %d while querying backlight property\n", xcb_generic_error ? xcb_generic_error->error_code : -1);
        exit(EX_UNAVAILABLE);
    }
    gs_xcb.backlight_legacy_atom = gs_xcb_intern_atom_reply_backlight->atom;
    free(gs_xcb_intern_atom_reply_backlight);

    if (gs_xcb.backlight_new_atom == XCB_NONE && gs_xcb.backlight_legacy_atom == XCB_NONE) {
        ERROR("Error: No outputs have backlight property\n");
        exit(EX_UNAVAILABLE);
    }

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // DPMS
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    DEBUG("[init] querying dpms extension\n");
    query_ext_reply = xcb_get_extension_data(gs_xcb.connection, &xcb_dpms_id);
    if ( !query_ext_reply || query_ext_reply->present == 0 ) {
        ERROR("Error: cannot query dpms extension. Exiting.\n");
        exit(EXIT_FAILURE);
    }

    xcb_dpms_capable_cookie_t gs_xcb_dpms_capable_cookie = xcb_dpms_capable_unchecked(gs_xcb.connection);
    xcb_dpms_capable_reply_t *gs_xcb_dpms_capable_reply  = xcb_dpms_capable_reply(gs_xcb.connection, gs_xcb_dpms_capable_cookie, NULL);
    if (!gs_xcb_dpms_capable_reply || gs_xcb_dpms_capable_reply->capable == 0) {
        free(gs_xcb_dpms_capable_reply);
        ERROR("Error: display not capable of dpms. Exiting.\n");
        exit(EXIT_FAILURE);
    }
    free(gs_xcb_dpms_capable_reply);

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Screensaver
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    DEBUG("[init] querying screensaver extension\n");
    query_ext_reply = xcb_get_extension_data(gs_xcb.connection, &xcb_screensaver_id);
    if ( !query_ext_reply || query_ext_reply->present == 0 ) {
        ERROR("Error: cannot query screensaver extension. Exiting.\n");
        exit(EXIT_FAILURE);
    }
    gs_xcb.screensaver_id = query_ext_reply->first_event + XCB_SCREENSAVER_NOTIFY;

    DEBUG("[init] querying screensaver settings\n");
    if (!query_state(&gs_globalstate, &gs_xcb)) {
        ERROR("Error: cannot get screensaver settings\n");
        exit(EXIT_FAILURE);
    }

    // Create a pixmap and register it as the screensaver's "window" via _SCREEN_SAVER_ID property
    DEBUG("[init] creating and registering screensaver's window\n");
    gs_xcb.pixmap = xcb_generate_id(gs_xcb.connection);
    xcb_void_cookie   = xcb_create_pixmap(gs_xcb.connection, 1, gs_xcb.pixmap , gs_xcb.screen->root, 1, 1);
    if ( (xcb_generic_error = xcb_request_check(gs_xcb.connection, xcb_void_cookie)) ) {
        ERROR("Error: cannot create screensaver window's pixmap. Exiting.\n");
        exit(EXIT_FAILURE);
    }

    xcb_intern_atom_cookie_t intern_atom_cookie = xcb_intern_atom(gs_xcb.connection, 0, strlen("_SCREEN_SAVER_ID"), "_SCREEN_SAVER_ID");
    gs_xcb.screensaver_id_atom                  = xcb_intern_atom_reply(gs_xcb.connection, intern_atom_cookie, NULL);
    if (!gs_xcb.screensaver_id_atom) {
        ERROR("Error: cannot create _SCREEN_SAVER_ID property. Exiting.\n");
        exit(EXIT_FAILURE);
    }
    xcb_void_cookie = xcb_change_property(
            gs_xcb.connection,
            XCB_PROP_MODE_REPLACE,
            gs_xcb.screen->root,
            gs_xcb.screensaver_id_atom->atom, XCB_ATOM_PIXMAP, 32, 1, &gs_xcb.pixmap
    );
    if ( (xcb_generic_error = xcb_request_check(gs_xcb.connection, xcb_void_cookie)) ) {
        ERROR("Error: cannot register _SCREEN_SAVER_ID property. Exiting.\n");
        exit(EXIT_FAILURE);
    }

    // set attributes for use as "external" screensaver
    xcb_void_cookie = xcb_screensaver_set_attributes(
        gs_xcb.connection,
        gs_xcb.screen->root,
        -1, -1, 1, 1,
        0,
        XCB_WINDOW_CLASS_COPY_FROM_PARENT,
        gs_xcb.screen->root_depth,
        gs_xcb.screen->root_visual,
        0, NULL
    );
    if ( (xcb_generic_error = xcb_request_check(gs_xcb.connection, xcb_void_cookie)) ) {
        ERROR("Error: cannot set screensaver attributes. Exiting.\n");
        exit(EXIT_FAILURE);
    }

    // register some "known" environment variables pointing to the screensaver
	char xid[32];
	(void)snprintf(xid, sizeof(xid), "0x%lx", (unsigned long)gs_xcb.pixmap);
	(void)setenv("XSS_WINDOW", xid, 1);
	(void)snprintf(xid, sizeof(xid), "0x%lx", (unsigned long)gs_xcb.pixmap);
	(void)setenv("XSCREENSAVER_WINDOW", xid, 1);

    DEBUG("[init] subscribing to screensaver events\n");
    xcb_void_cookie = xcb_screensaver_select_input(
            gs_xcb.connection,
            gs_xcb.screen->root,
            XCB_SCREENSAVER_EVENT_NOTIFY_MASK | XCB_SCREENSAVER_EVENT_CYCLE_MASK
    );
    if ( (xcb_generic_error = xcb_request_check(gs_xcb.connection, xcb_void_cookie)) ) {
        ERROR("Error: cannot subscribe to screensaver events. Exiting.\n");
        exit(EXIT_FAILURE);
    }

    atexit(shutdown_deregister_events);

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Get Initial Brightness from Backlight
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    DEBUG("[init] flushing xcb requests queue\n");
    xcb_flush(gs_xcb.connection);

    DEBUG("[init] get initial brightness readings\n");
    uint8_t brn_cur_perc, brn_old_perc;
    if (!operation_handler(OPERATION_GETBRIGHTNESS, &gs_xcb, 0, &brn_cur_perc, &brn_old_perc)) {
        #ifndef USE_SYSFS_BACKLIGHT_CONTROL
        ERROR("check if randr has Backlight property by $ xrandr --prop | grep -i backlight\n");
        #endif
        exit(EXIT_FAILURE);
    }
    if (brn_cur_perc == 0) {
        ERROR("cannot get sensible brightness reading for any display!\n");
        #ifndef USE_SYSFS_BACKLIGHT_CONTROL
        ERROR("check if randr has Backlight property by $ xrandr --prop | grep -i backlight\n");
        #endif
        exit(EXIT_FAILURE);
    }

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Install Signal Handler
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    DEBUG("[init] registering signal handlers\n");
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);
    signal(SIGINT,  signal_handler);

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Event Loop
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    DEBUG("[init] waiting for screensaver events (current brightness: %u%%)\n", brn_cur_perc);
    exit( event_loop(&gs_globalstate, &gs_xcb, &gs_eventstate) );
}

// vim: expandtab tabstop=4 shiftwidth=4
