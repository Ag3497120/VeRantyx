#!/usr/bin/env python3
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import vxrt_run


def check(condition, message):
    if not condition:
        raise SystemExit("vxrt run profile test failed: " + message)


with tempfile.TemporaryDirectory() as directory:
    root = os.path.join(directory, "game")
    os.mkdir(root)
    launcher = os.path.join(root, "EDLaunch.exe")
    with open(launcher, "wb") as stream:
        stream.write(b"test")
    detected = {"folder": root, "exe": os.path.join(root, "Game64.exe")}

    selected, error = vxrt_run.profile_executable({"exe": "EDLaunch.exe"}, detected)
    check(error is None and selected["exe"] == os.path.realpath(launcher), "profile entry point was not selected")
    check(detected["exe"].endswith("Game64.exe"), "resolution modified the discovery result")

    selected, error = vxrt_run.profile_executable({"exe": "missing.exe"}, detected)
    check(selected is detected and error and "does not exist" in error, "missing profile executable was not rejected")

    selected, error = vxrt_run.profile_executable({"exe": ""}, detected)
    check(selected is detected and error and "nonempty" in error, "empty profile executable was not rejected")

    selected, error = vxrt_run.profile_executable({"exe": "../outside.exe"}, detected)
    check(selected is detected and error and "outside" in error, "path escaping the install folder was not rejected")

    selected, error = vxrt_run.profile_executable({}, detected)
    check(error is None and selected is detected, "automatic executable discovery changed without a profile override")

    log = os.path.join(directory, "vxrt-EDLaunch.log")
    with open(log, "w", encoding="utf-8") as stream:
        stream.write("[vxrt] EDLaunch.exe: managed PE32 image (machine 0x014c, CLR metadata v4.0.30319); CLR hosting is not implemented\n")
    facts = vxrt_run.game_log_facts(log, "EDLaunch.exe")
    check(facts["first_problem"] and "CLR hosting is not implemented" in facts["first_problem"]["text"],
          "managed launcher rejection was not reported as the first problem")
    with open(log, "w", encoding="utf-8") as stream:
        stream.write("[vxrt] EDLaunch.exe: CLR App.Main: unsupported CIL opcode 0x73 in Main at IL_0000\n")
    facts = vxrt_run.game_log_facts(log, "EDLaunch.exe")
    check(facts["first_problem"] and "unsupported CIL opcode 0x73" in facts["first_problem"]["text"],
          "managed runtime CIL failure was not reported as the first problem")
    with open(log, "w", encoding="utf-8") as stream:
        stream.write("[vxrt] EDLaunch.exe: CLR App.InitializeComponent: WPF BAML/XAML loading is not implemented\n")
    facts = vxrt_run.game_log_facts(log, "EDLaunch.exe")
    check(facts["first_problem"] and "WPF BAML/XAML loading" in facts["first_problem"]["text"],
          "managed framework boundary was not reported as the first problem")

print("vxrt run profile: explicit install-relative entry point, missing/escaping paths and default discovery: ok")
