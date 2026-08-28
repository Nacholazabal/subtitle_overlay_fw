# ARCH-04 Implementation Status

## Completed Findings

### F12 - Logging consolidation ✓
- **Status**: Complete
- **Changes**:
  - Raised LOG_MAX_MESSAGE_LENGTH from 128 to 256 bytes
  - Converted video_dma, video_dynclk, hw_platform to use LOG_ERROR instead of fprintf(stderr)
  - Added monotonic timestamps to app_log_output (ms since start)
  - Fixed log.h documentation

### F11 - Error vocabulary conversion ✓
- **Status**: Complete
- **Changes**:
  - Converted all video HAL modules (video_dma, video_vtc, video_gpio, video_dynclk) to errno
  - Mapping: XST_SUCCESS→0, XST_INVALID_PARAM→-EINVAL, XST_DEVICE_NOT_FOUND→-ENODEV, XST_NO_DATA→-ENODATA, XST_FAILURE→-EIO
  - Updated service layer (video_io, video_pipeline, VideoAO) to use errno
  - Real error codes now reach SystemAO instead of flat -EIO

### F13 - Board identity leakage ✓
- **Status**: Complete
- **Changes**:
  - Created video_vtc_init_detector() and video_vtc_init_generator() with role-based init
  - Removed xparameters.h include from video_io.c (service layer)
  - Device IDs (XPAR_V_TC_0/1) now resolved internally in HAL

### F15 - STT parsing duplicate ✓
- **Status**: Complete
- **Changes**:
  - stt_event_ring_push now calls stt_transcript_parse_line for finality
  - Removed whitespace-sensitive strstr("is_final":true)
  - Messages with `"is_final": true` (with space) now classify correctly

### F9 - Audio sink interface ✓
- **Status**: Complete
- **Changes**:
  - Created audio_sink.h interface for dependency inversion
  - Created stt_audio_sink adapter wrapping stt_ws_client_submit_audio
  - usb_audio_stream no longer includes stt_ws_client.h
  - Producer is now testable in isolation

## Incomplete Finding

### F14 - Config layer ✗
- **Status**: NOT STARTED
- **Scope**: 22 environment variables scattered across 4 modules
- **Required Work**:
  1. Create src/app/app_config.{c,h}
  2. Move all getenv calls into app_config.c
  3. Read and validate all variables once at startup
  4. Log complete resolved configuration as one block
  5. Document all 22 variables in one place
  6. Extend existing *_config_t structs to receive values from app_config
  
- **Variables to centralize**:
  - src/svc/stt/stt_ws_config.c: 16 SUBTITLE_STT_* vars
  - src/svc/usb_audio/usb_audio_stream.c: 3 USB_AUDIO_* vars
  - src/hal/usb_audio/usb_audio_capture.c: 3 mixer/volume vars
  - src/svc/subtitle_pipeline/SubtitleAO.c: 2 timeout vars (via resolve_timeout_ticks)

## Test Status
- Build: ✓ PASSING (./scripts/build.sh succeeds)
- Tests: Pending final verification

## Next Steps for F14
1. Create app_config skeleton with all 22 variable definitions
2. Implement parse/validate functions
3. Update each module's *_config_t to receive parsed values
4. Add startup logging of complete config
5. Document variables in docs/configuration.md or app_config.h
