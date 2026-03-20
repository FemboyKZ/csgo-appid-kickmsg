#ifndef _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_

#include "smsdk_ext.h"

class CCSGOAppIDKickMsg : public SDKExtension, public IRootConsoleCommand
{
public:
    virtual bool SDK_OnLoad(char *error, size_t maxlength, bool late);
    virtual void SDK_OnAllLoaded();
    virtual void SDK_OnUnload();

    // IRootConsoleCommand
    void OnRootConsoleCommand(const char *cmdname, const ICommandArgs *args) override;
};

#endif // _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_