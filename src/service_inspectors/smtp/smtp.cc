//--------------------------------------------------------------------------
// Copyright (C) 2015-2015 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

#include "smtp.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#include "snort_types.h"
#include "snort_debug.h"

#include "smtp_module.h"
#include "profiler.h"
#include "stream/stream_api.h"
#include "file_api/file_api.h"
#include "parser.h"
#include "framework/inspector.h"
#include "utils/sfsnprintfappend.h"
#include "target_based/sftarget_protocol_reference.h"
#include "smtp_paf.h"
#include "smtp_util.h"
#include "smtp_normalize.h"
#include "smtp_xlink2state.h"
#include "sf_email_attach_decode.h"
#include "protocols/ssl.h"
#include "unified2_common.h"
#include "detection/detection_util.h"

THREAD_LOCAL ProfileStats smtpPerfStats;
THREAD_LOCAL SimpleStats smtpstats;
char smtp_normalizing;

/* Globals ****************************************************************/

const SMTPToken smtp_known_cmds[] =
{
    { "ATRN",          4, CMD_ATRN, SMTP_CMD_TYPE_NORMAL },
    { "AUTH",          4, CMD_AUTH, SMTP_CMD_TYPE_AUTH },
    { "BDAT",          4, CMD_BDAT, SMTP_CMD_TYPE_BDATA },
    { "DATA",          4, CMD_DATA, SMTP_CMD_TYPE_DATA },
    { "DEBUG",         5, CMD_DEBUG, SMTP_CMD_TYPE_NORMAL },
    { "EHLO",          4, CMD_EHLO, SMTP_CMD_TYPE_NORMAL },
    { "EMAL",          4, CMD_EMAL, SMTP_CMD_TYPE_NORMAL },
    { "ESAM",          4, CMD_ESAM, SMTP_CMD_TYPE_NORMAL },
    { "ESND",          4, CMD_ESND, SMTP_CMD_TYPE_NORMAL },
    { "ESOM",          4, CMD_ESOM, SMTP_CMD_TYPE_NORMAL },
    { "ETRN",          4, CMD_ETRN, SMTP_CMD_TYPE_NORMAL },
    { "EVFY",          4, CMD_EVFY, SMTP_CMD_TYPE_NORMAL },
    { "EXPN",          4, CMD_EXPN, SMTP_CMD_TYPE_NORMAL },
    { "HELO",          4, CMD_HELO, SMTP_CMD_TYPE_NORMAL },
    { "HELP",          4, CMD_HELP, SMTP_CMD_TYPE_NORMAL },
    { "IDENT",         5, CMD_IDENT, SMTP_CMD_TYPE_NORMAL },
    { "MAIL",          4, CMD_MAIL, SMTP_CMD_TYPE_NORMAL },
    { "NOOP",          4, CMD_NOOP, SMTP_CMD_TYPE_NORMAL },
    { "ONEX",          4, CMD_ONEX, SMTP_CMD_TYPE_NORMAL },
    { "QUEU",          4, CMD_QUEU, SMTP_CMD_TYPE_NORMAL },
    { "QUIT",          4, CMD_QUIT, SMTP_CMD_TYPE_NORMAL },
    { "RCPT",          4, CMD_RCPT, SMTP_CMD_TYPE_NORMAL },
    { "RSET",          4, CMD_RSET, SMTP_CMD_TYPE_NORMAL },
    { "SAML",          4, CMD_SAML, SMTP_CMD_TYPE_NORMAL },
    { "SEND",          4, CMD_SEND, SMTP_CMD_TYPE_NORMAL },
    { "SIZE",          4, CMD_SIZE, SMTP_CMD_TYPE_NORMAL },
    { "STARTTLS",      8, CMD_STARTTLS, SMTP_CMD_TYPE_NORMAL },
    { "SOML",          4, CMD_SOML, SMTP_CMD_TYPE_NORMAL },
    { "TICK",          4, CMD_TICK, SMTP_CMD_TYPE_NORMAL },
    { "TIME",          4, CMD_TIME, SMTP_CMD_TYPE_NORMAL },
    { "TURN",          4, CMD_TURN, SMTP_CMD_TYPE_NORMAL },
    { "TURNME",        6, CMD_TURNME, SMTP_CMD_TYPE_NORMAL },
    { "VERB",          4, CMD_VERB, SMTP_CMD_TYPE_NORMAL },
    { "VRFY",          4, CMD_VRFY, SMTP_CMD_TYPE_NORMAL },
    { "X-EXPS",        6, CMD_X_EXPS, SMTP_CMD_TYPE_AUTH },
    { "XADR",          4, CMD_XADR, SMTP_CMD_TYPE_NORMAL },
    { "XAUTH",         5, CMD_XAUTH, SMTP_CMD_TYPE_AUTH },
    { "XCIR",          4, CMD_XCIR, SMTP_CMD_TYPE_NORMAL },
    { "XEXCH50",       7, CMD_XEXCH50, SMTP_CMD_TYPE_BDATA },
    { "XGEN",          4, CMD_XGEN, SMTP_CMD_TYPE_NORMAL },
    { "XLICENSE",      8, CMD_XLICENSE, SMTP_CMD_TYPE_NORMAL },
    { "X-LINK2STATE", 12, CMD_X_LINK2STATE, SMTP_CMD_TYPE_NORMAL },
    { "XQUE",          4, CMD_XQUE, SMTP_CMD_TYPE_NORMAL },
    { "XSTA",          4, CMD_XSTA, SMTP_CMD_TYPE_NORMAL },
    { "XTRN",          4, CMD_XTRN, SMTP_CMD_TYPE_NORMAL },
    { "XUSR",          4, CMD_XUSR, SMTP_CMD_TYPE_NORMAL },
    { "*",             1, CMD_ABORT, SMTP_CMD_TYPE_NORMAL },
    { NULL,            0, 0, SMTP_CMD_TYPE_NORMAL }
};

const SMTPToken smtp_resps[] =
{
    { "220",  3,  RESP_220, SMTP_CMD_TYPE_NORMAL },  /* Service ready - initial response and
                                                        STARTTLS response */
    { "221",  3,  RESP_221, SMTP_CMD_TYPE_NORMAL },  /* Goodbye - response to QUIT */
    { "235",  3,  RESP_235, SMTP_CMD_TYPE_NORMAL },  /* Auth done response */
    { "250",  3,  RESP_250, SMTP_CMD_TYPE_NORMAL },  /* Requested mail action okay, completed */
    { "334",  3,  RESP_334, SMTP_CMD_TYPE_NORMAL },  /* Auth intermediate response */
    { "354",  3,  RESP_354, SMTP_CMD_TYPE_NORMAL },  /* Start mail input - data response */
    { "421",  3,  RESP_421, SMTP_CMD_TYPE_NORMAL },  /* Service not availiable - closes connection
                                                        */
    { "450",  3,  RESP_450, SMTP_CMD_TYPE_NORMAL },  /* Mailbox unavailable */
    { "451",  3,  RESP_451, SMTP_CMD_TYPE_NORMAL },  /* Local error in processing */
    { "452",  3,  RESP_452, SMTP_CMD_TYPE_NORMAL },  /* Insufficient system storage */
    { "500",  3,  RESP_500, SMTP_CMD_TYPE_NORMAL },  /* Command unrecognized */
    { "501",  3,  RESP_501, SMTP_CMD_TYPE_NORMAL },  /* Syntax error in parameters or arguments */
    { "502",  3,  RESP_502, SMTP_CMD_TYPE_NORMAL },  /* Command not implemented */
    { "503",  3,  RESP_503, SMTP_CMD_TYPE_NORMAL },  /* Bad sequence of commands */
    { "504",  3,  RESP_504, SMTP_CMD_TYPE_NORMAL },  /* Command parameter not implemented */
    { "535",  3,  RESP_535, SMTP_CMD_TYPE_NORMAL },  /* Authentication failed */
    { "550",  3,  RESP_550, SMTP_CMD_TYPE_NORMAL },  /* Action not taken - mailbox unavailable */
    { "551",  3,  RESP_551, SMTP_CMD_TYPE_NORMAL },  /* User not local; please try <forward-path>
                                                        */
    { "552",  3,  RESP_552, SMTP_CMD_TYPE_NORMAL },  /* Mail action aborted: exceeded storage
                                                        allocation */
    { "553",  3,  RESP_553, SMTP_CMD_TYPE_NORMAL },  /* Action not taken: mailbox name not allowed
                                                        */
    { "554",  3,  RESP_554, SMTP_CMD_TYPE_NORMAL },  /* Transaction failed */
    { NULL,   0,  0, SMTP_CMD_TYPE_NORMAL }
};

