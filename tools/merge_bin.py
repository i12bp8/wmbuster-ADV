"""PlatformIO post-build step: write a single image (bootloader, partition
table, boot_app0 and the application) that flashes at offset 0x0, e.g. with
https://espressif.github.io/esptool-js/ or M5Burner.

Output: .pio/build/<env>/wmbuster-adv-full.bin
"""
import os

Import("env")  # noqa: F821


def merge_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    app = os.path.join(build_dir, env.subst("${PROGNAME}.bin"))
    out = os.path.join(build_dir, "wmbuster-adv-full.bin")
    esptool = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy") or "", "esptool.py")
    images = []
    for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
        images += [env.subst(offset), env.subst(image)]
    images += [env.subst("$ESP32_APP_OFFSET"), app]
    cmd = [
        env.subst("$PYTHONEXE"), esptool, "--chip", env.BoardConfig().get("build.mcu", "esp32s3"),
        "merge_bin", "-o", out, "--flash_mode", "keep", "--flash_freq", "keep",
        "--flash_size", env.BoardConfig().get("upload.flash_size", "8MB"),
    ] + images
    if env.Execute(" ".join('"%s"' % c for c in cmd)) == 0:
        print("merge_bin: %s" % os.path.relpath(out, env.subst("$PROJECT_DIR")))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)  # noqa: F821
