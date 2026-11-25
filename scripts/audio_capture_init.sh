#!/bin/sh
set -e

amix() {
    amixer -c 0 "$@"
}

# 关闭右侧输入
amix sset 'Right Input Mixer Boost' off
amix sset 'Right Boost Mixer RINPUT1' off
amix sset 'Right Boost Mixer RINPUT2' off
amix sset 'Right Boost Mixer RINPUT3' off
amix cset name='Right Input Boost Mixer RINPUT2 Volume' 0
amix cset name='Right Input Boost Mixer RINPUT3 Volume' 0

# 板载 MIC 使用左侧 LINPUT2 通路
amix sset 'Left Boost Mixer LINPUT1' off
amix sset 'Left Boost Mixer LINPUT2' on
amix sset 'Left Boost Mixer LINPUT3' off
amix sset 'Left Input Mixer Boost' on
amix cset name='Left Input Boost Mixer LINPUT2 Volume' 5
amix cset name='Left Input Boost Mixer LINPUT3 Volume' 0

# 固定增益，左右 ADC 独立输出
amix cset name='ALC Function' Off
amix cset name='Capture Volume' 40,40
amix cset name='Capture Switch' on,on
amix cset name='ADC PCM Capture Volume' 195,195
amix cset name='ADC Data Output Select' 0

echo "Microphone configuration ready"