typedef struct _SMTPAuth
{
    const char* name;
    int name_len;
} SMTPAuth;

/* Cyrus SASL authentication mechanisms ANONYMOUS, PLAIN and LOGIN
 * does not have context
 */
const SMTPAuth smtp_auth_no_ctx[] =
{
    { "ANONYMOUS", 9 },
    { "PLAIN", 5 },
    { "LOGIN", 5 },
    { NULL, 0 }
};

SearchTool* smtp_resp_search_mpse = nullptr;

SMTPSearch smtp_resp_search[RESP_LAST];
THREAD_LOCAL const SMTPSearch* smtp_current_search = NULL;
THREAD_LOCAL SMTPSearchInfo smtp_search_info;

static void snort_smtp(SMTP_PROTO_CONF* GlobalConf, Packet* p);
static void SMTP_ResetState(void*);
void SMTP_DecodeAlert(void* ds);

static int SMTP_HandleHeaderLine(void* conf, void* pkt, const uint8_t* ptr, const uint8_t* eol,
    int max_header_len, void* ssn);
static int SMTP_NormalizeData(void* conf, void* pkt, const uint8_t* ptr, const uint8_t* data_end);

MimeMethods smtp_mime_methods = { SMTP_HandleHeaderLine, SMTP_NormalizeData, SMTP_DecodeAlert,
                                  SMTP_ResetState, smtp_is_data_end };

unsigned SmtpFlowData::flow_id = 0;
static SMTPData* get_session_data(Flow* flow)
{
    SmtpFlowData* fd = (SmtpFlowData*)flow->get_application_data(
        SmtpFlowData::flow_id);

    return fd ? &fd->session : NULL;
}

SMTPData* SetNewSMTPData(SMTP_PROTO_CONF* config, Packet* p)
{
    SMTPData* smtp_ssn;
    SmtpFlowData* fd = new SmtpFlowData;

    p->flow->set_application_data(fd);
    smtp_ssn = &fd->session;

    smtp_ssn->mime_ssn.log_config = &(config->log_config);
    smtp_ssn->mime_ssn.decode_conf = &(config->decode_conf);
    smtp_ssn->mime_ssn.methods = &(smtp_mime_methods);
    smtp_ssn->mime_ssn.config = config;
    if (file_api->set_log_buffers(&(smtp_ssn->mime_ssn.log_state), &(config->log_config)) < 0)
    {
        return NULL;
    }

    if(stream.is_midstream(p->flow))
    {
        DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Got midstream packet - "
            "setting state to unknown\n"); );
        smtp_ssn->state = STATE_UNKNOWN;
    }

    return smtp_ssn;
}

void SMTP_DecodeAlert(void* ds)
{
    Email_DecodeState* decode_state = (Email_DecodeState*)ds;
    switch ( decode_state->decode_type )
    {
    case DECODE_B64:
        SnortEventqAdd(GID_SMTP, SMTP_B64_DECODING_FAILED);
        break;
    case DECODE_QP:
        SnortEventqAdd(GID_SMTP, SMTP_QP_DECODING_FAILED);
        break;
    case DECODE_UU:
        SnortEventqAdd(GID_SMTP, SMTP_UU_DECODING_FAILED);
        break;

    default:
        break;
    }
}

void SMTP_InitCmds(SMTP_PROTO_CONF* config)
{
    const SMTPToken* tmp;

    if (config == NULL)
        return;

    /* add one to CMD_LAST for NULL entry */
    config->cmds = (SMTPToken*)calloc(CMD_LAST + 1, sizeof(SMTPToken));
    if (config->cmds == NULL)
    {
        FatalError("Could not allocate memory for SMTP Command structure.\n");
    }

    for (tmp = &smtp_known_cmds[0]; tmp->name != NULL; tmp++)
    {
        config->cmds[tmp->search_id].name_len = tmp->name_len;
        config->cmds[tmp->search_id].search_id = tmp->search_id;
        config->cmds[tmp->search_id].name = strdup(tmp->name);
        config->cmds[tmp->search_id].type = tmp->type;

        if (config->cmds[tmp->search_id].name == NULL)
        {
            FatalError("Could not allocate memory for SMTP Command structure.\n");
        }
    }

    /* initialize memory for command searches */
    config->cmd_search = (SMTPSearch*)calloc(CMD_LAST, sizeof(SMTPSearch));
    if (config->cmd_search == NULL)
    {
        FatalError("Could not allocate memory for SMTP Command Structure.\n");
    }

    config->num_cmds = CMD_LAST;
}

void SMTP_CommandSearchInit(SMTP_PROTO_CONF* config)
{
    const SMTPToken* tmp;
    config->cmd_search_mpse = new SearchTool();
    if (config->cmd_search_mpse == NULL)
    {
        FatalError("Could not allocate memory for SMTP Command search.\n");
    }
    for (tmp = config->cmds; tmp->name != NULL; tmp++)
    {
        config->cmd_search[tmp->search_id].name = (char *)tmp->name;
        config->cmd_search[tmp->search_id].name_len = tmp->name_len;
        config->cmd_search_mpse->add(tmp->name, tmp->name_len, tmp->search_id);
    }

    config->cmd_search_mpse->prep();
}

void SMTP_ResponseSearchInit(void)
{
    const SMTPToken* tmp;
    smtp_resp_search_mpse = new SearchTool();
    if (smtp_resp_search_mpse == NULL)
    {
        FatalError("Could not allocate memory for SMTP Response search.\n");
    }
    for (tmp = &smtp_resps[0]; tmp->name != NULL; tmp++)
    {
        smtp_resp_search[tmp->search_id].name = (char *)tmp->name;
        smtp_resp_search[tmp->search_id].name_len = tmp->name_len;
        smtp_resp_search_mpse->add(tmp->name, tmp->name_len, tmp->search_id);
    }
    smtp_resp_search_mpse->prep();
}

void SMTP_SearchFree(void)
{
    if (smtp_resp_search_mpse != NULL)
        delete smtp_resp_search_mpse;
}

