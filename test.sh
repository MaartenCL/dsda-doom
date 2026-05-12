#!/bin/bash

DOOM_EXE="./build/dsda-doom"
WAD_PATH="/data/games/doom/wads/doom.wad"
DEMO_PATH="/data/games/doom/dsda/replays"

# Launch the Host in a new window
konsole --noclose -e "$DOOM_EXE -iwad $WAD_PATH -complevel 3 -record $DEMO_PATH/testhost -host 2 -netlatency 100 30" &
# konsole --noclose -e "./build/dsda-doom -iwad /data/games/doom/wads/doom.wad -complevel 3 -record /data/games/doom/dsda/replays/testhost -host 2 -netlatency 100 30" &

# Launch the Client in a new window
konsole --noclose -e "$DOOM_EXE -iwad $WAD_PATH -complevel 3 -record $DEMO_PATH/testclient -join localhost -netlatency 100 30" &
# konsole --noclose -e "./build/dsda-doom -iwad /data/games/doom/wads/doom.wad -complevel 3 -record /data/games/doom/dsda/replays/testclient -join localhost -netlatency 100 30" &
