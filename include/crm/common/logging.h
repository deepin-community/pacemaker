/*
 * Copyright 2004-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Wrappers for and extensions to libqb logging
 * \ingroup core
 */

#ifndef CRM_LOGGING__H
#  define CRM_LOGGING__H

#  include <stdio.h>
#  include <glib.h>
#  include <qb/qblog.h>
#  include <libxml/tree.h>

/* Define custom log priorities.
 *
 * syslog(3) uses int for priorities, but libqb's struct qb_log_callsite uses
 * uint8_t, so make sure they fit in the latter.
 */

// Define something even less desired than debug
#  ifndef LOG_TRACE
#    define LOG_TRACE   (LOG_DEBUG+1)
#  endif

// Print message to stdout instead of logging it
#  ifndef LOG_STDOUT
#    define LOG_STDOUT  254
#  endif

// Don't send message anywhere
#  ifndef LOG_NEVER
#    define LOG_NEVER   255
#  endif

/* "Extended information" logging support */
#ifdef QB_XS
#  define CRM_XS QB_XS
#  define crm_extended_logging(t, e) qb_log_ctl((t), QB_LOG_CONF_EXTENDED, (e))
#else
#  define CRM_XS "|"

/* A caller might want to check the return value, so we can't define this as a
 * no-op, and we can't simply define it to be 0 because gcc will then complain
 * when the value isn't checked.
 */
static inline int
crm_extended_logging(int t, int e)
{
    return 0;
}
#endif

extern unsigned int crm_log_level;
extern unsigned int crm_trace_nonlog;

/*! \deprecated Pacemaker library functions set this when a configuration
 *              error is found, which turns on extra messages at the end of
 *              processing. It should not be used directly and will be removed
 *              from the public C API in a future release.
 */
extern gboolean crm_config_error;

/*! \deprecated Pacemaker library functions set this when a configuration
 *              warning is found, which turns on extra messages at the end of
 *              processing. It should not be used directly and will be removed
 *              from the public C API in a future release.
 */
extern gboolean crm_config_warning;

enum xml_log_options
{
    xml_log_option_filtered   = 0x0001,
    xml_log_option_formatted  = 0x0002,
    xml_log_option_text       = 0x0004, /* add this option to dump text into xml */
    xml_log_option_full_fledged = 0x0008, // Use libxml when converting XML to text
    xml_log_option_diff_plus  = 0x0010,
    xml_log_option_diff_minus = 0x0020,
    xml_log_option_diff_short = 0x0040,
    xml_log_option_diff_all   = 0x0100,
    xml_log_option_dirty_add  = 0x1000,
    xml_log_option_open       = 0x2000,
    xml_log_option_children   = 0x4000,
    xml_log_option_close      = 0x8000,
};

void crm_enable_blackbox(int nsig);
void crm_disable_blackbox(int nsig);
void crm_write_blackbox(int nsig, struct qb_log_callsite *callsite);

void crm_update_callsites(void);

void crm_log_deinit(void);

void crm_log_preinit(const char *entity, int argc, char **argv);
gboolean crm_log_init(const char *entity, uint8_t level, gboolean daemon,
                      gboolean to_stderr, int argc, char **argv, gboolean quiet);

void crm_log_args(int argc, char **argv);
void crm_log_output_fn(const char *file, const char *function, int line, int level,
                       const char *prefix, const char *output);

// Log a block of text line by line
#define crm_log_output(level, prefix, output)   \
    crm_log_output_fn(__FILE__, __func__, __LINE__, level, prefix, output)

void crm_bump_log_level(int argc, char **argv);

void crm_enable_stderr(int enable);

gboolean crm_is_callsite_active(struct qb_log_callsite *cs, uint8_t level, uint32_t tags);

void log_data_element(int log_level, const char *file, const char *function, int line,
                      const char *prefix, xmlNode * data, int depth, gboolean formatted);

/* returns the old value */
unsigned int set_crm_log_level(unsigned int level);

unsigned int get_crm_log_level(void);

/*
 * Throughout the macros below, note the leading, pre-comma, space in the
 * various ' , ##args' occurrences to aid portability across versions of 'gcc'.
 * https://gcc.gnu.org/onlinedocs/cpp/Variadic-Macros.html#Variadic-Macros
 */
#if defined(__clang__)
#    define CRM_TRACE_INIT_DATA(name)
#  else
#    include <assert.h> // required by QB_LOG_INIT_DATA() macro
#    define CRM_TRACE_INIT_DATA(name) QB_LOG_INIT_DATA(name)
#endif

/* Using "switch" instead of "if" in these macro definitions keeps
 * static analysis from complaining about constant evaluations
 */

/*!
 * \brief Log a message
 *
 * \param[in] level  Priority at which to log the message
 * \param[in] fmt    printf-style format string literal for message
 * \param[in] args   Any arguments needed by format string
 *
 * \note This is a macro, and \p level may be evaluated more than once.
 */
