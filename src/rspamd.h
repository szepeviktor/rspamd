/**
 * @file main.h
 * Definitions for main rspamd structures
 */

#ifndef RSPAMD_MAIN_H
#define RSPAMD_MAIN_H

#include "config.h"
#include "libutil/fstring.h"
#include "libutil/mem_pool.h"
#include "libutil/util.h"
#include "libutil/logger.h"
#include "libutil/http.h"
#include "libutil/upstream.h"
#include "libserver/url.h"
#include "libserver/protocol.h"
#include "libserver/buffer.h"
#include "libserver/events.h"
#include "libserver/roll_history.h"
#include "libserver/task.h"


/* Default values */
#define FIXED_CONFIG_FILE RSPAMD_CONFDIR "/rspamd.conf"
/* Time in seconds to exit for old worker */
#define SOFT_SHUTDOWN_TIME 10

/* Spam subject */
#define SPAM_SUBJECT "*** SPAM *** "

#ifdef CRLF
#undef CRLF
#undef CR
#undef LF
#endif

#define CRLF "\r\n"
#define CR '\r'
#define LF '\n'

/**
 * Worker process structure
 */
struct rspamd_worker {
	pid_t pid;                      /**< pid of worker									*/
	guint index;                    /**< index number									*/
	struct rspamd_main *srv;        /**< pointer to server structure					*/
	GQuark type;                    /**< process type									*/
	GHashTable *signal_events;      /**< signal events									*/
	GList *accept_events;           /**< socket events									*/
	struct rspamd_worker_conf *cf;  /**< worker config data								*/
	gpointer ctx;                   /**< worker's specific data							*/
	gint control_pipe[2];           /**< control pipe. [0] is used by main process,
	                                                   [1] is used by a worker			*/
};

struct rspamd_worker_signal_handler;

struct rspamd_worker_signal_cb {
	void (*handler) (struct rspamd_worker_signal_handler *, void *ud);
	void *handler_data;
	struct rspamd_worker_signal_cb *next, *prev;
};

struct rspamd_worker_signal_handler {
	gint signo;
	gboolean enabled;
	struct event ev;
	struct event_base *base;
	struct rspamd_worker *worker;
	struct rspamd_worker_signal_cb *cb;
};

struct rspamd_controller_pbkdf {
	gint id;
	guint rounds;
	gsize salt_len;
	gsize key_len;
};

/**
 * Common structure representing C module context
 */
struct module_s;
struct module_ctx {
	gint (*filter)(struct rspamd_task *task);                   /**< pointer to headers process function			*/
	struct module_s *mod;										/**< module pointer									*/
	gboolean enabled;											/**< true if module is enabled in configuration		*/
};

/**
 * Module
 */
typedef struct module_s {
	const gchar *name;
	int (*module_init_func)(struct rspamd_config *cfg, struct module_ctx **ctx);
	int (*module_config_func)(struct rspamd_config *cfg);
	int (*module_reconfig_func)(struct rspamd_config *cfg);
	int (*module_attach_controller_func)(struct module_ctx *ctx,
		GHashTable *custom_commands);
} module_t;

typedef struct worker_s {
	const gchar *name;
	gpointer (*worker_init_func)(struct rspamd_config *cfg);
	void (*worker_start_func)(struct rspamd_worker *worker);
	gboolean has_socket;
	gboolean unique;
	gboolean threaded;
	gboolean killable;
	gint listen_type;
} worker_t;

struct pidfh;
struct rspamd_config;
struct tokenizer;
struct rspamd_stat_classifier;
struct rspamd_classifier_config;
struct mime_part;
struct rspamd_dns_resolver;
struct rspamd_task;

/**
 * The epoch of the fuzzy client
 */
enum rspamd_fuzzy_epoch {
	RSPAMD_FUZZY_EPOCH6 = 0, /**< pre 0.6.x */
	RSPAMD_FUZZY_EPOCH8,     /**< 0.8 till 0.9 */
	RSPAMD_FUZZY_EPOCH9,     /**< 0.9 + */
	RSPAMD_FUZZY_EPOCH10,    /**< 1.0 + encryption */
	RSPAMD_FUZZY_EPOCH_MAX
};

/**
 * Server statistics
 */
