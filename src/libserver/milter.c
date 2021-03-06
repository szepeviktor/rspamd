/*-
 * Copyright 2017 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include "milter.h"
#include "milter_internal.h"
#include "email_addr.h"
#include "addr.h"
#include "unix-std.h"
#include "logger.h"
#include "ottery.h"
#include "libutil/http.h"
#include "libutil/http_private.h"
#include "libserver/protocol_internal.h"
#include "libmime/filter.h"
#include "libserver/worker_util.h"
#include "utlist.h"

#define msg_err_milter(...) rspamd_default_log_function(G_LOG_LEVEL_CRITICAL, \
        "milter", priv->pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn_milter(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING, \
        "milter", priv->pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info_milter(...)   rspamd_default_log_function (G_LOG_LEVEL_INFO, \
        "milter", priv->pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_debug_milter(...)  rspamd_default_log_function (G_LOG_LEVEL_DEBUG, \
        "milter", priv->pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)

struct rspamd_milter_context {
	gchar *spam_header;
	void *sessions_cache;
	gboolean discard_on_reject;
};

static struct rspamd_milter_context *milter_ctx = NULL;

static gboolean  rspamd_milter_handle_session (
		struct rspamd_milter_session *session,
		struct rspamd_milter_private *priv);

static GQuark
rspamd_milter_quark (void)
{
	return g_quark_from_static_string ("milter");
}

static void
rspamd_milter_obuf_free (struct rspamd_milter_outbuf *obuf)
{
	if (obuf) {
		if (obuf->buf) {
			rspamd_fstring_free (obuf->buf);
		}

		g_free (obuf);
	}
}

#define RSPAMD_MILTER_RESET_COMMON (1 << 0)
#define RSPAMD_MILTER_RESET_IO (1 << 1)
#define RSPAMD_MILTER_RESET_ADDR (1 << 2)
#define RSPAMD_MILTER_RESET_MACRO (1 << 3)
#define RSPAMD_MILTER_RESET_ALL (RSPAMD_MILTER_RESET_COMMON | \
	RSPAMD_MILTER_RESET_IO | \
	RSPAMD_MILTER_RESET_ADDR | \
	RSPAMD_MILTER_RESET_MACRO)
#define RSPAMD_MILTER_RESET_QUIT_NC (RSPAMD_MILTER_RESET_COMMON | \
	RSPAMD_MILTER_RESET_ADDR | \
	RSPAMD_MILTER_RESET_MACRO)
#define RSPAMD_MILTER_RESET_ABORT (RSPAMD_MILTER_RESET_COMMON)

static void
rspamd_milter_session_reset (struct rspamd_milter_session *session,
		guint how)
{
	struct rspamd_milter_outbuf *obuf, *obuf_tmp;
	struct rspamd_milter_private *priv = session->priv;
	struct rspamd_email_address *cur;
	guint i;

	if (how & RSPAMD_MILTER_RESET_IO) {
		DL_FOREACH_SAFE (priv->out_chain, obuf, obuf_tmp) {
			rspamd_milter_obuf_free (obuf);
		}

		priv->out_chain = NULL;

		if (priv->parser.buf) {
			priv->parser.buf->len = 0;
		}
	}

	if (how & RSPAMD_MILTER_RESET_COMMON) {
		if (session->message) {
			session->message->len = 0;
		}

		if (session->rcpts) {
			PTR_ARRAY_FOREACH (session->rcpts, i, cur) {
				rspamd_email_address_free (cur);
			}

			g_ptr_array_free (session->rcpts, TRUE);
			session->rcpts = NULL;
		}

		if (session->from) {
			rspamd_email_address_free (session->from);
			session->from = NULL;
		}

		if (session->helo) {
			session->helo->len = 0;
		}

		if (session->hostname) {
			session->hostname->len = 0;
		}

		if (priv->headers) {
			g_hash_table_remove_all (priv->headers);
		}
	}

	if (how & RSPAMD_MILTER_RESET_ADDR) {
		if (session->addr) {
			rspamd_inet_address_free (session->addr);
			session->addr = NULL;
		}
	}

	if (how & RSPAMD_MILTER_RESET_MACRO) {
		if (session->macros) {
			g_hash_table_unref (session->macros);
			session->macros = NULL;
		}
	}
}

static void
rspamd_milter_session_dtor (struct rspamd_milter_session *session)
{
	struct rspamd_milter_private *priv;

	if (session) {
		priv = session->priv;
		msg_debug_milter ("destroying milter session");

		if (event_get_base (&priv->ev)) {
			event_del (&priv->ev);
		}

		rspamd_milter_session_reset (session, RSPAMD_MILTER_RESET_ALL);

		if (priv->parser.buf) {
			rspamd_fstring_free (priv->parser.buf);
		}

		if (session->message) {
			rspamd_fstring_free (session->message);
		}

		if (session->helo) {
			rspamd_fstring_free (session->helo);
		}

		if (session->hostname) {
			rspamd_fstring_free (session->hostname);
		}

		if (priv->headers) {
			g_hash_table_destroy (priv->headers);
		}

		if (milter_ctx->sessions_cache) {
			rspamd_worker_session_cache_remove (milter_ctx->sessions_cache,
					session);
		}

		rspamd_mempool_delete (priv->pool);
		g_free (priv);
		g_free (session);
	}
}

static void
rspamd_milter_on_protocol_error (struct rspamd_milter_session *session,
		struct rspamd_milter_private *priv, GError *err)
{
	priv->state = RSPAMD_MILTER_WANNA_DIE;
	REF_RETAIN (session);
	priv->err_cb (priv->fd, session, priv->ud, err);
	REF_RELEASE (session);
	g_error_free (err);
}

static void
rspamd_milter_io_handler (gint fd, gshort what, void *ud)
{
	struct rspamd_milter_session *session = ud;
	struct rspamd_milter_private *priv;
	GError *err;

	priv = session->priv;

	if (what == EV_TIMEOUT) {
		msg_debug_milter ("connection timed out");
		err = g_error_new (rspamd_milter_quark (), ETIMEDOUT, "connection "
				"timed out");
		rspamd_milter_on_protocol_error (session, priv, err);
	}
	else {
		rspamd_milter_handle_session (session, priv);
	}
}

static inline void
rspamd_milter_plan_io (struct rspamd_milter_session *session,
		struct rspamd_milter_private *priv, gshort what)
{
	if (event_get_base (&priv->ev)) {
		event_del (&priv->ev);
	}

	event_set (&priv->ev, priv->fd, what, rspamd_milter_io_handler,
			session);
	event_base_set (priv->ev_base, &priv->ev);
	event_add (&priv->ev, priv->ptv);
}


#define READ_INT_32(pos, var) do { \
	memcpy (&(var), (pos), sizeof (var)); \
	(pos) += sizeof (var); \
	(var) = ntohl (var); \
} while (0)
#define READ_INT_16(pos, var) do { \
	memcpy (&(var), (pos), sizeof (var)); \
	(pos) += sizeof (var); \
	(var) = ntohs (var); \
} while (0)

static gboolean
rspamd_milter_process_command (struct rspamd_milter_session *session,
		struct rspamd_milter_private *priv)
{
	GError *err;
	rspamd_fstring_t *buf;
	const guchar *pos, *end, *zero;
	guint cmdlen;
	guint32 version, actions, protocol;

	buf = priv->parser.buf;
	pos = buf->str + priv->parser.cmd_start;
	cmdlen = priv->parser.datalen;
	end = pos + cmdlen;

	switch (priv->parser.cur_cmd) {
	case RSPAMD_MILTER_CMD_ABORT:
		msg_debug_milter ("got abort command");
		rspamd_milter_session_reset (session, RSPAMD_MILTER_RESET_ABORT);
		break;
	case RSPAMD_MILTER_CMD_BODY:
		if (!session->message) {
			session->message = rspamd_fstring_sized_new (
					RSPAMD_MILTER_MESSAGE_CHUNK);
		}

		msg_debug_milter ("got body chunk: %d bytes", (int)cmdlen);
		session->message = rspamd_fstring_append (session->message,
				pos, cmdlen);
		break;
	case RSPAMD_MILTER_CMD_CONNECT:
		msg_debug_milter ("got connect command");

		/*
		 * char hostname[]: Hostname, NUL terminated
		 * char family: Protocol family
		 * uint16 port: Port number (SMFIA_INET or SMFIA_INET6 only)
		 * char address[]: IP address (ASCII) or unix socket path, NUL terminated
		 */
		zero = memchr (pos, '\0', cmdlen);

		if (zero == NULL || zero > (end - sizeof (guint16) + 1)) {
			err = g_error_new (rspamd_milter_quark (), EINVAL, "invalid "
					"connect command (no name)");
			rspamd_milter_on_protocol_error (session, priv, err);

			return FALSE;
		}
		else {
			guchar proto;
			guint16 port;
			gchar ip6_str[INET6_ADDRSTRLEN + 3];
			gsize r;

			/*
			 * Important notice: Postfix do NOT use this command to pass
			 * client's info (e.g. hostname is not really here)
			 * Sendmail will pass it here
			 */
			if (session->hostname == NULL) {
				session->hostname = rspamd_fstring_new_init (pos, zero - pos);
			}
			else {
				session->hostname = rspamd_fstring_assign (session->hostname,
						pos, zero - pos);
			}

			pos = zero + 1;
			proto = *pos ++;

			if (proto == RSPAMD_MILTER_CONN_UNKNOWN) {
				/* We have no information about host */
				msg_debug_milter ("unknown connect address");
			}
			else {
				READ_INT_16 (pos, port);

				if (pos >= end) {
					/* No IP somehow */
					msg_debug_milter ("unknown connect IP/socket");
				}
				else {
					zero = memchr (pos, '\0', end - pos);

					if (zero == NULL) {
						err = g_error_new (rspamd_milter_quark (), EINVAL, "invalid "
								"connect command (no zero terminated IP)");
						rspamd_milter_on_protocol_error (session, priv, err);

						return FALSE;
					}

					switch (proto) {
					case RSPAMD_MILTER_CONN_UNIX:
						session->addr = rspamd_inet_address_new (AF_UNIX,
								pos);
						break;

					case RSPAMD_MILTER_CONN_INET:
						session->addr = rspamd_inet_address_new (AF_INET, NULL);

						if (!rspamd_parse_inet_address_ip (pos, zero - pos,
								session->addr)) {
							err = g_error_new (rspamd_milter_quark (), EINVAL,
									"invalid connect command (bad IPv4)");
							rspamd_milter_on_protocol_error (session, priv,
									err);

							return FALSE;
						}

						rspamd_inet_address_set_port (session->addr, port);
						break;

					case RSPAMD_MILTER_CONN_INET6:
						session->addr = rspamd_inet_address_new (AF_INET6, NULL);

						if (zero - pos > sizeof ("IPv6:") &&
								rspamd_lc_cmp (pos, "IPv6:",
										sizeof ("IPv6:") - 1) == 0) {
							/* Kill sendmail please */
							pos += sizeof ("IPv6:") - 1;

							if (*pos != '[') {
								/* Add explicit braces */
								r = rspamd_snprintf (ip6_str, sizeof (ip6_str),
										"[%*s]", (int)(zero - pos), pos);
							}
							else {
								r = rspamd_strlcpy (ip6_str, pos, sizeof (ip6_str));
							}
						}
						else {
							r = rspamd_strlcpy (ip6_str, pos, sizeof (ip6_str));
						}

						if (!rspamd_parse_inet_address_ip (ip6_str, r,
								session->addr)) {
							err = g_error_new (rspamd_milter_quark (), EINVAL,
									"invalid connect command (bad IPv6)");
							rspamd_milter_on_protocol_error (session, priv,
									err);

							return FALSE;
						}

						rspamd_inet_address_set_port (session->addr, port);
						break;

					default:
						err = g_error_new (rspamd_milter_quark (), EINVAL,
								"invalid connect command (bad protocol: %c)",
								proto);
						rspamd_milter_on_protocol_error (session, priv,
								err);

						return FALSE;
					}
				}
			}

			msg_info_milter ("got connection from %s",
					rspamd_inet_address_to_string_pretty (session->addr));
		}
		break;
	case RSPAMD_MILTER_CMD_MACRO:
		msg_debug_milter ("got macro command");
		/*
		 * Format is
		 * 1 byte - command associated (we don't care about it)
		 * 0-terminated name
		 * 0-terminated value
		 * ...
		 */
		if (session->macros == NULL) {
			session->macros = g_hash_table_new_full (rspamd_ftok_icase_hash,
					rspamd_ftok_icase_equal,
					rspamd_fstring_mapped_ftok_free,
					rspamd_fstring_mapped_ftok_free);
		}

		/* Ignore one byte */
		pos ++;

		while (pos < end) {
			zero = memchr (pos, '\0', cmdlen);

			if (zero == NULL) {
				err = g_error_new (rspamd_milter_quark (), EINVAL, "invalid "
						"macro command (no name)");
				rspamd_milter_on_protocol_error (session, priv, err);

				return FALSE;
			}
			else {
				rspamd_fstring_t *name, *value;
				rspamd_ftok_t *name_tok, *value_tok;
				const guchar *zero_val;

				zero_val = memchr (zero + 1, '\0', cmdlen);

				if (end > zero_val) {
					name = rspamd_fstring_new_init (pos, zero - pos);
					value = rspamd_fstring_new_init (zero + 1,
							zero_val - zero - 1);
					name_tok = rspamd_ftok_map (name);
					value_tok = rspamd_ftok_map (value);

					g_hash_table_replace (session->macros, name_tok, value_tok);
					msg_debug_milter ("got macro: %T -> %T",
							name_tok, value_tok);

					if (rspamd_ftok_cstr_equal (name_tok, "{mail_host}", FALSE)) {
						session->hostname = rspamd_fstring_assign (session->hostname,
								value_tok->begin, value_tok->len);
					}

					cmdlen -= zero_val - pos;
					pos = zero_val + 1;
				}
				else {
					err = g_error_new (rspamd_milter_quark (), EINVAL,
							"invalid macro command (bad value)");
					rspamd_milter_on_protocol_error (session, priv, err);

					return FALSE;
				}
			}
		}
		break;
	case RSPAMD_MILTER_CMD_BODYEOB:
		msg_debug_milter ("got eob command");
		REF_RETAIN (session);
		priv->fin_cb (priv->fd, session, priv->ud);
		REF_RELEASE (session);
		break;
	case RSPAMD_MILTER_CMD_HELO:
		msg_debug_milter ("got helo command");

		if (end > pos && *(end - 1) == '\0') {
			if (session->helo == NULL) {
				session->helo = rspamd_fstring_new_init (pos, cmdlen - 1);
			}
			else {
				session->helo = rspamd_fstring_assign (session->helo,
						pos, cmdlen - 1);
			}
		}
		else if (end > pos) {
			/* Should not happen */
			if (session->helo == NULL) {
				session->helo = rspamd_fstring_new_init (pos, cmdlen);
			}
			else {
				session->helo = rspamd_fstring_assign (session->helo,
						pos, cmdlen);
			}
		}

		msg_debug_milter ("got helo value: %V", session->helo);

		break;
	case RSPAMD_MILTER_CMD_QUIT_NC:
		/* We need to reset session and start over */
		msg_debug_milter ("got quit_nc command");
		rspamd_milter_session_reset (session, RSPAMD_MILTER_RESET_QUIT_NC);
		break;
	case RSPAMD_MILTER_CMD_HEADER:
		msg_debug_milter ("got header command");
		zero = memchr (pos, '\0', cmdlen);

		if (zero == NULL) {
			err = g_error_new (rspamd_milter_quark (), EINVAL, "invalid "
					"header command (no name)");
			rspamd_milter_on_protocol_error (session, priv, err);

			return FALSE;
		}
		else {
			if (end > zero && *(end - 1) == '\0') {
				gpointer res;
				gint num;

				res = g_hash_table_lookup (priv->headers, pos);
				if (res) {
					num = GPOINTER_TO_INT (res);
					num ++;
					/*
					 * We need to copy as glib is totally insane about it:
					 * > If you supplied a key_destroy_func when creating the
					 * > GHashTable, the passed key is freed using that function.
					 */
					g_hash_table_insert (priv->headers, g_strdup (pos),
							GINT_TO_POINTER (num));
				}
				else {
					num = 1;
					g_hash_table_insert (priv->headers, g_strdup (pos),
							GINT_TO_POINTER (num));
				}

				rspamd_printf_fstring (&session->message, "%*s: %*s\r\n",
						(int)(zero - pos), pos,
						(int)(end - zero - 2), zero + 1);
			}
			else {
				err = g_error_new (rspamd_milter_quark (), EINVAL, "invalid "
						"header command (bad value)");
				rspamd_milter_on_protocol_error (session, priv, err);

				return FALSE;
			}
		}
		break;
	case RSPAMD_MILTER_CMD_MAIL:
		msg_debug_milter ("mail command");

		while (pos < end) {
			struct rspamd_email_address *addr;
			gchar *cpy;

			zero = memchr (pos, '\0', end - pos);

			if (zero && zero > pos) {
				cpy = rspamd_mempool_alloc (priv->pool, zero - pos);
				memcpy (cpy, pos, zero - pos);
				msg_debug_milter ("got mail: %*s", (int)(zero - pos), cpy);
				addr = rspamd_email_address_from_smtp (cpy, zero - pos);

				if (addr) {
					session->from = addr;
				}

				/* TODO: parse esmtp arguments */
				break;
			}
			else {
				msg_debug_milter ("got weird from: %*s", (int)(end - pos),
						pos);
				/* That actually should not happen */
				cpy = rspamd_mempool_alloc (priv->pool, end - pos);
				memcpy (cpy, pos, end - pos);
				addr = rspamd_email_address_from_smtp (cpy, end - pos);

				if (addr) {
					session->from = addr;
				}

				break;
			}
		}
		break;
	case RSPAMD_MILTER_CMD_EOH:
		msg_debug_milter ("got eoh command");

		if (!session->message) {
			session->message = rspamd_fstring_sized_new (
					RSPAMD_MILTER_MESSAGE_CHUNK);
		}

		session->message = rspamd_fstring_append (session->message,
				"\r\n", 2);
		break;
	case RSPAMD_MILTER_CMD_OPTNEG:
		if (cmdlen != sizeof (guint32) * 3) {
			err = g_error_new (rspamd_milter_quark (), EINVAL, "invalid "
					"optneg command");
			rspamd_milter_on_protocol_error (session, priv, err);

			return FALSE;
		}

		READ_INT_32 (pos, version);
		READ_INT_32 (pos, actions);
		READ_INT_32 (pos, protocol);

		msg_debug_milter ("optneg: version: %d, actions: %d, protocol: %d",
				version, actions, protocol);

		if (version < RSPAMD_MILTER_PROTO_VER) {
			msg_warn_milter ("MTA specifies too old protocol: %d, "
					"aborting connection", version);

			err = g_error_new (rspamd_milter_quark (), EINVAL, "invalid "
					"protocol version: %d", version);
			rspamd_milter_on_protocol_error (session, priv, err);

			return FALSE;
		}

		version = RSPAMD_MILTER_PROTO_VER;
		actions |= RSPAMD_MILTER_ACTIONS_MASK;
		protocol = RSPAMD_MILTER_FLAG_NOREPLY_MASK;

		return rspamd_milter_send_action (session, RSPAMD_MILTER_OPTNEG,
			version, actions, protocol);
		break;
	case RSPAMD_MILTER_CMD_QUIT:
		if (priv->out_chain) {
			msg_debug_milter ("quit command, refcount: %d, "
					"some output buffers left - draining",
					session->ref.refcount);

			priv->state = RSPAMD_MILTER_WRITE_AND_DIE;
		}
		else {
			msg_debug_milter ("quit command, refcount: %d",
					session->ref.refcount);

			priv->state = RSPAMD_MILTER_WANNA_DIE;
			REF_RETAIN (session);
			priv->fin_cb (priv->fd, session, priv->ud);
			REF_RELEASE (session);

			return FALSE;
		}
		break;
	case RSPAMD_MILTER_CMD_RCPT:
		msg_debug_milter ("rcpt command");

		while (pos < end) {
			struct rspamd_email_address *addr;
			gchar *cpy;

			zero = memchr (pos, '\0', end - pos);

			if (zero && zero > pos) {
				cpy = rspamd_mempool_alloc (priv->pool, end - pos);
				memcpy (cpy, pos, end - pos);

				msg_debug_milter ("got rcpt: %*s", (int)(zero - pos), cpy);
				addr = rspamd_email_address_from_smtp (cpy, zero - pos);

				if (addr) {
					if (!session->rcpts) {
						session->rcpts = g_ptr_array_sized_new (1);
					}

					g_ptr_array_add (session->rcpts, addr);
				}

				pos = zero + 1;
			}
			else {
				cpy = rspamd_mempool_alloc (priv->pool, end - pos);
				memcpy (cpy, pos, end - pos);

				msg_debug_milter ("got weird rcpt: %*s", (int)(end - pos),
						pos);
				/* That actually should not happen */
				addr = rspamd_email_address_from_smtp (cpy, end - pos);

				if (addr) {
					if (!session->rcpts) {
						session->rcpts = g_ptr_array_sized_new (1);
					}

					g_ptr_array_add (session->rcpts, addr);
				}

				break;
			}
		}
		break;
	case RSPAMD_MILTER_CMD_DATA:
		if (!session->message) {
			session->message = rspamd_fstring_sized_new (
					RSPAMD_MILTER_MESSAGE_CHUNK);
		}
		msg_debug_milter ("got data command");
		/* We do not need reply as specified */
		break;
	default:
		msg_debug_milter ("got bad command: %c", priv->parser.cur_cmd);
		break;
	}

	return TRUE;
}

