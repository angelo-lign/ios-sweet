#!/bin/sh
# Build the ios-sweet userland helpers for the iOS 16 root (arm64, A11: no
# arm64e), ad-hoc signed: the kernel makes signed binaries on the root
# volume platform binaries (patch 0035), so no other signature is needed.
#
#   userland/build.sh            -> userland/out/ (autorun, GPU tests, backboardd wrapper)
set -e
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -isysroot $SDK -O2 -Wall -Wextra"
mkdir -p out
build() { # OUT SRC... [-- extra flags]
	out=$1; shift
	$CC -o "out/$out" "$@"
	codesign -f -s - "out/$out"
	echo "out/$out"
}
build sweet-autorun autorun/sweet-autorun.c
build redwood-gpu-test gputest/redwood-gpu-test.c -framework IOKit -framework CoreFoundation
build sweet-bbd bbd/sweet-bbd.c
build libsweet-bbd.dylib bbd/bbd-insert.c -dynamiclib -install_name /usr/lib/libsweet-bbd.dylib -framework IOSurface -Wl,-not_for_dyld_shared_cache \
	-framework IOKit -framework CoreFoundation
build libsweet-insert-test.dylib dyldtest/insert-test.c -dynamiclib -install_name /private/var/tmp/libsweet-insert-test.dylib
# removable system apps: runs SystemAppMigrator; signed again with the entitlements it needs
build sweet-sysapps sysapps/sweet-sysapps.m -fobjc-arc -framework Foundation
codesign -f -s - --entitlements sysapps/entitlements.plist out/sweet-sysapps
build sweet-fs fsutil/sweet-fs.c -framework CoreFoundation
build hfsgrow fsutil/hfsgrow.c
# Wi-Fi: ios-flame's wland (userland/wland/README.md): modem + WLFW + WCN3990 radio from userland over /dev/pmem
(cd wland && $CC -Wno-unused-parameter -o ../out/wland wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c pdmapper.c \
	tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c) && codesign -f -s - out/wland && echo out/wland
# from ios-flame (userland/flame/README.md): LaunchServices registration, unified log stream
build lsreg flame/lsreg.m -fobjc-arc -framework Foundation -framework MobileCoreServices
codesign -f -s - --entitlements flame/lsreg-ents.plist out/lsreg
build oslog flame/oslog.m -fobjc-arc -framework Foundation
codesign -f -s - --entitlements flame/oslog-ents.plist out/oslog
build netsvc flame/netsvc.m -fobjc-arc -framework Foundation -framework SystemConfiguration
codesign -f -s - --entitlements flame/netsvc-ents.plist out/netsvc
build nettest flame/nettest.m -fobjc-arc -framework Foundation
# a network service for en0 (preferences.plist from before 0139 has none)
build wifisvc wifisvc/wifisvc.c -framework CoreFoundation -framework SystemConfiguration
codesign -f -s - --entitlements wifisvc/entitlements.plist out/wifisvc
# brightness slider / Night Shift (upstream coreycusick/ios-guacamole 0197): polls BKSDisplayBrightnessGetCurrent and writes DCS 0x51 over DSI0
# (install /var/local/bin/sweet-displayd; job com.sweet.displayd); needs the kernel's AppleARMBacklight (xnu 0178)
build sweet-displayd jb/sweet-displayd.m -fobjc-arc -framework Foundation
codesign -f -s - --entitlements jb/sweet-displayd-ents.plist out/sweet-displayd
# speaker audio (docs/guacamole.md, Audio): ADSP boot + APR (adspd) and the two TFA9874 speaker amps (tfa), over /dev/pmem
build adspd adsp/adspd.c adsp/edge.c wland/smem.c wland/qmi.c wland/pmem.c -Iwland -Wno-unused-parameter
build tfa adsp/tfa.c wland/pmem.c -Iwland -Wno-unused-parameter
# mediaserverd through libsweet-vabt (VirtualAudio routes without a codec), see jb/mswrap.c and jb/vabt.c
build mswrap jb/mswrap.c
build libsweet-vabt.dylib jb/vabt.c -dynamiclib -install_name /var/local/lib/libsweet-vabt.dylib -Wl,-not_for_dyld_shared_cache
# SweetCodec: HAL AudioServerPlugIn standing in for the iPhone's codec (docs/guacamole.md, Audio). The iOS SDK hides CoreAudio's
# plug-in headers: take them from the macOS SDK. Install as /Library/Audio/Plug-Ins/HAL/SweetCodec.driver.
MACSDK=$(xcrun --sdk macosx --show-sdk-path)
mkdir -p out/inc out/SweetCodec.driver
rm -f out/inc/CoreAudio
ln -s "$MACSDK/System/Library/Frameworks/CoreAudio.framework/Headers" out/inc/CoreAudio
$CC -bundle -Iout/inc -Wno-unused-parameter -o out/SweetCodec.driver/SweetCodec audio/sweetcodec.c -framework CoreFoundation
cp audio/Info.plist out/SweetCodec.driver/Info.plist
codesign -f -s - out/SweetCodec.driver && echo out/SweetCodec.driver
# G3: needs userland/mesa/mk-mesa.sh first (libOSMesa.8.dylib + headers)
MESA=mesa/build
if [ -f $MESA/out/libOSMesa.8.dylib ]; then
	# libOSMesa is @rpath/...: next to the test, or in /usr/lib for libsweet-gl
	build osmesa-triangle gputest/osmesa-triangle.c -I$MESA/mesa-25.0.7/include $MESA/out/libOSMesa.8.dylib \
		-Wl,-rpath,@executable_path
	cp $MESA/out/libOSMesa.8.dylib out/
	# G4: EAGLContext on Mesa for backboardd (eagl.c: HackiOS via topaz, MIT)
	python3 gpu/mkeagl.py gpu/eagl_slots.txt $MESA/out/gl.xml $MESA/out/exported-gl.txt gpu/eagl.c > out/eagl_gen.h
	build libsweet-gl.dylib gpu/eagl.c gpu/gl_osmesa.c -dynamiclib -install_name /usr/lib/libsweet-gl.dylib \
		-Iout -Igpu -I$MESA/mesa-25.0.7/include $MESA/out/libOSMesa.8.dylib -Wl,-rpath,/usr/lib \
		-Wl,-not_for_dyld_shared_cache -Wno-unused-but-set-variable \
		-framework OpenGLES -framework IOKit -framework IOSurface -framework CoreFoundation -lobjc
else
	echo "skipped osmesa-triangle: run userland/mesa/mk-mesa.sh"
fi
