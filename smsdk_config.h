#ifndef _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_

/**
 * @file smsdk_config.h
 * @brief Contains macros for configuring basic extension information.
 */

/* Basic information exposed publicly */
#define SMEXT_CONF_NAME         "CS:GO Custom Kick Message on Wrong AppID"
#define SMEXT_CONF_DESCRIPTION  "Patches the engine to show a custom kick message when clients with the wrong AppID try to connect to a CS:GO server."
#define SMEXT_CONF_VERSION      "0.0.1"
#define SMEXT_CONF_AUTHOR       "jvnipers"
#define SMEXT_CONF_URL          "https://github.com/FemboyKZ/csgo-appid-kickmsg"
#define SMEXT_CONF_LOGTAG       "appid"
#define SMEXT_CONF_LICENSE      "AGPL"
#define SMEXT_CONF_DATESTRING   __DATE__

#define SMEXT_LINK(name) SDKExtension *g_pExtensionIface = name;
#define SMEXT_CONF_METAMOD
#define SMEXT_ENABLE_ROOTCONSOLEMENU

#endif // _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_