static gboolean
rspamd_milter_is_valid_cmd (guchar c)
{
	switch (c) {
	case RSPAMD_MILTER_CMD_ABORT:
	case RSPAMD_MILTER_CMD_BODY:
	case RSPAMD_MILTER_CMD_CONNECT:
	case RSPAMD_MILTER_CMD_MACRO:
	case RSPAMD_MILTER_CMD_BODYEOB:
	case RSPAMD_MILTER_CMD_HELO:
	case RSPAMD_MILTER_CMD_QUIT_NC:
	case RSPAMD_MILTER_CMD_HEADER:
	case RSPAMD_MILTER_CMD_MAIL:
	case RSPAMD_MILTER_CMD_EOH:
	case RSPAMD_MILTER_CMD_OPTNEG:
	case RSPAMD_MILTER_CMD_QUIT:
	case RSPAMD_MILTER_CMD_RCPT:
	case RSPAMD_MILTER_CMD_DATA:
	case RSPAMD_MILTER_CMD_UNKNOWN:
		return TRUE;
	default:
		break;
	}

	return FALSE;
}

static gboolean
rspamd_milter_consume_input (struct rspamd_milter_session *session,
		struct rspamd_milter_private *priv)
{
	const guchar *p, *end;
	GError *err;

	p = priv->parser.buf->str + priv->parser.pos;
	end = priv->parser.buf->str + priv->parser.buf->len;