#  define do_crm_log(level, fmt, args...) do {                              \
        switch (level) {                                                    \
            case LOG_STDOUT:                                                \
                printf(fmt "\n" , ##args);                                  \
                break;                                                      \
            case LOG_NEVER:                                                 \
                break;                                                      \
            default:                                                        \
                qb_log_from_external_source(__func__, __FILE__, fmt,        \
                    (level),   __LINE__, 0 , ##args);                       \
                break;                                                      \
        }                                                                   \
    } while (0)

/*!
 * \brief Log a message that is likely to be filtered out
 *
 * \param[in] level  Priority at which to log the message
 * \param[in] fmt    printf-style format string for message
 * \param[in] args   Any arguments needed by format string
 *
 * \note This is a macro, and \p level may be evaluated more than once.
 *       This does nothing when level is LOG_STDOUT.
 */
#  define do_crm_log_unlikely(level, fmt, args...) do {                     \
        switch (level) {                                                    \
            case LOG_STDOUT: case LOG_NEVER:                                \
                break;                                                      \
            default: {                                                      \
                static struct qb_log_callsite *trace_cs = NULL;             \
                if (trace_cs == NULL) {                                     \
                    trace_cs = qb_log_callsite_get(__func__, __FILE__, fmt, \
                                                   (level), __LINE__, 0);   \
                }                                                           \
                if (crm_is_callsite_active(trace_cs, (level), 0)) {         \
                    qb_log_from_external_source(__func__, __FILE__, fmt,    \
                        (level), __LINE__, 0 , ##args);                     \
                }                                                           \
            }                                                               \
            break;                                                          \
        }                                                                   \
    } while (0)

#  define CRM_LOG_ASSERT(expr) do {					\
        if (!(expr)) {                                                  \
            static struct qb_log_callsite *core_cs = NULL;              \
            if(core_cs == NULL) {                                       \
                core_cs = qb_log_callsite_get(__func__, __FILE__,       \
                                              "log-assert", LOG_TRACE,  \
                                              __LINE__, 0);             \
            }                                                           \
            crm_abort(__FILE__, __func__, __LINE__, #expr,              \
                      core_cs?core_cs->targets:FALSE, TRUE);            \
        }                                                               \
    } while(0)

/* 'failure_action' MUST NOT be 'continue' as it will apply to the
 * macro's do-while loop
 */
#  define CRM_CHECK(expr, failure_action) do {				            \
        if (!(expr)) {                                                  \
            static struct qb_log_callsite *core_cs = NULL;              \
            if (core_cs == NULL) {                                      \
                core_cs = qb_log_callsite_get(__func__, __FILE__,       \
                                              "check-assert",           \
                                              LOG_TRACE, __LINE__, 0);  \
            }                                                           \
	        crm_abort(__FILE__, __func__, __LINE__, #expr,	            \
		        (core_cs? core_cs->targets: FALSE), TRUE);              \
	        failure_action;						                        \
	    }								                                \
    } while(0)

/*!
 * \brief Log XML line-by-line in a formatted fashion
 *
 * \param[in] level  Priority at which to log the messages
 * \param[in] text   Prefix for each line
 * \param[in] xml    XML to log
 *
 * \note This is a macro, and \p level may be evaluated more than once.
 *       This does nothing when level is LOG_STDOUT.
 */
#  define do_crm_log_xml(level, text, xml) do {                             \
        switch (level) {                                                    \
            case LOG_STDOUT: case LOG_NEVER:                                \
                break;                                                      \
            default: {                                                      \
                static struct qb_log_callsite *xml_cs = NULL;               \
                if (xml_cs == NULL) {                                       \
                    xml_cs = qb_log_callsite_get(__func__, __FILE__,        \
                                        "xml-blob", (level), __LINE__, 0);  \
                }                                                           \
                if (crm_is_callsite_active(xml_cs, (level), 0)) {           \
                    log_data_element((level), __FILE__, __func__,           \
                         __LINE__, text, xml, 1, xml_log_option_formatted); \
                }                                                           \
            }                                                               \
            break;                                                          \
        }                                                                   \
    } while(0)

/*!
 * \brief Log a message as if it came from a different code location
 *
 * \param[in] level     Priority at which to log the message
 * \param[in] file      Source file name to use instead of __FILE__
 * \param[in] function  Source function name to use instead of __func__
 * \param[in] line      Source line number to use instead of __line__
 * \param[in] fmt       printf-style format string literal for message
 * \param[in] args      Any arguments needed by format string
 *
 * \note This is a macro, and \p level may be evaluated more than once.
 */
