Import("env")

import subprocess


def git_output(*args):
    try:
        return subprocess.check_output(
            ["git", *args],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


git_hash = git_output("rev-parse", "--short=12", "HEAD") or "unknown"
if git_hash != "unknown" and git_output("status", "--porcelain"):
    git_hash += "-dirty"

env.Append(CPPDEFINES=[("FIRMWARE_GIT_HASH", '\\"%s\\"' % git_hash)])
print("Firmware Git hash: %s" % git_hash)