	while (p < end) {
		msg_debug_milter("offset: %d, state: %d", (gint)(p - (const guchar *)priv->parser.buf->str), priv->parser.state);
		switch (priv->parser.state) {
		case st_len_1:
			/* The first length byte in big endian order */
			priv->parser.datalen = 0;
			priv->parser.datalen |= *p << 24;
			priv->parser.state = st_len_2;
			p++;
			break;
		case st_len_2:
			/* The second length byte in big endian order */
			priv->parser.datalen |= *p << 16;
			priv->parser.state = st_len_3;
			p++;
			break;
		case st_len_3:
			/* The third length byte in big endian order */
			priv->parser.datalen |= *p << 8;
			priv->parser.state = st_len_4;
			p++;
			break;
		case st_len_4:
			/* The fourth length byte in big endian order */
			priv->parser.datalen |= *p;
			priv->parser.state = st_read_cmd;
			p++;
			break;
		case st_read_cmd:
			priv->parser.cur_cmd = *p;
			priv->parser.state = st_read_data;

			if (priv->parser.datalen < 1) {
				err = g_error_new (rspamd_milter_quark (), EINVAL,
					"Command length is too short");
				rspamd_milter_on_protocol_error (session, priv, err);

				return FALSE;
			}
			else {
				/* Eat command itself */
				priv->parser.datalen --;
			}

			p++;
			priv->parser.cmd_start = p - (const guchar *)priv->parser.buf->str;
			break;
		case st_read_data:
			/* We might need some more data in buffer for further steps */
			if (priv->parser.datalen >
					RSPAMD_MILTER_MESSAGE_CHUNK * 2) {
				err = g_error_new (rspamd_milter_quark (), E2BIG,
						"Command length is too big: %zd",
						priv->parser.datalen);
				rspamd_milter_on_protocol_error (session, priv, err);

				return FALSE;
			}
			if (!rspamd_milter_is_valid_cmd (priv->parser.cur_cmd)) {
				err = g_error_new (rspamd_milter_quark (), E2BIG,
						"Unvalid command: %c",
						priv->parser.cur_cmd);
				rspamd_milter_on_protocol_error (session, priv, err);

				return FALSE;
			}
			if (priv->parser.buf->allocated < priv->parser.datalen) {
				priv->parser.pos = p - (const guchar *)priv->parser.buf->str;
				priv->parser.buf = rspamd_fstring_grow (priv->parser.buf,
						priv->parser.buf->len + priv->parser.datalen);
				/* This can realloc buffer */
				rspamd_milter_plan_io (session, priv, EV_READ);
				goto end;
			}
			else {
				/* We may have the full command available */
				if (p + priv->parser.datalen <= end) {
					/* We can process command */
					if (!rspamd_milter_process_command (session, priv)) {
						return FALSE;
					}

					p += priv->parser.datalen;
					priv->parser.state = st_len_1;
					priv->parser.cur_cmd = '\0';
					priv->parser.cmd_start = 0;
				}
				else {
					/* Need to read more */
					priv->parser.pos = p - (const guchar *)priv->parser.buf->str;
					rspamd_milter_plan_io (session, priv, EV_READ);
					goto end;
				}
			}
			break;
		}
	}