#  define do_crm_log_alias(level, file, function, line, fmt, args...) do {  \
        switch (level) {                                                    \
            case LOG_STDOUT:                                                \
                printf(fmt "\n" , ##args);                                  \
                break;                                                      \
            case LOG_NEVER:                                                 \
                break;                                                      \
            default:                                                        \
                qb_log_from_external_source(function, file, fmt, (level),   \
                                            line, 0 , ##args);              \
                break;                                                      \
        }                                                                   \
    } while (0)

/*!
 * \brief Send a system error message to both the log and stderr
 *
 * \param[in] level  Priority at which to log the message
 * \param[in] fmt    printf-style format string for message
 * \param[in] args   Any arguments needed by format string
 *
 * \deprecated One of the other logging functions should be used with
 *             pcmk_strerror() instead.
 * \note This is a macro, and \p level may be evaluated more than once.
 * \note Because crm_perror() adds the system error message and error number
 *       onto the end of fmt, that information will become extended information
 *       if CRM_XS is used inside fmt and will not show up in syslog.
 */
#  define crm_perror(level, fmt, args...) do {                              \
        switch (level) {                                                    \
            case LOG_NEVER:                                                 \
                break;                                                      \
            default: {                                                      \
                const char *err = strerror(errno);                          \
                /* cast to int makes coverity happy when level == 0 */      \
                if ((level) <= (int) crm_log_level) {                       \
                    fprintf(stderr, fmt ": %s (%d)\n" , ##args, err, errno);\
                }                                                           \
                do_crm_log((level), fmt ": %s (%d)" , ##args, err, errno);  \
            }                                                               \
            break;                                                          \
        }                                                                   \
    } while (0)

/*!
 * \brief Log a message with a tag (for use with PCMK_trace_tags)
 *
 * \param[in] level  Priority at which to log the message
 * \param[in] tag    String to tag message with
 * \param[in] fmt    printf-style format string for message
 * \param[in] args   Any arguments needed by format string
 *
 * \note This is a macro, and \p level may be evaluated more than once.
 *       This does nothing when level is LOG_STDOUT.
 */
#  define crm_log_tag(level, tag, fmt, args...)    do {                     \
        switch (level) {                                                    \
            case LOG_STDOUT: case LOG_NEVER:                                \
                break;                                                      \
            default: {                                                      \
                static struct qb_log_callsite *trace_tag_cs = NULL;         \
                int converted_tag = g_quark_try_string(tag);                \
                if (trace_tag_cs == NULL) {                                 \
                    trace_tag_cs = qb_log_callsite_get(__func__, __FILE__,  \
                                    fmt, (level), __LINE__, converted_tag); \
                }                                                           \
                if (crm_is_callsite_active(trace_tag_cs, (level),           \
                                           converted_tag)) {                \
                    qb_log_from_external_source(__func__, __FILE__, fmt,    \
                                (level), __LINE__, converted_tag , ##args); \
                }                                                           \
            }                                                               \
        }                                                                   \
    } while (0)

#  define crm_emerg(fmt, args...)   qb_log(LOG_EMERG,       fmt , ##args)
#  define crm_crit(fmt, args...)    qb_logt(LOG_CRIT,    0, fmt , ##args)
#  define crm_err(fmt, args...)     qb_logt(LOG_ERR,     0, fmt , ##args)
#  define crm_warn(fmt, args...)    qb_logt(LOG_WARNING, 0, fmt , ##args)
#  define crm_notice(fmt, args...)  qb_logt(LOG_NOTICE,  0, fmt , ##args)
#  define crm_info(fmt, args...)    qb_logt(LOG_INFO,    0, fmt , ##args)

#  define crm_debug(fmt, args...)   do_crm_log_unlikely(LOG_DEBUG, fmt , ##args)
#  define crm_trace(fmt, args...)   do_crm_log_unlikely(LOG_TRACE, fmt , ##args)

#  define crm_log_xml_crit(xml, text)    do_crm_log_xml(LOG_CRIT,    text, xml)
#  define crm_log_xml_err(xml, text)     do_crm_log_xml(LOG_ERR,     text, xml)
#  define crm_log_xml_warn(xml, text)    do_crm_log_xml(LOG_WARNING, text, xml)
#  define crm_log_xml_notice(xml, text)  do_crm_log_xml(LOG_NOTICE,  text, xml)
#  define crm_log_xml_info(xml, text)    do_crm_log_xml(LOG_INFO,    text, xml)
#  define crm_log_xml_debug(xml, text)   do_crm_log_xml(LOG_DEBUG,   text, xml)
#  define crm_log_xml_trace(xml, text)   do_crm_log_xml(LOG_TRACE,   text, xml)

#  define crm_log_xml_explicit(xml, text)  do {                 \
        static struct qb_log_callsite *digest_cs = NULL;        \
        digest_cs = qb_log_callsite_get(                        \
            __func__, __FILE__, text, LOG_TRACE, __LINE__,      \
            crm_trace_nonlog);                                  \
        if (digest_cs && digest_cs->targets) {                  \
            do_crm_log_xml(LOG_TRACE,   text, xml);             \
        }                                                       \
    } while(0)

#  define crm_str(x)    (const char*)(x?x:"<null>")

#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
#include <crm/common/logging_compat.h>
#endif

#ifdef __cplusplus
}
#endif

#endif
