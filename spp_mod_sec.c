/*
 * spp_mod_sec.c
 *
 * Copyright 2015 Fakhri Zulkifli <d0lph1n98@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 *
 */

/*
 * Snort ModSecurity Preprocessor
 * Author: Fakhri Zulkifli <d0lph1n98@yahoo.com>
 *
 * Parse ModSecurity CRS. Alert malicious traffic in HTTP protocol and tells
 * the iptables to drop the packet.
 */

#include <assert.h>
#include <sys/types.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sf_types.h"
#include "preprocids.h"
#include "sf_snort_packet.h"
#include "sf_dynamic_preproc_lib.h"
#include "sf_dynamic_preprocessor.h"
#include "snort_debug.h"
#include "spp_mod_sec.h"

#include "sfPolicy.h"
#include "sfPolicyUserData.h"

#include "profiler.h"

/*
 * Generator id. Define here the same as the official registry
 * in generators.h
 */
#define GENERATOR_SPP_MODSEC 146

#define SRC_PORT_MATCH  1
#define SRC_PORT_MATCH_STR "spp_mod_sec_preprocessor: src port match"
#define DST_PORT_MATCH  2
#define DST_PORT_MATCH_STR "spp_mod_sec_preprocessor: dest port match"

const int MAJOR_VERSION = 1;
const int MINOR_VERSION = 1;
const int BUILD_VERSION = 1;
const char *PREPROC_NAME = "SF_MODSEC";

#define DYNAMIC_PREPROC_SETUP ModSec_setup

/* ModSec_snort_alert* (*get_alerts)(void); */
/* static void* (*alertparser_thread)(void*) = NULL; */

/* ModSec configuration per policy */
ModSec_config *config = NULL;
tSfPolicyUserContextId modsec_config = NULL;

#ifdef SNORT_RELOAD
tSfPolicyUserContextId modsec_swap_config = NULL;
#endif

/* Already put in preprocids.h, still the same :( */
// #define PP_MODSEC 43

/*
 * Function prototype(s)
 */
static void ModSecInit(char *);
static void ModSecProcess(void *, void *);
static ModSec_config * ModSecParse(char *);
//static void ParseModSecRule(void *, void *);
#ifdef SNORT_RELOAD
static void ModSecReload(char *);
static int ModSecReloadSwapPolicyFree(tSfPolicyUserContextId, tSfPolicyId, void *);
static void * ModSecReloadSwap(void);
static void ModSecReloadSwapFree(void *);
#endif

void
ModSecFatalError(const char *msg, const char *file, const int line)
{
    _dpd.fatalMsg("%s: %s at %s:%d (%s)\n",
                  PREPROC_NAME, msg, file, line,
                  ((errno != 0) ? strerror(errno) : ""));
}

/* Set up the preprocessor module */
void ModSec_setup(void)
{
#ifndef SNORT_RELOAD
    _dpd.registerPreproc("mod_sec", ModSecInit);
#else
    _dpd.registerPreproc("mod_sec", ModSecInit, ModSecReload,
                         ModSecReloadSwap, ModSecReloadSwapFree);
#endif
    _dpd.logMsg("ModSecurity Preprocessor Initialized!\n");

    DEBUG_WRAP(_dpd.debugMsg(DEBUG_PLUGIN, "Preprocessor: ModSec is setup\n"););
}

/* Initializes the ModSec preprocessor module and registers
 * it in the preprocessor list.
 *
 * PARAMETERS:
 *
 * args: 	Pointer to argument string to process for config
 * 			data.
 *
 * RETURNS: 	Nothing.
 */
static void ModSecInit(char *args)
{
    //tSfPolicyId policy_id = _dpd.getParserPolicy();

    _dpd.logMsg("ModSec dynamic preprocessor configuration\n");

    if (modsec_config == NULL)
    {
        modsec_config = sfPolicyConfigCreate();
        if (modsec_config == NULL)
        {
            ModSecFatalError("Could not allocate configuration struct.\n", __FILE__, __LINE__);
        }

        /* if(_dpd.streamAPI == NULL) */
        /* { */
        /*     ModSecFatalError("SetupModSec(): The Stream preprocessor must be enabled.\n"); */
        /* } */
    }

    config = ModSecParse(args);
    //sfPolicyUserPolicySet(modsec_config, policy_id);
    sfPolicyUserDataSetCurrent(modsec_config, config);

    /* If webserv_port is != 0, start the web server */
    if(config->webserv_port != 0)
    {

    }

    /* Register the preprocessor function, Transport layer, ID 10000 */
    _dpd.addPreproc(ModSecProcess, PRIORITY_TRANSPORT, 10000, PROTO_BIT__TCP | PROTO_BIT__UDP);
    DEBUG_WRAP(_dpd.debugMsg(DEBUG_PLUGIN, "Preprocessor: ModSec is initialized\n"););
}

