import re
import shutil
import subprocess
import sys
from pathlib import Path


def bundle(executable, destination, dll_root):
    destination.mkdir(parents=True, exist_ok=True)
    available = {path.name.lower(): path for path in dll_root.glob("*.dll")}
    pending = [executable]
    copied = set()
    while pending:
        source = pending.pop()
        if source.name.lower() in copied:
            continue
        shutil.copy2(source, destination / source.name)
        copied.add(source.name.lower())
        headers = subprocess.check_output(["objdump", "-p", str(source)], text=True)
        for dll in re.findall(r"DLL Name:\s*(\S+)", headers):
            dependency = available.get(dll.lower())
            if dependency is not None and dependency.name.lower() not in copied:
                pending.append(dependency)


if __name__ == "__main__":
    bundle(*(Path(argument) for argument in sys.argv[1:]))
