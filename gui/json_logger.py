"""Game Result Logger Module (json_logger.py) (By Gemini)

This module automatically collects game metadata when a match concludes. 
It retrieves player types, selected algorithms, and search depths directly from the 
Tkinter SidePanel UI and active engines, exporting them into a formatted JSON log file.
"""

import os
import sys
import json
from pathlib import Path
from datetime import datetime

try:
    from gui.logger import log
except ImportError:
    from logger import log

def get_persistent_path(relative_path: str) -> Path:
    """ Get path to the actual folder where the EXE is (Read-Write) """
    if getattr(sys, 'frozen', False):
        # Path to the folder containing the .exe
        base_path = Path(sys.executable).parent
    else:
        # Path to the folder containing the script
        base_path = Path(os.path.abspath("."))
    return base_path / relative_path

SAVES_DIR = get_persistent_path("records")

# [MAKEDIR] Try make records dir if not exist
if not os.path.exists(SAVES_DIR):
    log.info("The directory 'records' does not exist")
    SAVES_DIR.mkdir(parents=True, exist_ok=True)
    if not os.path.exists(SAVES_DIR):
        log.warning("The directory 'records' does not exist")
    else:
        log.info("Successfully maked the directory")

file_name: str = "game_results.json"
FILE_PATH = SAVES_DIR / file_name

def extract_player_info(app, color: str) -> dict:
    """Safely extracts player, algorithm, and search depth information from GameApp.

    Args:
        app: The GameApp instance (typically 'self' in main.py).
        color: Either "white" or "black".
        
    Returns:
        A dictionary containing "type", "name", "algorithm", and "depth".
    """
    # Fetch the state dictionary directly (app.white or app.black)
    side_data = getattr(app, color, None)
    
    # Safely retrieve the active engine process if initialized
    engine_attr = f"{color}_uci_engine"
    engine = getattr(app, engine_attr, None)

    # 1. Check if the player is a Human or Bot
    is_bot = False
    engine_path = None
    
    if isinstance(side_data, dict):
        engine_path = side_data.get("engine")
        is_bot = (engine_path is not None)
    elif engine is not None:
        is_bot = True

    # Default configuration for a Human player
    if not is_bot:
        return {
            "type": "Human",
            "name": "Human Player",
            "algorithm": "N/A",
            "depth": "N/A"
        }

    # 2. Extract AI Engine Name
    engine_name = "minichess-ubgi"
    if engine is not None:
        if getattr(engine, "name", None):
            engine_name = engine.name
        elif getattr(engine, "id_name", None):
            engine_name = engine.id_name
        elif getattr(engine, "_exe_path", None):
            engine_name = os.path.basename(engine._exe_path)
    elif engine_path:
        engine_name = os.path.basename(engine_path)
    else:
        engine_name = "Bot"

    # 3. Extract search depth (0 means "Use time limit" in the framework)
    depth_val = "0 (Use time limit)"
    if isinstance(side_data, dict) and "depth" in side_data:
        raw_depth = side_data["depth"]
        if raw_depth != 0:
            depth_val = str(raw_depth)
            
    # 4. Extract selected search algorithm
    algo_val = "AlphaBeta-Search" # Default fallback
    if isinstance(side_data, dict) and "algo" in side_data:
        algo_val = str(side_data["algo"])

    return {
        "type": "AI",
        "name": engine_name,
        "algorithm": algo_val,
        "depth": depth_val
    }

def save_game_to_json(app, result_str: str) -> None:
    """Aggregates all game information and saves/appends it to a local JSON file."""
    filepath: str = FILE_PATH
    
    # Extract player details for both sides
    white_info = extract_player_info(app, "white")
    black_info = extract_player_info(app, "black")
    
    # Process move history into a list of readable strings (e.g., ["E2E4", "D7D5"])
    raw_moves = getattr(app, "move_history", [])
    moves_list = []
    for m in raw_moves:
        moves_list.append(str(m))
        
    # Construct the game log record
    record: dict[str, object] = {
        "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "game_result": result_str.replace("p0_", "White ").replace("p1_", "Black "),
        "players": {
            "white": white_info,
            "black": black_info
        },
        "moves_count": len(moves_list),
    }
    
    #record["moves_history"] = moves_list
    
    # Load existing logs if the file already exists
    records = []
    if os.path.exists(filepath):
        try:
            with open(filepath, "r", encoding="utf-8") as f:
                records = json.load(f)
                if not isinstance(records, list):
                    records = []
        except Exception as e:
            log.warning(f"[Warning] Failed to read existing JSON. Creating new file. Error: {e}")
            records = []
            
    # Append the new record
    records.append(record)
    
    # Write updated records back to the JSON file
    try:
        with open(filepath, "w", encoding="utf-8") as f:
            json.dump(records, f, indent=4, ensure_ascii=False)
        log.info(f"[Success] Game record saved successfully to {filepath}!")
    except Exception as e:
        log.warning(f"[Error] Failed to write JSON: {e}")
