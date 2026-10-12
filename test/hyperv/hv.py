#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Runs the Hyper-V tests on a host from somewhere else, over PowerShell
# remoting (WinRM). See doc/testing.md.
#
#   hv.py HOST deploy [--exe PATH]        copy the tool and test/hyperv/*.ps1 to the host
#   hv.py HOST run SCRIPT [ARGS...]       run a deployed script, e.g. Test-Vm.ps1 -VmName vm1
#   hv.py HOST ps  SCRIPT|-               run PowerShell given inline, or on stdin
#   hv.py HOST put LOCAL REMOTE           copy a file to the host
#   hv.py HOST get REMOTE LOCAL           fetch a file from the host
#
# The account comes from the environment, never from the command line, where
# it would land in the shell history and the process list:
#
#   HV_USER       DOMAIN\user, a member of the host's Administrators
#   HV_PASSWORD   asked for when not set
#   HV_AUTH       ntlm (default), kerberos, negotiate, credssp, basic
#   HV_SSL        1 for WinRM over HTTPS (5986) instead of HTTP (5985)
#   HV_DIR        directory on the host, default C:\abr-test
import argparse
import getpass
import glob
import os
import sys

try:
    from pypsrp.client import Client
except ImportError:
    sys.exit("Error: pypsrp is not installed; pip install -r test/hyperv/requirements.txt")

HERE = os.path.dirname(os.path.abspath(__file__))
REMOTE_DIR = os.environ.get("HV_DIR", r"C:\abr-test")


def connect(host):
    user = os.environ.get("HV_USER")
    if not user:
        sys.exit("Error: set HV_USER to the account to log on with (DOMAIN\\user)")
    password = os.environ.get("HV_PASSWORD")
    if password is None:
        password = getpass.getpass("Password for %s: " % user)
    return Client(host, username=user, password=password,
                  ssl=os.environ.get("HV_SSL", "0") == "1",
                  auth=os.environ.get("HV_AUTH", "ntlm"),
                  operation_timeout=600, read_timeout=660)


def quote(value):
    return "'" + value.replace("'", "''") + "'"


def run_ps(client, script):
    out, streams, had_errors = client.execute_ps(script)
    sys.stdout.write(out + ("\n" if out and not out.endswith("\n") else ""))
    for e in streams.error:
        sys.stdout.write("PSERR: %s\n" % e)
    return 1 if had_errors else 0


def main():
    parser = argparse.ArgumentParser(description="Run abr-vhdx-tool's Hyper-V tests over WinRM")
    parser.add_argument("host")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("deploy", help="copy the tool and the test scripts to the host")
    p.add_argument("--exe", default=os.path.join(HERE, "..", "..", "build-windows-cross", "bin", "abr-vhdx-tool.exe"))
    p = sub.add_parser("run", help="run a deployed test script")
    p.add_argument("script")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p = sub.add_parser("ps", help="run PowerShell; '-' reads it from stdin")
    p.add_argument("script")
    p = sub.add_parser("put", help="copy a file to the host")
    p.add_argument("local")
    p.add_argument("remote")
    p = sub.add_parser("get", help="fetch a file from the host")
    p.add_argument("remote")
    p.add_argument("local")
    args = parser.parse_args()

    if args.cmd == "deploy" and not os.path.isfile(args.exe):
        sys.exit("Error: %s not found; build the tool first, or pass --exe" % args.exe)

    client = connect(args.host)
    if args.cmd == "deploy":
        run_ps(client, "New-Item -ItemType Directory -Force %s | Out-Null" % quote(REMOTE_DIR))
        for path in [args.exe] + sorted(glob.glob(os.path.join(HERE, "*.ps1"))):
            name = os.path.basename(path)
            print("%s -> %s\\%s" % (name, REMOTE_DIR, name))
            client.copy(path, REMOTE_DIR + "\\" + name)
        return 0
    if args.cmd == "run":
        # Switches and parameter names go through as they are; values are
        # quoted, so that a path with spaces stays one argument.
        words = [a if a.startswith("-") else quote(a) for a in args.args]
        return run_ps(client, "& %s %s" % (quote(REMOTE_DIR + "\\" + args.script), " ".join(words)))
    if args.cmd == "ps":
        return run_ps(client, sys.stdin.read() if args.script == "-" else args.script)
    if args.cmd == "put":
        client.copy(args.local, args.remote)
    else:
        client.fetch(args.remote, args.local)
    return 0


if __name__ == "__main__":
    sys.exit(main())
