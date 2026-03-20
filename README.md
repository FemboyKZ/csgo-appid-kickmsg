
# Steam Wrong AppID Custom Kick Message

SM extension to patch engine to show a custom kick message to players on join when running on the wrong AppID.

## Building

### AMBuild

```py
mkdir build && cd build

python ../configure.py --sm-path ../sourcemod --mms-path ../metamod-source --targets x86

ambuild
```

## Configuration

A default kick message will be generated in `addon/sourcemod/configs/csgo_appid_kickmsg.txt` on first load.

Default kick message:

```txt
This server does not support your client version.
 
Use the standalone CSGO client.
https://store.steampowered.com/app/4465480/CounterStrikeGlobal_Offensive
```