static int AddCmd(SMTP_PROTO_CONF* config, const char* name, SMTPCmdTypeEnum type)
{
    SMTPToken* cmds, * tmp_cmds;
    SMTPSearch* cmd_search;
    SMTPCmdConfig* cmd_config;
    int ret;

    config->num_cmds++;

    /* allocate enough memory for new commmand - alloc one extra for NULL entry */
    cmds = (SMTPToken*)calloc(config->num_cmds + 1, sizeof(SMTPToken));
    if (cmds == NULL)
    {
        FatalError("Failed to allocate memory for SMTP command structure\n");
    }

    /* This gets filled in later */
    cmd_search = (SMTPSearch*)calloc(config->num_cmds, sizeof(SMTPSearch));
    if (cmd_search == NULL)
    {
        FatalError("Failed to allocate memory for SMTP command structure\n");
    }

    cmd_config = (SMTPCmdConfig*)calloc(config->num_cmds, sizeof(SMTPCmdConfig));
    if (cmd_config == NULL)
    {
        FatalError("Failed to allocate memory for SMTP command structure\n");
    }

    /* copy existing commands into newly allocated memory
     *      * don't need to copy anything from cmd_search since this hasn't been initialized yet */
    ret = SafeMemcpy(cmds, config->cmds, (config->num_cmds - 1) * sizeof(SMTPToken),
        cmds, cmds + (config->num_cmds - 1));

    if (ret != SAFEMEM_SUCCESS)
    {
        FatalError("Failed to memory copy SMTP command structure\n");
    }

    ret = SafeMemcpy(cmd_config, config->cmd_config, (config->num_cmds - 1) *
        sizeof(SMTPCmdConfig),
        cmd_config, cmd_config + (config->num_cmds - 1));

    if (ret != SAFEMEM_SUCCESS)
    {
        FatalError("Failed to memory copy SMTP command structure\n");
    }

    /* add new command to cmds
     *      * cmd_config doesn't need anything added - this will probably be done by a calling function
     *           * cmd_search will be initialized when the searches are initialized */
    tmp_cmds = &cmds[config->num_cmds - 1];
    tmp_cmds->name = strdup(name);
    tmp_cmds->name_len = strlen(name);
    tmp_cmds->search_id = config->num_cmds - 1;
    if (type)
        tmp_cmds->type = type;

    if (tmp_cmds->name == NULL)
    {
        FatalError("Failed to allocate memory for SMTP command structure\n");
    }

    /* free global memory structures */
    if (config->cmds != NULL)
        free(config->cmds);

    if (config->cmd_search != NULL)
        free(config->cmd_search);

    if (config->cmd_config != NULL)
        free(config->cmd_config);

    /* set globals to new memory */
    config->cmds = cmds;
    config->cmd_search = cmd_search;
    config->cmd_config = cmd_config;

    return (config->num_cmds - 1);
}

/* Return id associated with a given command string */
static int GetCmdId(SMTP_PROTO_CONF* config, const char* name, SMTPCmdTypeEnum type)
{
    SMTPToken* cmd;

    for (cmd = config->cmds; cmd->name != NULL; cmd++)
    {
        if (strcasecmp(cmd->name, name) == 0)
        {
            if (type && (type != cmd->type))
                cmd->type = type;

            return cmd->search_id;
        }
    }

    return AddCmd(config, name, type);
}

void ProcessSmtpCmdsList(SMTP_PROTO_CONF* config, const SmtpCmd* sc)
{
    const char* cmd = sc->name.c_str();
    int id;
    SMTPCmdTypeEnum type;

    if ( sc->flags & PCMD_AUTH )
        type = SMTP_CMD_TYPE_AUTH;

    else if (  sc->flags & PCMD_BDATA )
        type = SMTP_CMD_TYPE_BDATA;

    else if (  sc->flags & PCMD_DATA )
        type = SMTP_CMD_TYPE_DATA;

    else
        type = SMTP_CMD_TYPE_NORMAL;

    id = GetCmdId(config, cmd, type);
    if (  sc->flags & PCMD_INVALID )
        config->cmd_config[id].alert = true;

    else if ( sc->flags & PCMD_NORM )
        config->cmd_config[id].normalize = true;

    else
        config->cmd_config[id].alert = false;

    if ( sc->flags & PCMD_ALT )
        config->cmd_config[id].max_line_len = sc->number;
}

void SMTP_PrintConfig(SMTP_PROTO_CONF *config)
{
    const SMTPToken* cmd;
    char buf[8192];
    int max_line_len_count = 0;
    int max_line_len = 0;
    int alert_count = 0;

    if (config == NULL)
        return;

    memset(&buf[0], 0, sizeof(buf));

    LogMessage("SMTP Config:\n");
    snprintf(buf, sizeof(buf) - 1, "    Normalize: ");

    if(config->normalize == NORMALIZE_ALL)
        sfsnprintfappend(buf, sizeof(buf) - 1, "all");
    else if(config->normalize == NORMALIZE_NONE)
        sfsnprintfappend(buf, sizeof(buf) - 1, "none");
    else if(config->normalize == NORMALIZE_CMDS)
    {
        for (cmd = config->cmds; cmd->name != NULL; cmd++)
        {
            if (config->cmd_config[cmd->search_id].normalize)
            {
                sfsnprintfappend(buf, sizeof(buf) - 1, "%s ", cmd->name);
            }
        }
    }

    LogMessage("%s\n", buf);

    LogMessage("    Ignore Data: %s\n",
        config->decode_conf.ignore_data ? "Yes" : "No");
    LogMessage("    Ignore TLS Data: %s\n",
        config->ignore_tls_data ? "Yes" : "No");
    snprintf(buf, sizeof(buf) - 1, "    Max Command Line Length: ");

    if (config->max_command_line_len == 0)
        sfsnprintfappend(buf, sizeof(buf) - 1, "Unlimited");
    else
        sfsnprintfappend(buf, sizeof(buf) - 1, "%d", config->max_command_line_len);

    LogMessage("%s\n", buf);

    {
        snprintf(buf, sizeof(buf) - 1, "    Max Specific Command Line Length: ");

        for (cmd = config->cmds; cmd->name != NULL; cmd++)
        {
            max_line_len = config->cmd_config[cmd->search_id].max_line_len;

            if (max_line_len != 0)
            {
                if (max_line_len_count % 5 == 0)
                {
                    LogMessage("%s\n", buf);
                    snprintf(buf, sizeof(buf) - 1, "       %s:%d ", cmd->name, max_line_len);
                }
                else
                {
                    sfsnprintfappend(buf, sizeof(buf) - 1, "%s:%d ", cmd->name, max_line_len);
                }

                max_line_len_count++;
            }
        }

        if (max_line_len_count == 0)
            LogMessage("%sNone\n", buf);
        else
            LogMessage("%s\n", buf);
    }
    snprintf(buf, sizeof(buf) - 1, "    Max Header Line Length: ");

    if (config->max_header_line_len == 0)
        LogMessage("%sUnlimited\n", buf);
    else
        LogMessage("%s%d\n", buf, config->max_header_line_len);

    snprintf(buf, sizeof(buf) - 1, "    Max Response Line Length: ");

    if (config->max_response_line_len == 0)
        LogMessage("%sUnlimited\n", buf);
    else
        LogMessage("%s%d\n", buf, config->max_response_line_len);

    LogMessage("    X-Link2State Enabled: %s\n",
        (config->xlink2state == ALERT_XLINK2STATE) ? "Yes" : "No");
    if (config->xlink2state == DROP_XLINK2STATE)
    {
        LogMessage("    Drop on X-Link2State Alert: %s\n", "Yes" );
    }
    else
    {
        LogMessage("    Drop on X-Link2State Alert: %s\n", "No" );
    }

    snprintf(buf, sizeof(buf) - 1, "    Alert on commands: ");

    for (cmd = config->cmds; cmd->name != NULL; cmd++)
    {
        if (config->cmd_config[cmd->search_id].alert)
        {
            sfsnprintfappend(buf, sizeof(buf) - 1, "%s ", cmd->name);
            alert_count++;
        }
    }

    if (alert_count == 0)
    {
        LogMessage("%sNone\n", buf);
    }
    else
    {
        LogMessage("%s\n", buf);
    }
    if (config->decode_conf.b64_depth > -1)
    {
        LogMessage("    Base64 Decoding: %s\n", "Enabled");
        switch (config->decode_conf.b64_depth)
        {
        case 0:
            LogMessage("    Base64 Decoding Depth: %s\n", "Unlimited");
            break;
        default:
            LogMessage("    Base64 Decoding Depth: %d\n", config->decode_conf.b64_depth);
            break;
        }
    }
    else
        LogMessage("    Base64 Decoding: %s\n", "Disabled");

    if (config->decode_conf.qp_depth > -1)
    {
        LogMessage("    Quoted-Printable Decoding: %s\n","Enabled");
        switch (config->decode_conf.qp_depth)
        {
        case 0:
            LogMessage("    Quoted-Printable Decoding Depth: %s\n", "Unlimited");
            break;
        default:
            LogMessage("    Quoted-Printable Decoding Depth: %d\n", config->decode_conf.qp_depth);
            break;
        }
    }
    else
        LogMessage("    Quoted-Printable Decoding: %s\n", "Disabled");

    if (config->decode_conf.uu_depth > -1)
    {
        LogMessage("    Unix-to-Unix Decoding: %s\n","Enabled");
        switch (config->decode_conf.uu_depth)
        {
        case 0:
            LogMessage("    Unix-to-Unix Decoding Depth: %s\n", "Unlimited");
            break;
        default:
            LogMessage("    Unix-to-Unix Decoding Depth: %d\n", config->decode_conf.uu_depth);
            break;
        }
    }
    else
        LogMessage("    Unix-to-Unix Decoding: %s\n", "Disabled");

    if (config->decode_conf.bitenc_depth > -1)
    {
        LogMessage("    Non-Encoded MIME attachment Extraction: %s\n","Enabled");
        switch (config->decode_conf.bitenc_depth)
        {
        case 0:
            LogMessage("    Non-Encoded MIME attachment Extraction Depth: %s\n", "Unlimited");
            break;
        default:
            LogMessage("    Non-Encoded MIME attachment Extraction Depth: %d\n",
                config->decode_conf.bitenc_depth);
            break;
        }
    }
    else
        LogMessage("    Non-Encoded MIME attachment Extraction/text: %s\n", "Disabled");

    LogMessage("    Log Attachment filename: %s\n",
        config->log_config.log_filename ? "Enabled" : "Not Enabled");

    LogMessage("    Log MAIL FROM Address: %s\n",
        config->log_config.log_mailfrom ? "Enabled" : "Not Enabled");

    LogMessage("    Log RCPT TO Addresses: %s\n",
        config->log_config.log_rcptto ? "Enabled" : "Not Enabled");

    LogMessage("    Log Email Headers: %s\n",
        config->log_config.log_email_hdrs ? "Enabled" : "Not Enabled");
    if (config->log_config.log_email_hdrs)
    {
        LogMessage("    Email Hdrs Log Depth: %u\n",
            config->log_config.email_hdrs_log_depth);
    }
}

