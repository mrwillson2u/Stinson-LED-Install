import os
from pathlib import Path
from dotenv import dotenv_values
# Import("env")

# Ensure correct project root
project_dir = Path(os.getcwd())
env_file = project_dir / ".env"

if not env_file.exists():
    print(f"[extra_script.py] WARNING: {env_file} not found")

env_vars = dotenv_values(env_file)

# print("[extra_script.py] Loaded variables:", env_vars)

# Inject as -DKEY="value"
# env.Append(CPPDEFINES=[(key, f'"{value}"') for key, value in env_vars.items()])
# for key, value in env_vars.items():
#     env.Append(CPPDEFINES=[f'{key}="{value}"'])
# Dynamically set upload port
# if "UPLOAD_PORT" in env_vars:
#     env.Replace(UPLOAD_PORT=env_vars["UPLOAD_PORT"])
def get_vars():
    for key, value in env_vars.items():
        print(f"'-D {key}=\"{value}\"\n'")