/* Parse the arguments passed to the module saving them to a valid configuration struct
 *
 * PARAMETERS:
 *
 * args: 	Arguments passed to the module
 *
 * RETURN: 	Pointer to ModSec_config keeping the configuration for the module
 */
static ModSec_config * ModSecParse(char *args)
{
    char *arg;
    // char *match;

    unsigned short webserv_port = 0;

    if(!(config = (ModSec_config *)malloc(sizeof(ModSec_config))))
        ModSecFatalError("Could not allocate configuration struct", __FILE__, __LINE__);
    memset(config, 0, sizeof(ModSec_config));

    /* Parsing the webserv_port option */
    /* TODO: SSL_ENABLED port option */
    if((arg = (char *)strcmp(args, "webserv_port")))
    {
        for(arg += strlen("webserv_port"); *arg && (*arg < '0' || *arg > MAX_PORTS); arg++);

        if(!(*arg))
        {
            ModSecFatalError("webserv_port option used but "
                             "no value specified", __FILE__, __LINE__);
        }

        webserv_port = (unsigned short)strtoul(arg, NULL, 10);
        config->webserv_port = webserv_port;
    } else {
        config->webserv_port = DEFAULT_WEBSERV_PORT;
    }

    _dpd.logMsg("Web server port: %d\n", config->webserv_port);

    return config;
}

#if 0
static ModSec_config * ModSecParse(char *args)
{
    char *arg = NULL;
    char *argEnd = NULL;
    unsigned port = 0;

    ModSec_config *config = (ModSec_config *)calloc(1, sizeof(ModSec_config));

    if (config == NULL)
        _dpd.fatalMsg("Could not allocate configuration struct.\n");

    config->ports[PORT_INDEX(80)] |= CONV_PORT(80);

    arg = strtok(args, " ");

    if(!args)
    {
        /* Help Display */
        return;
    }

    argEnd = strdup((char*) args);

    if(!argEnd)
    {
        ModSecFatalError("Could not allocate memory to parse ModSec options.\n");
        return;
    }

    while(arg) {
        if(!strcmp(arg, MODSEC_SERVERPORTS_KEYWORD))
        {
            /* Use the user specified '80' */
            config->ports[ PORT_INDEX( 80 ) ] = 0;

            arg = strtok(NULL, "\t\n\r");
            if (!arg)
            {
                _dpd.fatalMsg("ModSec: Missing port\n");
            }

            /* Remove the braces, they said */
            arg = strtok(NULL, " ");
            if ((!arg) || (arg[0] != '{'))
            {
                ModSecFatalError("Bad value specified for %s.\n",MODSEC_SERVERPORTS_KEYWORD);
            }

            while ((arg) && (arg[0] != '}'))
            {
                if (!isdigit((int)arg[0]))
                {
                    ModSecFatalError("Bad port &s.\n", arg);
                }
                else
                {
                    port = atoi(arg);
                    if(port < 0 || port > MAX_PORTS)
                    {
                        ModSecFatalError("Port value illegitimate: %s\n", arg);
                    }

                    config->ports[PORT_INDEX(port)] |= CONV_PORT(port);
                }

                arg = strtok(NULL, " ");
            }
        }
        /* port = strtol(arg, &argEnd, 10); */
        /* if (port < 0 || port > 65535) */
        /* { */
        /*     _dpd.fatalMsg("ModSec: Invalid port %d\n", port); */
        /* } */
        /* config->portToCheck = (u_int16_t)port; */
        /*  */
        /* _dpd.logMsg("    Port: %d\n", config->portToCheck); */

        else
        {
            /* _dpd.fatalMsg("ModSec: Invalid option %s\n", */
            /*               arg?arg:"(missing port)"); */
            ModSecFatalError("Invalid argument: %s\n", arg);
            return;
        }

        arg = strtok(NULL, " ");
    }
    return config;
}
#endif

/* Main runtime entry point for ModSec preprocessor.
 * Analyzes ModSec packets for anomalies/exploits.
 *
 * PARAMETERS:
 *
 * pkt: 	Pointer to current packet to process.
 * context: 	Pointer to context block, not used.
 *
 * RETURNS: 	Nothing.
 */
