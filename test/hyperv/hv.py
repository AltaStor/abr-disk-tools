#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Runs the Hyper-V tests on a host from somewhere else, over PowerShell
# remoting (WinRM). See doc/testing.md.
#
#   hv.py HOST deploy [--exe PATH]        copy the tool and test/hyperv/*.ps1 to the host
#   hv.py HOST run [--secret P=VAR]... SCRIPT [ARGS...]
#                                         run a deployed script, e.g. Test-Vm.ps1 -VmName vm1
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
#
# A script's own secrets, such as Test-SmbShare.ps1's share password, go with
# 'run --secret SharePassword=VAR': the value of the local variable VAR, or
# one asked for when VAR is not set, becomes the script's -SharePassword as a
# SecureString. It travels as a PSRP secure string, encrypted with a session
# key on top of the WinRM session's own encryption, and is never part of the
# script text the host runs -- which PowerShell may log (event 4104) even
# with no logging policy set, for any script it deems suspicious.
import argparse
import getpass
import glob
import os
import re
import sys

try:
    from pypsrp.client import Client
    from pypsrp.complex_objects import ObjectMeta
    from pypsrp.powershell import PowerShell, RunspacePool
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


def run_script(client, script, words, secrets):
    # The secrets are bound as parameters of a wrapper, not written into it:
    # the text names only $Name, and the value comes as a serialized secure
    # string, which PowerShell decrypts straight into a SecureString.
    with RunspacePool(client.wsman) as pool:
        ps = PowerShell(pool)
        names = [name for name, _ in secrets]
        text = "param(%s)\n& %s %s %s" % (
            ", ".join("[securestring]$" + n for n in names), quote(script), " ".join(words),
            " ".join("-%s $%s" % (n, n) for n in names))
        ps.add_script(text)
        if secrets:
            pool.exchange_keys()
            for name, value in secrets:
                ps.add_parameter(name, pool.serialize(value, ObjectMeta("SS")))
        ps.add_cmdlet("Out-String").add_parameter("Stream", True)
        output = ps.invoke()
        out = "\n".join(output)
        sys.stdout.write(out + ("\n" if out and not out.endswith("\n") else ""))
        for e in ps.streams.error:
            sys.stdout.write("PSERR: %s\n" % e)
        return 1 if ps.had_errors else 0


def read_secrets(specs):
    secrets = []
    for spec in specs:
        name, _, var = spec.partition("=")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or not var:
            sys.exit("Error: --secret takes PARAMETER=VARIABLE, not %r" % spec)
        value = os.environ.get(var)
        if value is None:
            value = getpass.getpass("%s (%s is not set): " % (name, var))
        secrets.append((name, value))
    return secrets


def main():
    parser = argparse.ArgumentParser(description="Run abr-vhdx-tool's Hyper-V tests over WinRM")
    parser.add_argument("host")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("deploy", help="copy the tool and the test scripts to the host")
    p.add_argument("--exe", default=os.path.join(HERE, "..", "..", "build-windows-cross", "bin", "abr-vhdx-tool.exe"))
    p = sub.add_parser("run", help="run a deployed test script")
    p.add_argument("--secret", action="append", default=[], metavar="PARAMETER=VARIABLE",
                   help="pass the local variable VARIABLE as the script's SecureString -PARAMETER")
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

    # Asked for before connecting, so that a prompt for a secret is not
    # mistaken for the one for the account.
    secrets = read_secrets(args.secret) if args.cmd == "run" else []
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
        return run_script(client, REMOTE_DIR + "\\" + args.script, words, secrets)
    if args.cmd == "ps":
        return run_ps(client, sys.stdin.read() if args.script == "-" else args.script)
    if args.cmd == "put":
        client.copy(args.local, args.remote)
    else:
        client.fetch(args.remote, args.local)
    return 0


if __name__ == "__main__":
    sys.exit(main())
