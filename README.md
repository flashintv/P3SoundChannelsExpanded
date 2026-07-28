# P3SoundChannelsExpanded
Written to fix an issue where being fired at by more than a couple of enemies caused all sounds to cut out.
Code first started simple with 2 detours and a lot of memory patching, but then delved into madness because I realized that a ton of functions had instances of the CChannelList class which stores the channel list with a fixed-size buffer inside of it.

## Showcase
| Vanilla | P3SoundChannelsExpanded |
| --- | --- |
| [![YouTube](http://i.ytimg.com/vi/lO8K9Tiz0vQ/hqdefault.jpg)](https://www.youtube.com/watch?v=lO8K9Tiz0vQ) | [![YouTube](http://i.ytimg.com/vi/FyHzlFt-Ce0/hqdefault.jpg)](https://www.youtube.com/watch?v=FyHzlFt-Ce0) |

## Credits
* alliedmodders for their CSigScan code (https://wiki.alliedmods.net/Signature_Scanning)
* cursey for their safetyhook project (https://github.com/cursey/safetyhook)
* Valve Software for their Source Engine code used within the project
* Kizoky for reporting this issue
