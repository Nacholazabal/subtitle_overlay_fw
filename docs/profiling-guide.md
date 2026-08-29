# Profiling Guide

## Overview

This project uses **Linux perf** for unified profiling across the entire stack:
- Python server/bridge (STT processing)
- C firmware (subtitle rendering, HDMI overlay)
- Kernel (DMA, interrupts, drivers)

All profiling data exports to **Perfetto** for timeline visualization.

## Quick Start

```bash
# 1. Deploy firmware with perf-enabled rootfs (already built)
# 2. Run your test on the board
# 3. Capture perf trace (30 seconds)
./scripts/capture_perf_trace.sh 30

# 4. Convert to Perfetto
python3 scripts/perf_to_perfetto.py logs/perf_trace.txt logs/trace.json

# 5. Open in browser
# Visit https://ui.perfetto.dev
# Click "Open trace file" → select logs/trace.json
```

## What You Get

**perf captures:**
- Function-level timing (which functions are slow)
- Call stacks (who called what)
- CPU cycles per function
- Both Python and C code
- Kernel activity (DMA, IRQs)

**Perfetto shows:**
- Timeline view of all activity
- Zoom into microsecond-level events
- See where CPU time is spent
- Identify bottlenecks visually

## Detailed Workflow

### 1. Enable perf in PetaLinux (One-time setup)

Already done! The build script `scripts/enable_perf.sh` has:
- Enabled CONFIG_perf in rootfs
- Rebuilt PetaLinux
- perf tool is now in the boot image

### 2. Deploy to Board

```bash
# Package boot image (when ready to deploy to board)
ssh petalinux-vm "cd /home/tesislinux/tesis/hdmi-overlay && \
  petalinux-package --boot --force \
    --fsbl images/linux/zynq_fsbl.elf \
    --fpga images/linux/system.bit \
    --u-boot"

# Copy to SD card (from VM)
# - Copy BOOT.BIN to SD card FAT partition
# - Copy image.ub to SD card FAT partition
```

### 3. Capture Trace on Board

**Option A: Automatic capture script**
```bash
./scripts/capture_perf_trace.sh 30  # 30 second capture
```

**Option B: Manual capture**
```bash
# SSH to board
ssh root@192.168.1.10

# Start perf recording
perf record -p $(pidof subtitle_overlay_fw) -g -F 999 sleep 30

# Convert to text
perf script -F time,comm,pid,tid,event,ip,sym > /tmp/perf_trace.txt

# Copy back to WSL
exit
scp root@192.168.1.10:/tmp/perf_trace.txt logs/
```

### 4. Convert to Perfetto

```bash
python3 scripts/perf_to_perfetto.py logs/perf_trace.txt logs/trace.json
```

### 5. Visualize in Perfetto

1. Open https://ui.perfetto.dev in browser
2. Click "Open trace file"
3. Select `logs/trace.json`
4. Navigate the timeline:
   - Scroll = zoom in/out
   - Click events to see details
   - W/S keys = zoom
   - A/D keys = pan left/right

## What to Look For

### CPU Hotspots
- Long bars = functions taking time
- Look for repeated patterns
- Check if rendering or network is slow

### Function Call Depth
- Deep stacks = nested function calls
- Look for unnecessary recursion
- Check call frequency

### Idle vs Busy
- Gaps = CPU idle (good for latency)
- Solid blocks = CPU working
- Many small events = overhead

## Common Profiling Scenarios

### Find Rendering Bottleneck
```bash
# Capture while subtitle is updating
./scripts/capture_perf_trace.sh 30

# In Perfetto, search for:
# - subtitle_text_render
# - subtitle_bram_write
# - font_rasterize
```

### Measure Network Overhead
```bash
# Search in Perfetto for:
# - stt_ws_client
# - usb_audio_stream
# - TCP/socket functions
```

### Check Event Queue Latency
```bash
# Look for QP/C event handling:
# - QActive_post
# - QF_run
# - State machine handlers
```

## Integration with Existing Tools

### Combine with JSONL Events

You already have STT server timing in `logs/stt_events.jsonl`. To correlate:

1. Note the wall-clock time when you start perf
2. Check timestamps in JSONL events
3. Align both in Perfetto (coming soon: merge script)

### Compare with analyze_run.py

```bash
# Run both profilers together
./scripts/capture_perf_trace.sh 30 &
# ... run test ...
python3 -m server.audio_tests.analyze_run

# analyze_run.py gives you statistics (p50/p90)
# Perfetto gives you timeline visualization
```

## Performance Tips

### Reduce Overhead
```bash
# Lower sample frequency (default is 999 Hz)
perf record -F 99 ...  # 99 samples/second instead of 999

# Target specific process only
perf record -p $(pidof subtitle_overlay_fw) ...
```

### Longer Captures
```bash
# Capture 5 minutes
./scripts/capture_perf_trace.sh 300

# Note: larger traces = more disk space
# 30s ≈ 1-5 MB
# 300s ≈ 10-50 MB
```

### Filter by Function

```bash
# On board, after perf record:
perf script -F time,comm,pid,tid,event,ip,sym | grep subtitle > filtered_trace.txt
```

## Troubleshooting

### "perf: command not found"
- Did you rebuild PetaLinux with perf enabled?
- Check: `ssh root@192.168.1.10 'which perf'`

### "No permission to enable events"
- perf requires root
- Check: `ssh root@192.168.1.10 'id'` should show uid=0(root)

### Empty trace
- Is subtitle_overlay_fw running?
- Check: `ssh root@192.168.1.10 'pidof subtitle_overlay_fw'`

### Symbols show as addresses (0x12345678)
- Firmware not stripped? Check Makefile STRIP settings
- Symbol table missing? Rebuild with debug info

## Next Steps

Once you have perf working:

1. **Automated profiling** - Add perf capture to test scripts
2. **Regression tracking** - Compare traces across git commits
3. **Latency budgets** - Define acceptable timing per function
4. **Custom trace points** - Add USDT markers for specific events

## References

- perf wiki: https://perf.wiki.kernel.org/
- Perfetto docs: https://perfetto.dev/docs/
- Brendan Gregg's perf examples: http://www.brendangregg.com/perf.html