/*
 * * Reset SMTP session state
 * *
 * * @param  none
 * *
 * * @return none
 * */
static void SMTP_ResetState(void* ssn)
{
    SMTPData* smtp_ssn = get_session_data((Flow*)ssn);
    smtp_ssn->state = STATE_COMMAND;
    smtp_ssn->state_flags = 0;
}

static inline int InspectPacket(Packet* p)
{
    return p->has_paf_payload();
}

/*
 * Do first-packet setup
 *
 * @param   p   standard Packet structure
 *
 * @return  none
 */
static int SMTP_Setup(Packet* p, SMTPData* ssn)
{
    int pkt_dir;

    /* Get the direction of the packet. */
    if ( p->packet_flags & PKT_FROM_SERVER )
        pkt_dir = SMTP_PKT_FROM_SERVER;
    else
        pkt_dir = SMTP_PKT_FROM_CLIENT;

    if (!(ssn->session_flags & SMTP_FLAG_CHECK_SSL))
        ssn->session_flags |= SMTP_FLAG_CHECK_SSL;
    /* Check to see if there is a reassembly gap.  If so, we won't know
     *      *      * what state we're in when we get the _next_ reassembled packet */

    /* Check to see if there is a reassembly gap.  If so, we won't know
     * what state we're in when we get the _next_ reassembled packet */
    if ((pkt_dir != SMTP_PKT_FROM_SERVER) &&
        (p->packet_flags & PKT_REBUILT_STREAM))
    {
        int missing_in_rebuilt =
            stream.missing_in_reassembled(p->flow, SSN_DIR_FROM_CLIENT);

        if (ssn->session_flags & SMTP_FLAG_NEXT_STATE_UNKNOWN)
        {
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Found gap in previous reassembly buffer - "
                "set state to unknown\n"); );
            ssn->state = STATE_UNKNOWN;
            ssn->session_flags &= ~SMTP_FLAG_NEXT_STATE_UNKNOWN;
        }

        if (missing_in_rebuilt == SSN_MISSING_BEFORE)
        {
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Found missing packets before "
                "in reassembly buffer - set state to unknown\n"); );
            ssn->state = STATE_UNKNOWN;
        }
    }

    return pkt_dir;
}

/*
 * Callback function for string search
 *
 * @param   id      id in array of search strings from smtp_config.cmds
 * @param   index   index in array of search strings from smtp_config.cmds
 * @param   data    buffer passed in to search function
 *
 * @return response
 * @retval 1        commands caller to stop searching
 */
static int SMTP_SearchStrFound(void* id, void*, int index, void*, void*)
{
    int search_id = (int)(uintptr_t)id;

    smtp_search_info.id = search_id;
    smtp_search_info.index = index;
    smtp_search_info.length = smtp_current_search[search_id].name_len;

    /* Returning non-zero stops search, which is okay since we only look for one at a time */
    return 1;
}

static bool SMTP_IsAuthCtxIgnored(const uint8_t* start, int length)
{
    const SMTPAuth* tmp;
    for (tmp = &smtp_auth_no_ctx[0]; tmp->name != NULL; tmp++)
    {
        if ((tmp->name_len == length) && (!memcmp(start, tmp->name, length)))
            return true;
    }

    return false;
}

static bool SMTP_IsAuthChanged(SMTPData* smtp_ssn, const uint8_t* start_ptr, const
    uint8_t* end_ptr)
{
    int length;
    bool auth_changed = false;
    uint8_t* start = (uint8_t*)start_ptr;
    uint8_t* end = (uint8_t*)end_ptr;

    while ((start < end) && isspace(*start))
        start++;
    while ((start < end) && isspace(*(end-1)))
        end--;

    if (start >= end)
        return auth_changed;

    length = end - start;

    if (length > MAX_AUTH_NAME_LEN)
        return auth_changed;

    if (SMTP_IsAuthCtxIgnored(start, length))
        return auth_changed;

    /* if authentication mechanism is set, compare it with current one*/
    if (smtp_ssn->auth_name)
    {
        if (smtp_ssn->auth_name->length != length)
            auth_changed = true;
        else if (memcmp(start, smtp_ssn->auth_name->name, length))
            auth_changed = true;
    }
    else
        smtp_ssn->auth_name = (SMTPAuthName*)calloc(1, sizeof(*(smtp_ssn->auth_name)));

    /* save the current authentication mechanism*/
    if (!smtp_ssn->auth_name)
        return auth_changed;

    if (auth_changed || (!smtp_ssn->auth_name->length))
    {
        memcpy(smtp_ssn->auth_name->name, start, length);
        smtp_ssn->auth_name->length = length;
    }

    return auth_changed;
}

