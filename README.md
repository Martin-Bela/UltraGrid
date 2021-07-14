GitHub CI Linux QuickSync (libmfx)
==================================

There are 2 attempts in this branch:

- U18.04 with build from source - that will probably never work
- u20.04
  * needed to add manually libmfxhw64.so.1

Problems
---------
### U18
- perhaps hardcoded location of the actual library libmfxhw64.so.1
- also hardcoded location of plugins

### U20
- hardcoded location of plugins - Windows allows bundling them as described
[here](https://github.com/Intel-Media-SDK/MediaSDK/blob/master/doc/mediasdkusr-man.md#application-folder-installation) but not for Linux

####
- possible solution - use system libmfx.so.1, libmfxhw64.so.1.34 (`LD_PRELOAD`) - not sure if needed to use also system va

Problem:
- `some encoding parameters are not supported by the QSV runtime. Please double check the input parameters.` (compiled with system mfx U20.04, run on debian 11)