	/* Leftover */
	switch (priv->parser.state) {
	case st_read_data:
		if (p + priv->parser.datalen <= end) {
			if (!rspamd_milter_process_command (session, priv)) {
				return FALSE;
			}

			priv->parser.state = st_len_1;
			priv->parser.cur_cmd = '\0';
			priv->parser.cmd_start = 0;
		}
		break;
	default:
		/* No need to do anything */
		break;
	}

	if (p == end) {
		priv->parser.buf->len = 0;
		priv->parser.pos = 0;
	}

	if (priv->out_chain) {
		rspamd_milter_plan_io (session, priv, EV_READ|EV_WRITE);
	}
	else {
		rspamd_milter_plan_io (session, priv, EV_READ);
	}
end:

	return TRUE;
}

static gboolean
rspamd_milter_handle_session (struct rspamd_milter_session *session,
		struct rspamd_milter_private *priv)
{
	struct rspamd_milter_outbuf *obuf, *obuf_tmp;
	gssize r, to_write;
	GError *err;

	g_assert (session != NULL);

	switch (priv->state) {
	case RSPAMD_MILTER_READ_MORE:
		if (priv->parser.buf->len >= priv->parser.buf->allocated) {
			priv->parser.buf = rspamd_fstring_grow (priv->parser.buf,
					priv->parser.buf->len * 2);
		}

		r = read (priv->fd, priv->parser.buf->str + priv->parser.buf->len,
				priv->parser.buf->allocated - priv->parser.buf->len);

		msg_debug_milter ("read %z bytes, %z remain, %z allocated",
				r, priv->parser.buf->len, priv->parser.buf->allocated);

		if (r == -1) {
			if (errno == EAGAIN || errno == EINTR) {
				rspamd_milter_plan_io (session, priv, EV_READ);
			}
			else {
				/* Fatal IO error */
				err = g_error_new (rspamd_milter_quark (), errno,
						"IO read error: %s", strerror (errno));
				REF_RETAIN (session);
				priv->err_cb (priv->fd, session, priv->ud, err);
				REF_RELEASE (session);
				g_error_free (err);
			}
		}
		else if (r == 0) {
			err = g_error_new (rspamd_milter_quark (), ECONNRESET,
					"Unexpected EOF");
			REF_RETAIN (session);
			priv->err_cb (priv->fd, session, priv->ud, err);
			REF_RELEASE (session);
			g_error_free (err);
		}
		else {
			priv->parser.buf->len += r;

			return rspamd_milter_consume_input (session, priv);
		}
	case RSPAMD_MILTER_WRITE_REPLY:
	case RSPAMD_MILTER_WRITE_AND_DIE:
		if (priv->out_chain == NULL) {
			if (priv->state == RSPAMD_MILTER_WRITE_AND_DIE) {
				/* Finished writing, let's die finally */
				msg_debug_milter ("output drained, terminating, refcount: %d",
						session->ref.refcount);

				/* Session should be destroyed by fin_cb... */
				REF_RETAIN (session);
				priv->fin_cb (priv->fd, session, priv->ud);
				REF_RELEASE (session);

				return FALSE;
			}
			else {
				/* We have written everything, so we can read something */
				priv->state = RSPAMD_MILTER_READ_MORE;
				rspamd_milter_plan_io (session, priv, EV_READ);
			}
		}
		else {
			DL_FOREACH_SAFE (priv->out_chain, obuf, obuf_tmp) {
				to_write = obuf->buf->len - obuf->pos;

				g_assert (to_write > 0);

				r = write (priv->fd, obuf->buf->str + obuf->pos, to_write);

				if (r == -1) {
					if (errno == EAGAIN || errno == EINTR) {
						rspamd_milter_plan_io (session, priv, EV_WRITE);
					}
					else {
						/* Fatal IO error */
						err = g_error_new (rspamd_milter_quark (), errno,
								"IO write error: %s", strerror (errno));
						REF_RETAIN (session);
						priv->err_cb (priv->fd, session, priv->ud, err);
						REF_RELEASE (session);
						g_error_free (err);
					}
				}
				else if (r == 0) {
					err = g_error_new (rspamd_milter_quark (), ECONNRESET,
							"Unexpected EOF");
					REF_RETAIN (session);
					priv->err_cb (priv->fd, session, priv->ud, err);
					REF_RELEASE (session);
					g_error_free (err);
				}
				else {
					if (r == to_write) {
						/* We have done with this buf */
						DL_DELETE (priv->out_chain, obuf);
						rspamd_milter_obuf_free (obuf);
					}
					else {
						/* We need to plan another write */
						obuf->pos += r;
						rspamd_milter_plan_io (session, priv, EV_WRITE);

						return TRUE;
					}
				}
			}

			/* Here we have written everything, so we can plan reading */
			priv->state = RSPAMD_MILTER_READ_MORE;
			rspamd_milter_plan_io (session, priv, EV_READ);
		}
		break;
	case RSPAMD_MILTER_WANNA_DIE:
		/* We are here after processing everything, so release session */
		REF_RELEASE (session);
		break;
	}

