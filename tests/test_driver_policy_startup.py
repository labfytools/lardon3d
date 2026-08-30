import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    environment = os.environ.copy()
    environment["MESA_SHADER_CACHE_DISABLE"] = "false"
    try:
        result = subprocess.run(
            [sys.argv[1]],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=4,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"unable to execute production boundary: {error}", file=sys.stderr)
        return 1

    expected = b"MESA_SHADER_CACHE_DISABLE must be true for safe CPU affinity\n"
    if result.returncode != 1 or result.stdout or result.stderr != expected:
        print(
            "unexpected production boundary result: "
            f"status={result.returncode} stdout={result.stdout!r} "
            f"stderr={result.stderr!r}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