/*
 * Handle COMMAND state
 *
 * @param   p       standard Packet structure
 * @param   ptr     pointer into p->data buffer to start looking at data
 * @param   end     points to end of p->data buffer
 *
 * @return          pointer into p->data where we stopped looking at data
 *                  will be end of line or end of packet
 */
static const uint8_t* SMTP_HandleCommand(SMTP_PROTO_CONF* config, Packet* p, SMTPData* smtp_ssn,
    const uint8_t* ptr, const uint8_t* end)
{
    const uint8_t* eol;   /* end of line */
    const uint8_t* eolm;  /* end of line marker */
    int cmd_line_len;
    int ret;
    int cmd_found;
    char alert_long_command_line = 0;

    /* get end of line and end of line marker */
    SMTP_GetEOL(ptr, end, &eol, &eolm);

    /* calculate length of command line */
    cmd_line_len = eol - ptr;

    /* check for command line exceeding maximum
     * do this before checking for a command since this could overflow
     * some server's buffers without the presence of a known command */
    if ((config->max_command_line_len != 0) &&
        (cmd_line_len > config->max_command_line_len))
    {
        alert_long_command_line = 1;
    }

    /* TODO If the end of line marker coincides with the end of data we can't be
     * sure that we got a command and not a substring which we could tell through
     * inspection of the next packet. Maybe a command pending state where the first
     * char in the next packet is checked for a space and end of line marker */

    /* do not confine since there could be space chars before command */
    smtp_current_search = &config->cmd_search[0];
    cmd_found = config->cmd_search_mpse->find(
        (const char*)ptr, eolm - ptr, SMTP_SearchStrFound);
    /* see if we actually found a command and not a substring */
    if (cmd_found > 0)
    {
        const uint8_t* tmp = ptr;
        const uint8_t* cmd_start = ptr + smtp_search_info.index;
        const uint8_t* cmd_end = cmd_start + smtp_search_info.length;

        /* move past spaces up until start of command */
        while ((tmp < cmd_start) && isspace((int)*tmp))
            tmp++;

        /* if not all spaces before command, we found a
         * substring */
        if (tmp != cmd_start)
            cmd_found = 0;

        /* if we're before the end of line marker and the next
         * character is not whitespace, we found a substring */
        if ((cmd_end < eolm) && !isspace((int)*cmd_end))
            cmd_found = 0;

        /* there is a chance that end of command coincides with the end of data
         * in which case, it could be a substring, but for now, we will treat it as found */
    }

    /* if command not found, alert and move on */
    if (!cmd_found)
    {
        /* If we missed one or more packets we might not actually be in the command
         * state.  Check to see if we're encrypted */
        if (smtp_ssn->state == STATE_UNKNOWN)
        {
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Command not found, but state is "
                "unknown - checking for SSL\n"); );

            /* check for encrypted */

            if ((smtp_ssn->session_flags & SMTP_FLAG_CHECK_SSL) &&
                (IsSSL(ptr, end - ptr, p->packet_flags)))
            {
                DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Packet is SSL encrypted\n"); );

                smtp_ssn->state = STATE_TLS_DATA;

                /* Ignore data */
                if (config->ignore_tls_data)
                {
                    DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Ignoring encrypted data\n"); );
                    set_alt_data(NULL, 0);
                }

                return end;
            }
            else
            {
                DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Not SSL - try data state\n"); );
                /* don't check for ssl again in this packet */
                if (smtp_ssn->session_flags & SMTP_FLAG_CHECK_SSL)
                    smtp_ssn->session_flags &= ~SMTP_FLAG_CHECK_SSL;

                smtp_ssn->state = STATE_DATA;
                smtp_ssn->mime_ssn.data_state = STATE_DATA_UNKNOWN;

                return ptr;
            }
        }
        else
        {
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "No known command found\n"); );

            if (smtp_ssn->state != STATE_AUTH)
            {
                SnortEventqAdd(GID_SMTP,SMTP_UNKNOWN_CMD);

                if (alert_long_command_line)
                    SnortEventqAdd(GID_SMTP, SMTP_COMMAND_OVERFLOW);
            }

            /* if normalizing, copy line to alt buffer */
            if (smtp_normalizing)
            {
                ret = SMTP_CopyToAltBuffer(p, ptr, eol - ptr);
                if (ret == -1)
                    return NULL;
            }

            return eol;
        }
    }

    /* At this point we have definitely found a legitimate command */

    /* check if max command line length for a specific command is exceeded */
    if (config->cmd_config[smtp_search_info.id].max_line_len != 0)
    {
        if (cmd_line_len > config->cmd_config[smtp_search_info.id].max_line_len)
        {
            SnortEventqAdd(GID_SMTP, SMTP_SPECIFIC_CMD_OVERFLOW);
        }
    }
    else if (alert_long_command_line)
    {
        SnortEventqAdd(GID_SMTP, SMTP_COMMAND_OVERFLOW);
    }

    if (config->cmd_config[smtp_search_info.id].alert)
    {
        /* Are we alerting on this command? */
        SnortEventqAdd(GID_SMTP, SMTP_ILLEGAL_CMD);
    }

    switch (smtp_search_info.id)
    {
    /* unless we do our own parsing of MAIL and RCTP commands we have to assume they
     * are ok unless we got a server error in which case we flush and if this is a
     * reassembled packet, the last command in this packet will be the command that
     * caused the error */
    case CMD_MAIL:
        smtp_ssn->state_flags |= SMTP_FLAG_GOT_MAIL_CMD;
        if ( config->log_config.log_mailfrom )
        {
            if (!SMTP_CopyEmailID(ptr, eolm - ptr, CMD_MAIL, smtp_ssn->mime_ssn.log_state))
                smtp_ssn->mime_ssn.log_flags |= MIME_FLAG_MAIL_FROM_PRESENT;
        }

        break;

    case CMD_RCPT:
        if ((smtp_ssn->state_flags & SMTP_FLAG_GOT_MAIL_CMD) ||
            smtp_ssn->state == STATE_UNKNOWN)
        {
            smtp_ssn->state_flags |= SMTP_FLAG_GOT_RCPT_CMD;
        }

        if ( config->log_config.log_rcptto)
        {
            if (!SMTP_CopyEmailID(ptr, eolm - ptr, CMD_RCPT, smtp_ssn->mime_ssn.log_state))
                smtp_ssn->mime_ssn.log_flags |= MIME_FLAG_RCPT_TO_PRESENT;
        }

        break;

    case CMD_RSET:
    case CMD_HELO:
    case CMD_EHLO:
    case CMD_QUIT:
        smtp_ssn->state_flags &= ~(SMTP_FLAG_GOT_MAIL_CMD | SMTP_FLAG_GOT_RCPT_CMD);

        break;

    case CMD_STARTTLS:
        /* if reassembled we flush after seeing a 220 so this should be the last
         * command in reassembled packet and if not reassembled it should be the
         * last line in the packet as you can't pipeline the tls hello */
        if (eol == end)
            smtp_ssn->state = STATE_TLS_CLIENT_PEND;

        break;

    case CMD_X_LINK2STATE:
        if (config->xlink2state)
            ParseXLink2State(config, p, smtp_ssn, ptr + smtp_search_info.index);

        break;

    case CMD_AUTH:
        smtp_ssn->state = STATE_AUTH;
        if (SMTP_IsAuthChanged(smtp_ssn, ptr + smtp_search_info.index + smtp_search_info.length,
            eolm)
            && (smtp_ssn->state_flags & SMTP_FLAG_ABORT))
        {
            SnortEventqAdd(GID_SMTP, SMTP_AUTH_ABORT_AUTH);
        }
        smtp_ssn->state_flags &= ~(SMTP_FLAG_ABORT);
        break;

    case CMD_ABORT:
        smtp_ssn->state_flags |= SMTP_FLAG_ABORT;
        break;

    default:
        switch (smtp_known_cmds[smtp_search_info.id].type)
        {
        case SMTP_CMD_TYPE_DATA:
            if ((smtp_ssn->state_flags & SMTP_FLAG_GOT_RCPT_CMD) ||
                smtp_ssn->state == STATE_UNKNOWN)
            {
                DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Set to data state.\n"); );

                smtp_ssn->state = STATE_DATA;
                smtp_ssn->state_flags &= ~(SMTP_FLAG_GOT_MAIL_CMD | SMTP_FLAG_GOT_RCPT_CMD);
            }
            else
            {
                DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Didn't get MAIL -> RCPT command sequence - "
                    "stay in command state.\n"); );
            }

            break;

        case SMTP_CMD_TYPE_BDATA:
            if ((smtp_ssn->state_flags & (SMTP_FLAG_GOT_RCPT_CMD | SMTP_FLAG_BDAT))
                || (smtp_ssn->state == STATE_UNKNOWN))
            {
                const uint8_t* begin_chunk;
                const uint8_t* end_chunk;
                const uint8_t* tmp;
                int num_digits;
                int ten_power;
                uint32_t dat_chunk = 0;

                begin_chunk = ptr + smtp_search_info.index + smtp_search_info.length;
                while ((begin_chunk < eolm) && isspace((int)*begin_chunk))
                    begin_chunk++;

                /* bad BDAT command - needs chunk argument */
                if (begin_chunk == eolm)
                    break;

                end_chunk = begin_chunk;
                while ((end_chunk < eolm) && isdigit((int)*end_chunk))
                    end_chunk++;

                /* didn't get all digits */
                if ((end_chunk < eolm) && !isspace((int)*end_chunk))
                    break;

                /* get chunk size */
                num_digits = end_chunk - begin_chunk;

                /* more than 9 digits could potentially overflow a 32 bit integer
                 * most servers won't accept this much in a chunk */
                if (num_digits > 9)
                    break;

                tmp = end_chunk;
                for (ten_power = 1, tmp--; tmp >= begin_chunk; ten_power *= 10, tmp--)
                    dat_chunk += (*tmp - '0') * ten_power;

                if (smtp_search_info.id == CMD_BDAT)
                {
                    /* got a valid chunk size - check to see if this is the last chunk */
                    const uint8_t* last = end_chunk;
                    bool bdat_last = false;

                    while ((last < eolm) && isspace((int)*last))
                        last++;

                    if (((eolm - last) >= 4)
                        && (strncasecmp("LAST", (const char*)last, 4) == 0))
                    {
                        bdat_last = true;
                    }

                    if (bdat_last || (dat_chunk == 0))
                        smtp_ssn->state_flags &= ~(SMTP_FLAG_BDAT);
                    else
                        smtp_ssn->state_flags |= SMTP_FLAG_BDAT;

                    smtp_ssn->state = STATE_BDATA;
                    smtp_ssn->state_flags &= ~(SMTP_FLAG_GOT_MAIL_CMD | SMTP_FLAG_GOT_RCPT_CMD);
                }
                else if (smtp_search_info.id == CMD_XEXCH50)
                {
                    smtp_ssn->state = STATE_XEXCH50;
                }
                else
                {
                    smtp_ssn->state = STATE_BDATA;
                    smtp_ssn->state_flags &= ~(SMTP_FLAG_GOT_MAIL_CMD | SMTP_FLAG_GOT_RCPT_CMD);
                }

                smtp_ssn->dat_chunk = dat_chunk;
            }

            break;

        case SMTP_CMD_TYPE_AUTH:
            smtp_ssn->state = STATE_AUTH;
            break;

        default:
            break;
        }
        break;
    }

    /* Since we found a command, if state is still unknown,
     * set to command state */
    if (smtp_ssn->state == STATE_UNKNOWN)
        smtp_ssn->state = STATE_COMMAND;

    /* normalize command line */
    if (config->normalize == NORMALIZE_ALL ||
        config->cmd_config[smtp_search_info.id].normalize)
    {
        ret = SMTP_NormalizeCmd(p, ptr, eolm, eol);
        if (ret == -1)
            return NULL;
    }
    else if (smtp_normalizing) /* Already normalizing */
    {
        ret = SMTP_CopyToAltBuffer(p, ptr, eol - ptr);
        if (ret == -1)
            return NULL;
    }

    return eol;
}