void ModSecProcess(void *pkt, void *context)
{
    SFSnortPacket *p = (SFSnortPacket *)pkt;
    ModSec_config *_config;

    sfPolicyUserPolicySet(modsec_config, _dpd.getNapRuntimePolicy());
    _config = (ModSec_config *)sfPolicyUserDataGetCurrent(modsec_config);

    /* sfPolicyUserPolicySet(modsec_config, _dpd.getNapRuntimePolicy()); */
    /* config = (ModSec_config *)sfPolicyUserDataGetCurrent(modsec_config); */
    if (_config == NULL)
        return;

    // preconditions - what we registered for
    assert(IsUDP(p) || IsTCP(p));

    ModSecPktEnqueue(pkt);

    /*
     * removeSubstr function
     *
     * Remove a pattern from the string.
     */
    void removeSubstr(char *string, char *sub) {
        char *match = string;
        int len = strlen(sub);
        while((match = strstr(match, sub))) {
            *match = '\0';
            strcat(string, match+len);
            match++;
        }
    }

    int y;
    FILE *data;
    char line[100]; 	// output parsed string is limited
    int counter = 0;
    char keyword[] = ""; // no function whatsoever
    int index = 0;
    // int result;

    struct rule {
        char keyword1[100];
        char keyword2[100];
    } ruleset[10];

    if((data=fopen("rule", "r")) != NULL) {
        while(fgets(line,sizeof(line),data)) {
            if((strcmp(line,keyword))) {
                char s[10] = "$,";
                char *token = strtok(line, s);

                while(token != NULL) {
                    if(counter == 1) {
                        strcpy(ruleset[index].keyword1, token);
                    }
                    if(counter == 2) {
                        strcpy(ruleset[index].keyword2, token);
                    }
                    counter++;
                    token = strtok(NULL, s);
                }
            }
        }
    }

    /* Skid's code */
    for(y = 0; y < index; ++y) {
        removeSubstr(ruleset[y].keyword1, "ARGS|XML:/* \"");
        removeSubstr(ruleset[y].keyword1, "RGS_NAMES|");
        printf("%s ", ruleset[y].keyword1);
        removeSubstr(ruleset[y].keyword2, "\" \"phase:2");
        printf("%s ", ruleset[y].keyword2);
        printf("\n");
    }

    /* fclose(data); */
    /* return; */

    if(p->src_port == _config->webserv_port)
    {
        _dpd.alertAdd(GENERATOR_SPP_MODSEC, DST_PORT_MATCH, 1, 0, 3, DST_PORT_MATCH_STR, 0);
        if(!(p))
        {

        }
    }

    fclose(data);
    return;
}

#ifdef SNORT_RELOAD
static void ModSecReload(char *args)
{
    //tSfPolicyId policy_id = _dpd.getParserPolicy();

    _dpd.logMsg("ModSec dynamic preprocessor configuration\n");

    if(modsec_swap_config == NULL)
    {
        modsec_swap_config = sfPolicyConfigCreate();
        if(modsec_swap_config == NULL)
            ModSecFatalError("Could not allocate configuration struct", __FILE__, __LINE__);
    }

    config = ModSecParse(args);
    //sfPolicyUserPolicySet(modsec_swap_config, policy_id);
    sfPolicyUserDataSetCurrent(modsec_swap_config, config);

    /* Register the preprocessor function, Transport layer, ID 10000 */
    _dpd.addPreproc(ModSecProcess, PRIORITY_TRANSPORT, 10000, PROTO_BIT__TCP | PROTO_BIT__UDP);

    DEBUG_WRAP(_dpd.debugMsg(DEBUG_PLUGIN, "Preprocessor: ModSec is initialized\n"));
}

static int ModSecReloadSwapPolicyFree(tSfPolicyUserContextId config, tSfPolicyId policyId, void *data)
{
    ModSec_config *policy_config = (ModSec_config *)data;

    sfPolicyUserDataClear(config, policyId);
    free(policy_config);
    return 0;
}

static void * ModSecReloadSwap(void)
{
    tSfPolicyUserContextId old_config = modsec_config;

    if (modsec_swap_config == NULL)
        return NULL;

    modsec_config = modsec_swap_config;
    modsec_swap_config = NULL;

    return (void *)old_config;
}

static void ModSecReloadSwapFree(void *data)
{
    tSfPolicyUserContextId config = (tSfPolicyUserContextId)data;

    if (data == NULL)
        return;

    sfPolicyUserDataFreeIterate(config, ModSecReloadSwapPolicyFree);
    sfPolicyConfigDelete(config);
}
#endif
