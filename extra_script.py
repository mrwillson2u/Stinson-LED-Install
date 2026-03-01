import os
from pathlib import Path
from dotenv import dotenv_values
Import("env")

# Ensure correct project root
project_dir = Path(os.getcwd())
env_file = project_dir / ".env"

if not env_file.exists():
    print(f"[extra_script.py] WARNING: {env_file} not found")

env_vars = dotenv_values(env_file)

print("[extra_script.py] Loaded variables:", env_vars)

# Inject as -DKEY="value"
env.Append(CPPDEFINES=[(key, f'\\"{value}\\"') for key, value in env_vars.items()])


# Only set upload port for OTA environment (avoids overriding USB uploads)
if "UPLOAD_PORT" in env_vars and env_vars["UPLOAD_PORT"] and env["PIOENV"] == "esp32_ota":
    env.Replace(UPLOAD_PORT=env_vars["UPLOAD_PORT"])