static int SMTP_NormalizeData(void* conf, void* pkt, const uint8_t* ptr, const uint8_t* data_end)
{
    Packet* p = (Packet*)pkt;
    SMTP_PROTO_CONF* config = (SMTP_PROTO_CONF*)conf;

    /* if we're ignoring data and not already normalizing, copy everything
     * up to here into alt buffer so detection engine doesn't have
     * to look at the data; otherwise, if we're normalizing and not
     * ignoring data, copy all of the data into the alt buffer */
    if (config->decode_conf.ignore_data && !smtp_normalizing)
    {
        return SMTP_CopyToAltBuffer(p, p->data, ptr - p->data);
    }
    else if (!config->decode_conf.ignore_data && smtp_normalizing)
    {
        return SMTP_CopyToAltBuffer(p, ptr, data_end - ptr);
    }

    return 0;
}

static int SMTP_HandleHeaderLine(void* conf, void* pkt, const uint8_t* ptr, const uint8_t* eol,
    int max_header_len, void* ssn)
{
    int ret;
    int header_line_len;
    Packet* p = (Packet*)pkt;
    SMTP_PROTO_CONF* config = (SMTP_PROTO_CONF*)conf;
    MimeState* mime_ssn = (MimeState*)ssn;
    /* get length of header line */
    header_line_len = eol - ptr;

    if (max_header_len)
        SnortEventqAdd(GID_SMTP, SMTP_HEADER_NAME_OVERFLOW);

    if ((config->max_header_line_len != 0) &&
        (header_line_len > config->max_header_line_len))
    {
        if (mime_ssn->data_state != STATE_DATA_UNKNOWN)
        {
            SnortEventqAdd(GID_SMTP, SMTP_DATA_HDR_OVERFLOW);
        }
        else
        {
            /* assume we guessed wrong and are in the body */
            return 1;
        }
    }

    /* XXX Does VRT want data headers normalized?
     * currently the code does not normalize headers */
    if (smtp_normalizing)
    {
        ret = SMTP_CopyToAltBuffer(p, ptr, eol - ptr);
        if (ret == -1)
            return (-1);
    }

    if (config->log_config.log_email_hdrs)
    {
        if (mime_ssn->data_state == STATE_DATA_HEADER)
        {
            ret = SMTP_CopyEmailHdrs(ptr, eol - ptr, mime_ssn->log_state);
            if (ret == 0)
                mime_ssn->log_flags |= MIME_FLAG_EMAIL_HDRS_PRESENT;
        }
    }

    return 0;
}

/*
 * Process client packet
 *
 * @param   packet  standard Packet structure
 *
 * @return  none
 */
