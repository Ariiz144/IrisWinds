import subprocess
import os
import glob
import shutil

def to_win_path(p):
    if p.startswith("/c/"):
        return "C:/" + p[3:]
    if p.startswith("/C/"):
        return "C:/" + p[3:]
    return p

def get_deps(binary_path):
    try:
        out = subprocess.check_output(["ldd", binary_path], stderr=subprocess.DEVNULL).decode(errors='ignore')
    except Exception as e:
        return {}
    deps = {}
    for line in out.splitlines():
        if "=>" in line:
            parts = line.strip().split("=>")
            name = parts[0].strip()
            path = parts[1].split("(")[0].strip()
            if path and not path.lower().startswith("/c/windows"):
                deps[name] = to_win_path(path)
    return deps

all_dlls = {}
queue = []
for exe in ["build/iriswinds.exe", "build/iriswinds-config.exe", "build/IrisWinds Configuration.exe"]:
    if os.path.exists(exe):
        queue.append(exe)

visited = set()
while queue:
    curr = queue.pop(0)
    if curr in visited:
        continue
    visited.add(curr)
    deps = get_deps(curr)
    for name, path in deps.items():
        if name not in all_dlls:
            all_dlls[name] = path
            queue.append(path)

print(f"Total non-system DLL dependencies found: {len(all_dlls)}")
os.makedirs("build", exist_ok=True)

for name, path in sorted(all_dlls.items()):
    print(f"  {name} -> {path}")
    dest_build = os.path.join("build", name)
    if os.path.exists(path):
        if not os.path.exists(dest_build) or os.path.getsize(dest_build) != os.path.getsize(path):
            shutil.copy2(path, dest_build)
            print(f"    Copied {name} to build/")

print("DLL bundling completed successfully!")