	return TRUE;
}


gboolean
rspamd_milter_handle_socket (gint fd, const struct timeval *tv,
		rspamd_mempool_t *pool,
		struct event_base *ev_base, rspamd_milter_finish finish_cb,
		rspamd_milter_error error_cb, void *ud)
{
	struct rspamd_milter_session *session;
	struct rspamd_milter_private *priv;

	g_assert (finish_cb != NULL);
	g_assert (error_cb != NULL);
	g_assert (milter_ctx != NULL);

	session = g_malloc0 (sizeof (*session));
	priv = g_malloc0 (sizeof (*priv));
	priv->fd = fd;
	priv->ud = ud;
	priv->fin_cb = finish_cb;
	priv->err_cb = error_cb;
	priv->parser.state = st_len_1;
	priv->parser.buf = rspamd_fstring_sized_new (RSPAMD_MILTER_MESSAGE_CHUNK + 5);
	priv->ev_base = ev_base;
	priv->state = RSPAMD_MILTER_READ_MORE;
	priv->pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), "milter");
	priv->discard_on_reject = milter_ctx->discard_on_reject;

	if (pool) {
		/* Copy tag */
		memcpy (priv->pool->tag.uid, pool->tag.uid, sizeof (pool->tag.uid));
	}

	priv->headers = g_hash_table_new_full (rspamd_strcase_hash,
			rspamd_strcase_equal, g_free, NULL);

	if (tv) {
		memcpy (&priv->tv, tv, sizeof (*tv));
		priv->ptv = &priv->tv;
	}
	else {
		priv->ptv = NULL;
	}

	session->priv = priv;
	REF_INIT_RETAIN (session, rspamd_milter_session_dtor);

	if (milter_ctx->sessions_cache) {
		rspamd_worker_session_cache_add (milter_ctx->sessions_cache,
				priv->pool->tag.uid, &session->ref.refcount, session);
	}

	return rspamd_milter_handle_session (session, priv);
}

gboolean
rspamd_milter_set_reply (struct rspamd_milter_session *session,
		rspamd_fstring_t *xcode,
		rspamd_fstring_t *rcode,
		rspamd_fstring_t *reply)
{
	GString *buf;
	gboolean ret;

	buf = g_string_sized_new (xcode->len + rcode->len + reply->len + 2);
	rspamd_printf_gstring (buf, "%V %V %V", xcode, rcode, reply);
	ret = rspamd_milter_send_action (session, RSPAMD_MILTER_REPLYCODE,
		buf);
	g_string_free (buf, TRUE);

	return ret;
}

#define SET_COMMAND(cmd, sz, reply, pos) do { \
	guint32 _len; \
	_len = (sz) + 1; \
	(reply) = rspamd_fstring_sized_new (sizeof (_len) + _len); \
	(reply)->len = sizeof (_len) + _len; \
	_len = htonl (_len); \
	memcpy ((reply)->str, &_len, sizeof (_len)); \
	(reply)->str[sizeof(_len)] = (cmd); \
	(pos) = (guchar *)(reply)->str + sizeof (_len) + 1; \
} while (0)

gboolean
rspamd_milter_send_action (struct rspamd_milter_session *session,
		enum rspamd_milter_reply act, ...)
{
	guint32 ver, actions, protocol, idx;
	va_list ap;
	guchar cmd, *pos;
	rspamd_fstring_t *reply = NULL;
	GString *name, *value;
	struct rspamd_milter_outbuf *obuf;
	struct rspamd_milter_private *priv = session->priv;

	va_start (ap, act);
	cmd = act;