static void SMTP_ProcessClientPacket(SMTP_PROTO_CONF* config, Packet* p, SMTPData* smtp_ssn)
{
    const uint8_t* ptr = p->data;
    const uint8_t* end = p->data + p->dsize;

    if (smtp_ssn->state == STATE_CONNECT)
    {
        smtp_ssn->state = STATE_COMMAND;
    }

    while ((ptr != NULL) && (ptr < end))
    {
        switch (smtp_ssn->state)
        {
        case STATE_COMMAND:
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "COMMAND STATE ~~~~~~~~~~~~~~~~~~~~~~~~~~\n"); );
            ptr = SMTP_HandleCommand(config, p, smtp_ssn, ptr, end);
            break;
        case STATE_DATA:
        case STATE_BDATA:
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "DATA STATE ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"); );
            ptr = file_api->process_mime_data(p, ptr, end, &(smtp_ssn->mime_ssn), 1, true);
            //ptr = SMTP_HandleData(p, ptr, end, &(smtp_ssn->mime_ssn));
            break;
        case STATE_XEXCH50:
            if (smtp_normalizing)
                SMTP_CopyToAltBuffer(p, ptr, end - ptr);
            if (smtp_is_data_end (p->flow))
                smtp_ssn->state = STATE_COMMAND;
            return;
        case STATE_AUTH:
            ptr = SMTP_HandleCommand(config, p, smtp_ssn, ptr, end);
            break;
        case STATE_UNKNOWN:
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "UNKNOWN STATE ~~~~~~~~~~~~~~~~~~~~~~~~~~\n"); );
            /* If state is unknown try command state to see if we can
             * regain our bearings */
            ptr = SMTP_HandleCommand(config, p, smtp_ssn, ptr, end);
            break;
        default:
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Bad SMTP state\n"); );
            return;
        }
    }

#ifdef DEBUG_MSGS
    if (smtp_normalizing)
    {
        DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Normalized data\n%s\n", SMTP_PrintBuffer(p)); );
    }
#endif
}

/*
 * Process server packet
 *
 * @param   packet  standard Packet structure
 *
 * @return  None
 */
static void SMTP_ProcessServerPacket(SMTP_PROTO_CONF* config, Packet* p, SMTPData* smtp_ssn,
    int* next_state)
{
    int resp_found;
    const uint8_t* ptr;
    const uint8_t* end;
    const uint8_t* eolm;
    const uint8_t* eol;
    int resp_line_len;
#ifdef DEBUG_MSGS
    const uint8_t* dash;
#endif

    *next_state = 0;

    ptr = p->data;
    end = p->data + p->dsize;

    if (smtp_ssn->state == STATE_TLS_SERVER_PEND)
    {
        if (IsTlsServerHello(ptr, end))
        {
            smtp_ssn->state = STATE_TLS_DATA;
        }
        else if (!(stream.get_session_flags(p->flow) & SSNFLAG_MIDSTREAM)
            && !stream.missed_packets(p->flow, SSN_DIR_BOTH))
        {
            /* Check to see if the raw packet is in order */
            if (p->packet_flags & PKT_STREAM_ORDER_OK)
            {
                /* revert back to command state - assume server didn't accept STARTTLS */
                smtp_ssn->state = STATE_COMMAND;
            }
            else
                return;
        }
    }

    if (smtp_ssn->state == STATE_TLS_DATA)
    {
        smtp_ssn->state = STATE_COMMAND;
    }

    while (ptr < end)
    {
        SMTP_GetEOL(ptr, end, &eol, &eolm);

        resp_line_len = eol - ptr;

        /* Check for response code */
        smtp_current_search = &smtp_resp_search[0];
        resp_found = smtp_resp_search_mpse->find(
            (const char*)ptr, resp_line_len, SMTP_SearchStrFound);

        if (resp_found > 0)
        {
            switch (smtp_search_info.id)
            {
            case RESP_220:
                /* This is either an initial server response or a STARTTLS response */
                if (smtp_ssn->state == STATE_CONNECT)
                    smtp_ssn->state = STATE_COMMAND;
                break;

            case RESP_250:
            case RESP_221:
            case RESP_334:
            case RESP_354:
                break;

            case RESP_235:
                // Auth done
                *next_state = STATE_COMMAND;
                break;

            default:
                if (smtp_ssn->state != STATE_COMMAND)
                {
                    *next_state = STATE_COMMAND;
                }
                break;
            }

#ifdef DEBUG_MSGS
            dash = ptr + smtp_search_info.index + smtp_search_info.length;

            /* only add response if not a dash after response code */
            if ((dash == eolm) || ((dash < eolm) && (*dash != '-')))
            {
                DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Server sent %s response\n",
                    smtp_resps[smtp_search_info.id].name); );
            }
#endif
        }
        else
        {
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP,
                "Server response not found - see if it's SSL data\n"); );

            if ((smtp_ssn->session_flags & SMTP_FLAG_CHECK_SSL) &&
                (IsSSL(ptr, end - ptr, p->packet_flags)))
            {
                DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Server response is an SSL packet\n"); );

                smtp_ssn->state = STATE_TLS_DATA;

                /* Ignore data */
                if (config->ignore_tls_data)
                {
                    DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Ignoring Server TLS encrypted data\n"); );
                    set_alt_data(NULL, 0);
                }

                return;
            }
            else if (smtp_ssn->session_flags & SMTP_FLAG_CHECK_SSL)
            {
                smtp_ssn->session_flags &= ~SMTP_FLAG_CHECK_SSL;
            }
        }

        if ((config->max_response_line_len != 0) &&
            (resp_line_len > config->max_response_line_len))
        {
            SnortEventqAdd(GID_SMTP, SMTP_RESPONSE_OVERFLOW);
        }

        ptr = eol;
    }
}

/*
 * Entry point to snort preprocessor for each packet
 *
 * @param   packet  standard Packet structure
 *
 * @return  none
 */
static void snort_smtp(SMTP_PROTO_CONF* config, Packet* p)
{
    int pkt_dir;

    /* Attempt to get a previously allocated SMTP block. */

    SMTPData* smtp_ssn = get_session_data(p->flow);

    if (smtp_ssn == NULL)
    {
        /* Check the stream session. If it does not currently
         *          * have our SMTP data-block attached, create one.
         *                   */
        smtp_ssn = SetNewSMTPData(config, p);

        if ( !smtp_ssn )
        {
            /* Could not get/create the session data for this packet. */
            return;
        }
    }

    pkt_dir = SMTP_Setup(p, smtp_ssn);
    SMTP_ResetAltBuffer();

    /* reset normalization stuff */
    smtp_normalizing = 0;
    SetDetectLimit(p, 0);

    if (pkt_dir == SMTP_PKT_FROM_SERVER)
    {
        int next_state = 0;

        DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "SMTP server packet\n"); );

        /* Process as a server packet */
        SMTP_ProcessServerPacket(config, p, smtp_ssn, &next_state);

        if (next_state)
            smtp_ssn->state = next_state;
    }
    else
    {
#ifdef DEBUG_MSGS
        if (pkt_dir == SMTP_PKT_FROM_CLIENT)
        {
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "SMTP client packet\n"); );
        }
        else
        {
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "SMTP packet NOT from client or server! "
                "Processing as a client packet\n"); );
        }
