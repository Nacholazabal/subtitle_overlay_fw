# F14 Config Layer - Remaining Work

## ✅ Completed
1. Created app_config.{c,h} infrastructure
2. Removed getenv from HAL (usb_audio_capture mixer settings)
3. Removed getenv from usb_audio_stream (AGC settings) 
4. Extended usb_audio_capture_config_t with mixer fields
5. Added app_config.c to Makefile

## ⚠️ In Progress - Build Currently Broken
The signature of `usb_audio_stream_start` changed to add AGC parameters, but the caller in USBAudioAO.c hasn't been updated yet. This will cause a compilation error.

## 🔧 TODO to Complete F14

### 1. Wire app_config into app.c
```c
// In main():
app_config_t app_cfg;
if (app_config_init(&app_cfg) != 0) {
    LOG_ERROR("app: config init failed");
    return 1;
}

// Pass to app_init or make global
```

### 2. Update USBAudioAO.c caller
```c
// In on_component_init:
extern app_config_t g_app_config; // or pass through
audio_sink_t sink = stt_audio_sink_create();

status = usb_audio_stream_start(&me->stream, 
                               &config,
                               &sink,
                               g_app_config.audio_agc_enabled,
                               g_app_config.audio_agc_target_pct);
```

### 3. Remove getenv from SubtitleAO.c
File: src/svc/subtitle_pipeline/SubtitleAO.c  
Lines: ~243 (resolve_timeout_ticks helper)
- SUBTITLE_CLEAR_TIMEOUT_MS
- SUBTITLE_PARTIAL_TIMEOUT_MS

These are already in app_config, just need to pass them through.

### 4. Clean up usb_audio_stream_default_config
File: src/svc/usb_audio/usb_audio_stream.c:320  
The copy_env_string call for USB_AUDIO_PCM_DEVICE should be removed since
it's now in app_config.

### 5. Add unit test
Create test/app/test_app_config.c:
- Test valid values are accepted
- Test invalid values use defaults and log warnings
- Test absent values use defaults
- Test complete config is logged

### 6. Document all 22 variables
Create docs/configuration.md or extend app_config.h header with full table:
- Variable name
- Default value  
- Valid range
- Description

## Acceptance Criteria Checklist
```
[ ] grep -rn 'getenv' src/ shows calls only in src/app/app_config.c
[ ] Startup logs complete resolved configuration as one block
[ ] All 22 variables documented in one place
[ ] usb_audio_capture receives mixer config (no getenv in HAL) ✓
[ ] scripts/run.sh updated to pass through key variables
[ ] make test green
[ ] make clang-tidy clean
[ ] ./scripts/build.sh succeeds
```

## Estimated Time to Complete
- Wire config + fix caller: 15 minutes
- Remove SubtitleAO getenv: 10 minutes
- Clean up default_config: 5 minutes
- Add unit test: 30 minutes
- Documentation: 20 minutes

**Total: ~80 minutes**