	switch (act) {
	case RSPAMD_MILTER_ACCEPT:
	case RSPAMD_MILTER_CONTINUE:
	case RSPAMD_MILTER_DISCARD:
	case RSPAMD_MILTER_PROGRESS:
	case RSPAMD_MILTER_REJECT:
	case RSPAMD_MILTER_TEMPFAIL:
		/* No additional arguments */
		SET_COMMAND (cmd, 0, reply, pos);
		break;
	case RSPAMD_MILTER_ADDHEADER:
		name = va_arg (ap, GString *);
		value = va_arg (ap, GString *);

		/* Name and value must be zero terminated */
		SET_COMMAND (cmd, name->len + value->len + 2, reply, pos);
		memcpy (pos, name->str, name->len + 1);
		pos += name->len + 1;
		memcpy (pos, value->str, value->len + 1);
		break;
	case RSPAMD_MILTER_CHGHEADER:
	case RSPAMD_MILTER_INSHEADER:
		idx = htonl (va_arg (ap, guint32));
		name = va_arg (ap, GString *);
		value = va_arg (ap, GString *);

		/* Name and value must be zero terminated */
		SET_COMMAND (cmd, name->len + value->len + 2 + sizeof (guint32),
				reply, pos);
		memcpy (pos, &idx, sizeof (idx));
		pos += sizeof (idx);
		memcpy (pos, name->str, name->len + 1);
		pos += name->len + 1;
		memcpy (pos, value->str, value->len + 1);
		break;
	case RSPAMD_MILTER_REPLYCODE:
	case RSPAMD_MILTER_ADDRCPT:
	case RSPAMD_MILTER_DELRCPT:
	case RSPAMD_MILTER_CHGFROM:
		/* Single GString * argument */
		value = va_arg (ap, GString *);
		SET_COMMAND (cmd, value->len + 1, reply, pos);
		memcpy (pos, value->str, value->len + 1);
		break;
	case RSPAMD_MILTER_OPTNEG:
		ver = htonl (va_arg (ap, guint32));
		actions = htonl (va_arg (ap, guint32));
		protocol = htonl (va_arg (ap, guint32));

		SET_COMMAND (cmd, sizeof (guint32) * 3, reply, pos);
		memcpy (pos, &ver, sizeof (ver));
		pos += sizeof (ver);
		memcpy (pos, &actions, sizeof (actions));
		pos += sizeof (actions);
		memcpy (pos,  &protocol, sizeof (protocol));
		break;
	default:
		msg_err_milter ("invalid command: %c", cmd);
		break;
	}

	va_end (ap);

	if (reply) {
		obuf = g_malloc (sizeof (*obuf));
		obuf->buf = reply;
		obuf->pos = 0;
		DL_APPEND (priv->out_chain, obuf);
		priv->state = RSPAMD_MILTER_WRITE_REPLY;
		rspamd_milter_plan_io (session, priv, EV_WRITE);

		return TRUE;
	}

	return FALSE;
}

gboolean
rspamd_milter_add_header (struct rspamd_milter_session *session,
		GString *name, GString *value)
{
	return rspamd_milter_send_action (session, RSPAMD_MILTER_ADDHEADER,
		name, value);
}

gboolean
rspamd_milter_del_header (struct rspamd_milter_session *session,
		GString *name)
{
	GString value;
	guint32 idx = 1;

	value.str = (gchar *)"";
	value.len = 0;

	return rspamd_milter_send_action (session, RSPAMD_MILTER_CHGHEADER,
			idx, name, value);
}

void
rspamd_milter_session_unref (struct rspamd_milter_session *session)
{
	REF_RELEASE (session);
}

struct rspamd_milter_session *
rspamd_milter_session_ref (struct rspamd_milter_session *session)
{
	REF_RETAIN (session);

	return session;
}

#define IF_MACRO(lit) RSPAMD_FTOK_ASSIGN (&srch, (lit)); \
	found = g_hash_table_lookup (session->macros, &srch); \
	if (found)

static void
rspamd_milter_macro_http (struct rspamd_milter_session *session,
		struct rspamd_http_message *msg)
{
	rspamd_ftok_t *found, srch;

	/*
	 * We assume postfix macros here, sendmail ones might be slightly
	 * different
	 */

	if (!session->macros) {
		return;
	}

	IF_MACRO("{i}") {
		rspamd_http_message_add_header_len (msg, QUEUE_ID_HEADER,
				found->begin, found->len);
	}
	else {
		IF_MACRO("i") {
			rspamd_http_message_add_header_len (msg, QUEUE_ID_HEADER,
					found->begin, found->len);
		}
	}

	IF_MACRO("{daemon_name}") {
		rspamd_http_message_add_header_len (msg, MTA_TAG_HEADER,
				found->begin, found->len);
	}

	IF_MACRO("{v}") {
		rspamd_http_message_add_header_len (msg, USER_AGENT_HEADER,
				found->begin, found->len);
	}
	else {
		IF_MACRO("v") {
			rspamd_http_message_add_header_len (msg, USER_AGENT_HEADER,
					found->begin, found->len);
		}
	}

	IF_MACRO("{cipher}") {
		rspamd_http_message_add_header_len (msg, TLS_CIPHER_HEADER,
				found->begin, found->len);
	}

	IF_MACRO("{tls_version}") {
		rspamd_http_message_add_header_len (msg, TLS_VERSION_HEADER,
				found->begin, found->len);
	}

	IF_MACRO("{auth_authen}") {
		rspamd_http_message_add_header_len (msg, USER_HEADER,
				found->begin, found->len);
	}

	if (!session->hostname || session->hostname->len == 0) {
		IF_MACRO("{client_name}") {
			rspamd_http_message_add_header_len (msg, HOSTNAME_HEADER,
					found->begin, found->len);
		}
	}

	IF_MACRO("{daemon_name}") {
		/* Postfix style */
		rspamd_http_message_add_header_len (msg, MTA_NAME_HEADER,
				found->begin, found->len);
	}
	else {
		/* Sendmail style */
		IF_MACRO("{j}") {
			rspamd_http_message_add_header_len (msg, MTA_NAME_HEADER,
					found->begin, found->len);
		}
		else {
			IF_MACRO("j") {
				rspamd_http_message_add_header_len (msg, MTA_NAME_HEADER,
						found->begin, found->len);
			}
		}
	}
}

struct rspamd_http_message *
rspamd_milter_to_http (struct rspamd_milter_session *session)
{
	struct rspamd_http_message *msg;
	guint i;
	struct rspamd_email_address *rcpt;

	g_assert (session != NULL);

	msg = rspamd_http_new_message (HTTP_REQUEST);

	msg->url = rspamd_fstring_assign (msg->url, "/" MSG_CMD_CHECK_V2,
			sizeof ("/" MSG_CMD_CHECK_V2) - 1);

	if (session->message) {
		rspamd_http_message_set_body_from_fstring_steal (msg, session->message);
		session->message = NULL;
	}

	if (session->hostname && session->hostname->len > 0) {
		rspamd_http_message_add_header_fstr (msg, HOSTNAME_HEADER,
				session->hostname);
	}

	if (session->helo && session->helo->len > 0) {
		rspamd_http_message_add_header_fstr (msg, HELO_HEADER,
				session->helo);
	}

	if (session->from) {
		rspamd_http_message_add_header_len (msg, FROM_HEADER,
				session->from->raw, session->from->raw_len);
	}

	if (session->rcpts) {
		PTR_ARRAY_FOREACH (session->rcpts, i, rcpt) {
			rspamd_http_message_add_header_len (msg, RCPT_HEADER,
					rcpt->raw, rcpt->raw_len);
		}
	}

	if (session->addr) {
		if (rspamd_inet_address_get_af (session->addr) != AF_UNIX) {
			rspamd_http_message_add_header (msg, IP_ADDR_HEADER,
					rspamd_inet_address_to_string_pretty (session->addr));
		}
		else {
			rspamd_http_message_add_header (msg, IP_ADDR_HEADER,
					rspamd_inet_address_to_string (session->addr));
		}
	}

	rspamd_milter_macro_http (session, msg);
	rspamd_http_message_add_header (msg, MILTER_HEADER, "Yes");

	return msg;
}