#endif

        /* This packet should be a tls client hello */
        if (smtp_ssn->state == STATE_TLS_CLIENT_PEND)
        {
            if (IsTlsClientHello(p->data, p->data + p->dsize))
            {
                DEBUG_WRAP(DebugMessage(DEBUG_SMTP,
                    "TLS DATA STATE ~~~~~~~~~~~~~~~~~~~~~~~~~\n"); );

                smtp_ssn->state = STATE_TLS_SERVER_PEND;
            }
            else if (p->packet_flags & PKT_STREAM_ORDER_OK)
            {
                /* reset state - server may have rejected STARTTLS command */
                smtp_ssn->state = STATE_COMMAND;
            }
        }

        if ((smtp_ssn->state == STATE_TLS_DATA)
            || (smtp_ssn->state == STATE_TLS_SERVER_PEND))
        {
            /* if we're ignoring tls data, set a zero length alt buffer */
            if (config->ignore_tls_data)
            {
                set_alt_data(NULL, 0);
                stream.stop_inspection(p->flow, p, SSN_DIR_BOTH, -1, 0);
                return;
            }
        }
        else
        {
            if ( !InspectPacket(p))
            {
                /* Packet will be rebuilt, so wait for it */
                DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Client packet will be reassembled\n"));
                return;
            }
            else if (!(p->packet_flags & PKT_REBUILT_STREAM))
            {
                /* If this isn't a reassembled packet and didn't get
                 * inserted into reassembly buffer, there could be a
                 * problem.  If we miss syn or syn-ack that had window
                 * scaling this packet might not have gotten inserted
                 * into reassembly buffer because it fell outside of
                 * window, because we aren't scaling it */
                smtp_ssn->session_flags |= SMTP_FLAG_GOT_NON_REBUILT;
                smtp_ssn->state = STATE_UNKNOWN;
            }
            else if ((smtp_ssn->session_flags & SMTP_FLAG_GOT_NON_REBUILT))
            {
                /* This is a rebuilt packet.  If we got previous packets
                 * that were not rebuilt, state is going to be messed up
                 * so set state to unknown. It's likely this was the
                 * beginning of the conversation so reset state */
                DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Got non-rebuilt packets before "
                    "this rebuilt packet\n"); );

                smtp_ssn->state = STATE_UNKNOWN;
                smtp_ssn->session_flags &= ~SMTP_FLAG_GOT_NON_REBUILT;
            }

#ifdef DEBUG_MSGS
            /* Interesting to see how often packets are rebuilt */
            DEBUG_WRAP(DebugMessage(DEBUG_SMTP, "Payload: %s\n%s\n",
                (p->packet_flags & PKT_REBUILT_STREAM) ?
                "reassembled" : "not reassembled",
                SMTP_PrintBuffer(p)); );
#endif

            SMTP_ProcessClientPacket(config, p, smtp_ssn);
        }
    }

    SMTP_LogFuncs(config, p, &(smtp_ssn->mime_ssn));
}

/* Callback to return the MIME attachment filenames accumulated */
int SMTP_GetFilename(Flow* flow, uint8_t** buf, uint32_t* len, uint32_t* type)
{
    SMTPData* ssn = get_session_data(flow);

    if (ssn == NULL)
        return 0;

    *buf = ssn->mime_ssn.log_state->file_log.filenames;
    *len = ssn->mime_ssn.log_state->file_log.file_logged;
    *type = EVENT_INFO_SMTP_FILENAME;
    return 1;
}

/* Callback to return the email addresses accumulated from the MAIL FROM command */
int SMTP_GetMailFrom(Flow* flow, uint8_t** buf, uint32_t* len, uint32_t* type)
{
    SMTPData* ssn = get_session_data(flow);

    if (ssn == NULL)
        return 0;

    *buf = ssn->mime_ssn.log_state->senders;
    *len = ssn->mime_ssn.log_state->snds_logged;
    *type = EVENT_INFO_SMTP_MAILFROM;
    return 1;
}

/* Callback to return the email addresses accumulated from the RCP TO command */
int SMTP_GetRcptTo(Flow* flow, uint8_t** buf, uint32_t* len, uint32_t* type)
{
    SMTPData* ssn = get_session_data(flow);

    if (ssn == NULL)
        return 0;

    *buf = ssn->mime_ssn.log_state->recipients;
    *len = ssn->mime_ssn.log_state->rcpts_logged;
    *type = EVENT_INFO_SMTP_RCPTTO;
    return 1;
}

/* Calback to return the email headers */
int SMTP_GetEmailHdrs(Flow* flow, uint8_t** buf, uint32_t* len, uint32_t* type)
{
    SMTPData* ssn = get_session_data(flow);

    if (ssn == NULL)
        return 0;

    *buf = ssn->mime_ssn.log_state->emailHdrs;
    *len = ssn->mime_ssn.log_state->hdrs_logged;
    *type = EVENT_INFO_SMTP_EMAIL_HDRS;
    return 1;
}

static void SMTP_RegXtraDataFuncs(SMTP_PROTO_CONF* config)
{
    config->xtra_filename_id = stream.reg_xtra_data_cb(SMTP_GetFilename);
    config->xtra_mfrom_id = stream.reg_xtra_data_cb(SMTP_GetMailFrom);
    config->xtra_rcptto_id = stream.reg_xtra_data_cb(SMTP_GetRcptTo);
    config->xtra_ehdrs_id = stream.reg_xtra_data_cb(SMTP_GetEmailHdrs);
}

//-------------------------------------------------------------------------
// class stuff
//-------------------------------------------------------------------------

class Smtp : public Inspector
{
public:
    Smtp(SMTP_PROTO_CONF*);
    ~Smtp();

    bool configure(SnortConfig*) override;
    void show(SnortConfig*) override;
    void eval(Packet*) override;

    StreamSplitter* get_splitter(bool c2s) override
    { return new SmtpSplitter(c2s); }

private:
    SMTP_PROTO_CONF* config;
};

Smtp::Smtp(SMTP_PROTO_CONF* pc)
{
    config = pc;
}

Smtp::~Smtp()
{
    if ( config )
        delete config;
}

bool Smtp::configure(SnortConfig*)
{
    config->decode_conf.file_depth = file_api->get_max_file_depth();

    if (config->decode_conf.file_depth > 0)
        config->log_config.log_filename = 1;

    if (file_api->is_decoding_enabled(&config->decode_conf) )
    {
        updateMaxDepth(config->decode_conf.file_depth,
            &config->decode_conf.max_depth);
    }
    file_api->check_decode_config(&config->decode_conf);

    return true;
}

void Smtp::show(SnortConfig*)
{
    SMTP_PrintConfig(config);
}

void Smtp::eval(Packet* p)
{
    PROFILE_VARS;
    // precondition - what we registered for
    assert(p->has_tcp_data());

    ++smtpstats.total_packets;

    MODULE_PROFILE_START(smtpPerfStats);

    snort_smtp(config, p);

    MODULE_PROFILE_END(smtpPerfStats);
}

//-------------------------------------------------------------------------
// api stuff
//-------------------------------------------------------------------------

static Module* mod_ctor()
{ return new SmtpModule; }

static void mod_dtor(Module* m)
{ delete m; }

static void smtp_init()
{
    SmtpFlowData::init();
    SMTP_ResponseSearchInit();
}

static void smtp_term()
{
    SMTP_SearchFree();
}

static Inspector* smtp_ctor(Module* m)
{
    SmtpModule* mod = (SmtpModule*)m;
    SMTP_PROTO_CONF* conf = mod->get_data();
    unsigned i = 0;
    SMTP_RegXtraDataFuncs(conf);
    SMTP_InitCmds(conf);

    while ( const SmtpCmd* cmd = mod->get_cmd(i++) )
        ProcessSmtpCmdsList(conf, cmd);

    SMTP_CommandSearchInit(conf);

    return new Smtp(conf);
}

static void smtp_dtor(Inspector* p)
{
    delete p;
}

const InspectApi smtp_api =
{
    {
        PT_INSPECTOR,
        sizeof(InspectApi),
        INSAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        SMTP_NAME,
        SMTP_HELP,
        mod_ctor,
        mod_dtor
    },
    IT_SERVICE,
    (uint16_t)PktType::TCP,
    nullptr,                // buffers
    "smtp",
    smtp_init,
    smtp_term,
    nullptr,                // tinit
    nullptr,                // tterm
    smtp_ctor,
    smtp_dtor,
    nullptr,                // ssn
    nullptr                 // reset
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
{
    &smtp_api.base,
    nullptr
};
#else
const BaseApi* sin_smtp = &smtp_api.base;
#endif

