#!/bin/sh
# Makes the clips of test_media_ffmpeg with FFmpeg's command-line tool (the ua-media-ffmpeg-*
# test image) into $1, then runs the test on them. Test patterns and tones only.
set -e
D=${1:-/tmp/clips}
mkdir -p "$D"
F="ffmpeg -loglevel error -y"
TONES="-f lavfi -i sine=frequency=440:sample_rate=48000:duration=1 -f lavfi -i sine=frequency=660:sample_rate=48000:duration=1"
PIC="-f lavfi -i testsrc2=size=160x96:rate=30:duration=1"
$F $TONES -filter_complex "[0][1]amerge=inputs=2" -c:a aac -b:a 128k "$D/aac.mp4"
$F $PIC -c:v libx264 -profile:v high -bf 2 -g 10 -pix_fmt yuv420p "$D/high.mp4"
$F $PIC -c:v libx264 -flags +ildct+ilme -x264-params tff=1 -pix_fmt yuv420p "$D/interlaced.mp4"
$F $PIC $TONES -filter_complex "[1][2]amerge=inputs=2[a]" -map 0 -map "[a]" -c:v libvpx -b:v 1M -c:a libvorbis "$D/ff.webm"
$F $PIC $TONES -filter_complex "[1][2]amerge=inputs=2[a]" -map 0 -map "[a]" -c:v libx264 -c:a aac "$D/ff.mkv"
exec "$2" "$D"