void *
rspamd_milter_update_userdata (struct rspamd_milter_session *session,
		void *ud)
{
	struct rspamd_milter_private *priv = session->priv;
	void *prev_ud;

	prev_ud = priv->ud;
	priv->ud = ud;

	return prev_ud;
}

static void
rspamd_milter_remove_header_safe (struct rspamd_milter_session *session,
		const gchar *key, gint nhdr)
{
	gint saved_nhdr, i;
	gpointer found;
	GString *hname, *hvalue;
	struct rspamd_milter_private *priv = session->priv;

	found = g_hash_table_lookup (priv->headers, key);

	if (found) {
		saved_nhdr = GPOINTER_TO_INT (found);

		hname = g_string_new (key);
		hvalue = g_string_new ("");

		if (nhdr >= 1) {
			rspamd_milter_send_action (session,
					RSPAMD_MILTER_CHGHEADER,
					nhdr, hname, hvalue);
		}
		else if (nhdr == 0) {
			/* We need to clear all headers */
			for (i = 1; i <= saved_nhdr; i ++) {
				rspamd_milter_send_action (session,
						RSPAMD_MILTER_CHGHEADER,
						i, hname, hvalue);
			}
		}
		else {
			/* Remove from the end */
			if (nhdr >= -saved_nhdr) {
				rspamd_milter_send_action (session,
						RSPAMD_MILTER_CHGHEADER,
						saved_nhdr + nhdr + 1, hname, hvalue);
			}
		}

		g_string_free (hname, TRUE);
		g_string_free (hvalue, TRUE);
	}
}

/*
 * Returns `TRUE` if action has been processed internally by this function
 */
static gboolean
rspamd_milter_process_milter_block (struct rspamd_milter_session *session,
		const ucl_object_t *obj, gint action)
{
	const ucl_object_t *elt, *cur, *cur_elt;
	ucl_object_iter_t it;
	struct rspamd_milter_private *priv = session->priv;
	GString *hname, *hvalue;
	gint idx = -1;

	if (obj && ucl_object_type (obj) == UCL_OBJECT) {
		elt = ucl_object_lookup (obj, "remove_headers");
		/*
		 * remove_headers:  {"name": 1, ... }
		 * where number is the header's position starting from '1'
		 */
		if (elt && ucl_object_type (elt) == UCL_OBJECT) {
			it = NULL;

			while ((cur = ucl_object_iterate (elt, &it, true)) != NULL) {
				if (ucl_object_type (cur) == UCL_INT) {
					rspamd_milter_remove_header_safe (session,
							ucl_object_key (cur),
							ucl_object_toint (cur));
				}
			}
		}

		elt = ucl_object_lookup (obj, "add_headers");
		/*
		 * add_headers: {"name": "value", ... }
		 * name could have multiple values
		 * -or- (since 1.7)
		 * {"name": {"value": "val", "order": 0}, ... }
		 */
		if (elt && ucl_object_type (elt) == UCL_OBJECT) {
			it = NULL;

			while ((cur = ucl_object_iterate (elt, &it, true)) != NULL) {
				LL_FOREACH (cur, cur_elt) {
					if (ucl_object_type (cur_elt) == UCL_STRING) {
						hname = g_string_new (ucl_object_key (cur));
						hvalue = g_string_new (ucl_object_tostring (cur_elt));

						rspamd_milter_send_action (session,
								RSPAMD_MILTER_ADDHEADER,
								hname, hvalue);
						g_string_free (hname, TRUE);
						g_string_free (hvalue, TRUE);
					}
					else if (ucl_object_type (cur_elt) == UCL_OBJECT) {
						const ucl_object_t *val;

						val = ucl_object_lookup (cur_elt, "value");

						if (val && ucl_object_type (val) == UCL_STRING) {
							const ucl_object_t *idx_obj;

							idx_obj = ucl_object_lookup_any (cur_elt, "order",
									"index", NULL);
							if (idx_obj) {
								idx = ucl_object_toint (idx_obj);
							}

							hname = g_string_new (ucl_object_key (cur));
							hvalue = g_string_new (ucl_object_tostring (val));

							if (idx >= 0) {
								rspamd_milter_send_action (session,
										RSPAMD_MILTER_INSHEADER,
										idx,
										hname, hvalue);
							}
							else {
								rspamd_milter_send_action (session,
										RSPAMD_MILTER_ADDHEADER,
										hname, hvalue);
							}

							g_string_free (hname, TRUE);
							g_string_free (hvalue, TRUE);
						}
					}
				}
			}
		}

		elt = ucl_object_lookup (obj, "change_from");

		if (elt && ucl_object_type (elt) == UCL_STRING) {
			hvalue = g_string_new (ucl_object_tostring (elt));
			rspamd_milter_send_action (session,
					RSPAMD_MILTER_CHGFROM,
					hvalue);
			g_string_free (hvalue, TRUE);
		}

		elt = ucl_object_lookup (obj, "reject");

		if (elt && ucl_object_type (elt) == UCL_STRING) {
			if (strcmp (ucl_object_tostring (elt), "discard") == 0) {
				priv->discard_on_reject = TRUE;
				msg_info_milter ("discard message instead of rejection");
			}
			else {
				priv->discard_on_reject = FALSE;
			}
		}

		elt = ucl_object_lookup (obj, "no_action");

		if (elt && ucl_object_type (elt) == UCL_BOOLEAN) {
			priv->no_action = ucl_object_toboolean (elt);
		}
	}

	if (action == METRIC_ACTION_ADD_HEADER) {
		elt = ucl_object_lookup (obj, "spam_header");

		if (elt) {
			if (ucl_object_type (elt) == UCL_STRING) {
				rspamd_milter_remove_header_safe (session,
						milter_ctx->spam_header,
						0);

				hname = g_string_new (milter_ctx->spam_header);
				hvalue = g_string_new (ucl_object_tostring (elt));
				rspamd_milter_send_action (session, RSPAMD_MILTER_CHGHEADER,
						(guint32)1, hname, hvalue);
				g_string_free (hname, TRUE);
				g_string_free (hvalue, TRUE);
				rspamd_milter_send_action (session, RSPAMD_MILTER_ACCEPT);

				return TRUE;
			}
			else if (ucl_object_type (elt) == UCL_OBJECT) {
				it = NULL;

				while ((cur = ucl_object_iterate (elt, &it, true)) != NULL) {
					rspamd_milter_remove_header_safe (session,
							ucl_object_key (cur),
							0);

					hname = g_string_new (ucl_object_key (cur));
					hvalue = g_string_new (ucl_object_tostring (cur));
					rspamd_milter_send_action (session, RSPAMD_MILTER_CHGHEADER,
							(guint32) 1, hname, hvalue);
					g_string_free (hname, TRUE);
					g_string_free (hvalue, TRUE);
				}

				rspamd_milter_send_action (session, RSPAMD_MILTER_ACCEPT);

				return TRUE;
			}
		}
	}

	return FALSE;
}

