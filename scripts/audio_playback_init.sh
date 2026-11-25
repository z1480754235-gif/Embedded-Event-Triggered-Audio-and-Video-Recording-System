#!/bin/sh
set -e

amixer -c 0 sset 'Left Output Mixer PCM' on
amixer -c 0 sset 'Right Output Mixer PCM' on
amixer -c 0 sset 'Playback' 220,220
amixer -c 0 sset 'Speaker' 110,110

echo "Speaker configuration ready"