struct rspamd_stat {
	guint messages_scanned;                             /**< total number of messages scanned				*/
	guint actions_stat[METRIC_ACTION_NOACTION + 1];     /**< statistic for each action						*/
	guint connections_count;                            /**< total connections count						*/
	guint control_connections_count;                    /**< connections count to control interface			*/
	guint messages_learned;                             /**< messages learned								*/
	guint fuzzy_hashes;                                 /**< number of fuzzy hashes stored					*/
	guint fuzzy_hashes_expired;                         /**< number of fuzzy hashes expired					*/
	guint64 fuzzy_hashes_checked[RSPAMD_FUZZY_EPOCH_MAX]; /**< ammount of check requests for each epoch		*/
	guint64 fuzzy_hashes_found[RSPAMD_FUZZY_EPOCH_MAX]; /**< amount of hashes found by epoch				*/
};

/**
 * Struct that determine main server object (for logging purposes)
 */
struct rspamd_main {
	struct rspamd_config *cfg;                                  /**< pointer to config structure					*/
	pid_t pid;                                                  /**< main pid										*/
	/* Pid file structure */
	rspamd_pidfh_t *pfh;                                        /**< struct pidfh for pidfile						*/
	GQuark type;                                                /**< process type									*/
	guint ev_initialized;                                       /**< is event system is initialized					*/
	struct rspamd_stat *stat;                                   /**< pointer to statistics							*/

	rspamd_mempool_t *server_pool;                                  /**< server's memory pool							*/
	GHashTable *workers;                                        /**< workers pool indexed by pid                    */
	rspamd_logger_t *logger;
	uid_t workers_uid;                                          /**< worker's uid running to                        */
	gid_t workers_gid;                                          /**< worker's gid running to						*/
	gboolean is_privilleged;                                    /**< true if run in privilleged mode                */
	struct roll_history *history;                               /**< rolling history								*/
};

/**
 * Structure to point exception in text from processing
 */
struct process_exception {
	gsize pos;
	gsize len;
};

/**
 * Control session object
 */
struct controller_command;
struct controller_session;
typedef gboolean (*controller_func_t)(gchar **args,
	struct controller_session *session);

struct controller_session {
	struct rspamd_worker *worker;                               /**< pointer to worker structure (controller in fact) */
	enum {
		STATE_COMMAND,
		STATE_HEADER,
		STATE_LEARN,
		STATE_LEARN_SPAM_PRE,
		STATE_LEARN_SPAM,
		STATE_REPLY,
		STATE_QUIT,
		STATE_OTHER,
		STATE_WAIT,
		STATE_WEIGHTS
	} state;                                                    /**< current session state							*/
	gint sock;                                                  /**< socket descriptor								*/
	/* Access to authorized commands */
	gboolean authorized;                                        /**< whether this session is authorized				*/
	gboolean restful;                                           /**< whether this session is a restful session		*/
	GHashTable *kwargs;                                         /**< keyword arguments for restful command			*/
	struct controller_command *cmd;                             /**< real command									*/
	rspamd_mempool_t *session_pool;                             /**< memory pool for session                        */
	struct rspamd_config *cfg;                                  /**< pointer to config file							*/
	gchar *learn_rcpt;                                          /**< recipient for learning							*/
	gchar *learn_from;                                          /**< from address for learning						*/
	struct rspamd_classifier_config *learn_classifier;
	gchar *learn_symbol;                                            /**< symbol to train								*/
	double learn_multiplier;                                    /**< multiplier for learning						*/
	rspamd_io_dispatcher_t *dispatcher;                         /**< IO dispatcher object							*/
	rspamd_fstring_t *learn_buf;                                         /**< learn input									*/
	GList *parts;                                               /**< extracted mime parts							*/
	gint in_class;                                              /**< positive or negative learn						*/
	gboolean (*other_handler)(struct controller_session *session,
		rspamd_fstring_t *in);                       /**< other command handler to execute at the end of processing */
	void *other_data;                                           /**< and its data                                   */
	controller_func_t custom_handler;                           /**< custom command handler							*/
	struct rspamd_async_session * s;                             /**< async session object							*/
	struct rspamd_task *learn_task;
	struct rspamd_dns_resolver *resolver;                       /**< DNS resolver									*/
	struct event_base *ev_base;                                 /**< Event base										*/
};


/**
 * Register custom controller function
 */
void register_custom_controller_command (const gchar *name,
	controller_func_t handler,
	gboolean privilleged,
	gboolean require_message);

#define RSPAMD_PBKDF_ID_V1 1
extern const struct rspamd_controller_pbkdf pbkdf_list[];

#endif

/*
 * vi:ts=4
 */
