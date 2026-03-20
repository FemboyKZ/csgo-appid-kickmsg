
# Steam Wrong AppID Custom Kick Message

SM extension to patch engine to show a custom kick message to players on join when running on the wrong AppID.

## Building

### AMBuild

```py
mkdir build && cd build

python ../configure.py --sm-path ../sourcemod --mms-path ../metamod-source --targets x86

ambuild
```

### Configuration

A default kick message will be generated in `addon/sourcemod/configs/csgo_appid_kickmsg.txt` on first load.