void
rspamd_milter_send_task_results (struct rspamd_milter_session *session,
		const ucl_object_t *results)
{
	const ucl_object_t *elt;
	struct rspamd_milter_private *priv = session->priv;
	const gchar *str_action;
	gint action = METRIC_ACTION_REJECT;
	rspamd_fstring_t *xcode = NULL, *rcode = NULL, *reply = NULL;
	GString *hname, *hvalue;
	gboolean processed = FALSE;

	if (results == NULL) {
		msg_err_milter ("cannot find scan results, tempfail");
		rspamd_milter_send_action (session, RSPAMD_MILTER_TEMPFAIL);

		return;
	}

	elt = ucl_object_lookup (results, "action");

	if (!elt) {
		msg_err_milter ("cannot find action in results, tempfail");
		rspamd_milter_send_action (session, RSPAMD_MILTER_TEMPFAIL);

		return;
	}

	str_action = ucl_object_tostring (elt);
	rspamd_action_from_str (str_action, &action);

	elt = ucl_object_lookup (results, "messages");
	if (elt) {
		const ucl_object_t *smtp_res;
		const gchar *msg;
		gsize len = 0;

		smtp_res = ucl_object_lookup (elt, "smtp_message");

		if (smtp_res) {
			msg = ucl_object_tolstring (smtp_res, &len);
			reply = rspamd_fstring_new_init (msg, len);
		}
	}

	/* Deal with milter headers */
	elt = ucl_object_lookup (results, "milter");

	if (elt) {
		processed = rspamd_milter_process_milter_block (session, elt, action);
	}

	/* DKIM-Signature */
	elt = ucl_object_lookup (results, "dkim-signature");

	if (elt) {
		hname = g_string_new (RSPAMD_MILTER_DKIM_HEADER);
		hvalue = g_string_new (ucl_object_tostring (elt));

		rspamd_milter_send_action (session, RSPAMD_MILTER_INSHEADER,
				1, hname, hvalue);
		g_string_free (hname, TRUE);
		g_string_free (hvalue, TRUE);
	}

	if (processed) {
		goto cleanup;
	}

	if (priv->no_action) {
		msg_info_milter ("do not apply action %s, no_action is set",
				str_action);
		hname = g_string_new (RSPAMD_MILTER_ACTION_HEADER);
		hvalue = g_string_new (str_action);

		rspamd_milter_send_action (session, RSPAMD_MILTER_ADDHEADER,
				hname, hvalue);
		g_string_free (hname, TRUE);
		g_string_free (hvalue, TRUE);
		rspamd_milter_send_action (session, RSPAMD_MILTER_ACCEPT);

		goto cleanup;
	}

	switch (action) {
	case METRIC_ACTION_REJECT:
		if (priv->discard_on_reject) {
			rspamd_milter_send_action (session, RSPAMD_MILTER_DISCARD);
		}
		else {
			rcode = rspamd_fstring_new_init (RSPAMD_MILTER_RCODE_REJECT,
					sizeof (RSPAMD_MILTER_RCODE_REJECT) - 1);
			xcode = rspamd_fstring_new_init (RSPAMD_MILTER_XCODE_REJECT,
					sizeof (RSPAMD_MILTER_XCODE_REJECT) - 1);

			if (!reply) {
				reply = rspamd_fstring_new_init (RSPAMD_MILTER_REJECT_MESSAGE,
						sizeof (RSPAMD_MILTER_REJECT_MESSAGE) - 1);
			}

			rspamd_milter_set_reply (session, rcode, xcode, reply);
			rspamd_milter_send_action (session, RSPAMD_MILTER_REJECT);
		}
		break;
	case METRIC_ACTION_SOFT_REJECT:
		rcode = rspamd_fstring_new_init (RSPAMD_MILTER_RCODE_TEMPFAIL,
				sizeof (RSPAMD_MILTER_RCODE_TEMPFAIL) - 1);
		xcode = rspamd_fstring_new_init (RSPAMD_MILTER_XCODE_TEMPFAIL,
				sizeof (RSPAMD_MILTER_XCODE_TEMPFAIL) - 1);

		if (!reply) {
			reply = rspamd_fstring_new_init (RSPAMD_MILTER_TEMPFAIL_MESSAGE,
					sizeof (RSPAMD_MILTER_TEMPFAIL_MESSAGE) - 1);
		}

		rspamd_milter_set_reply (session, rcode, xcode, reply);
		rspamd_milter_send_action (session, RSPAMD_MILTER_REJECT);
		break;

	case METRIC_ACTION_REWRITE_SUBJECT:
		elt = ucl_object_lookup (results, "subject");

		if (elt) {
			hname = g_string_new ("Subject");
			hvalue = g_string_new (ucl_object_tostring (elt));

			rspamd_milter_send_action (session, RSPAMD_MILTER_CHGHEADER,
					(guint32)1, hname, hvalue);
			g_string_free (hname, TRUE);
			g_string_free (hvalue, TRUE);
		}

		rspamd_milter_send_action (session, RSPAMD_MILTER_ACCEPT);
		break;

	case METRIC_ACTION_ADD_HEADER:
		/* Remove existing headers */
		rspamd_milter_remove_header_safe (session,
				milter_ctx->spam_header,
				0);

		hname = g_string_new (milter_ctx->spam_header);
		hvalue = g_string_new ("Yes");
		rspamd_milter_send_action (session, RSPAMD_MILTER_CHGHEADER,
				(guint32)1, hname, hvalue);
		g_string_free (hname, TRUE);
		g_string_free (hvalue, TRUE);
		rspamd_milter_send_action (session, RSPAMD_MILTER_ACCEPT);
		break;

	case METRIC_ACTION_GREYLIST:
	case METRIC_ACTION_NOACTION:
	default:
		rspamd_milter_send_action (session, RSPAMD_MILTER_ACCEPT);
		break;
	}

cleanup:
	rspamd_fstring_free (rcode);
	rspamd_fstring_free (xcode);
	rspamd_fstring_free (reply);
}

void
rspamd_milter_init_library (const gchar *spam_header, void *sessions_cache,
		gboolean discard_on_reject)
{
	if (milter_ctx) {
		g_free (milter_ctx->spam_header);
		g_free (milter_ctx);
	}

	milter_ctx = g_malloc (sizeof (*milter_ctx));

	if (spam_header) {
		milter_ctx->spam_header = g_strdup (spam_header);
	}
	else {
		milter_ctx->spam_header = g_strdup (RSPAMD_MILTER_SPAM_HEADER);
	}

	milter_ctx->sessions_cache = sessions_cache;
	milter_ctx->discard_on_reject = discard_on_reject;
}

rspamd_mempool_t *
rspamd_milter_get_session_pool (struct rspamd_milter_session *session)
{
	struct rspamd_milter_private *priv = session->priv;

	return priv->pool;
}
