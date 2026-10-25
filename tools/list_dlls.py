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
queue = ["build/iriswinds.exe"]
if os.path.exists("build/iriswinds-config.exe"):
    queue.append("build/iriswinds-config.exe")
if os.path.exists("build/IrisWinds Configuration.exe"):
    queue.append("build/IrisWinds Configuration.exe")

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
dist_dir = "build/dist/IrisWinds"
os.makedirs(dist_dir, exist_ok=True)
os.makedirs("build", exist_ok=True)

for name, path in sorted(all_dlls.items()):
    print(f"  {name} -> {path}")
    dest_dist = os.path.join(dist_dir, name)
    if os.path.exists(path):
        if not os.path.exists(dest_dist) or os.path.getsize(dest_dist) != os.path.getsize(path):
            shutil.copy2(path, dest_dist)
            print(f"    Copied {name} to {dist_dir}")

        dest_build = os.path.join("build", name)
        if not os.path.exists(dest_build) or os.path.getsize(dest_build) != os.path.getsize(path):
            shutil.copy2(path, dest_build)
            print(f"    Copied {name} to build/")

# Also copy all .dll from build/ to dist_dir
for dll in glob.glob("build/*.dll"):
    name = os.path.basename(dll)
    dest_dist = os.path.join(dist_dir, name)
    if not os.path.exists(dest_dist) or os.path.getsize(dest_dist) != os.path.getsize(dll):
        shutil.copy2(dll, dest_dist)
        print(f"  Copied build/{name} to {dist_dir}")

print("DLL bundling completed successfully!")
