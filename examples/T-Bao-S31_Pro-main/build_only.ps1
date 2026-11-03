$ErrorActionPreference = 'Continue'

$idfPath = 'C:\esp-idf-master'
$pyEnv   = 'C:\Espressif\python_env\idf6.2_py3.11_env'
$proj    = $PSScriptRoot
$log     = Join-Path $proj 'build_only.log'

# Prefer the environment selected by the caller when the historical paths
# above are not installed on this machine.
if (-not (Test-Path (Join-Path $idfPath 'tools\idf.py')) -and $env:IDF_PATH) {
    $idfPath = $env:IDF_PATH
}
if (-not (Test-Path (Join-Path $pyEnv 'Scripts\python.exe')) -and $env:IDF_PYTHON_ENV_PATH) {
    $pyEnv = $env:IDF_PYTHON_ENV_PATH
}

# NOTE: activate.py --export is broken in this environment (it resolves a
# non-existent Z:\ESPIDF\... tools path and complains about missing tools), so we
# set the environment by hand instead of dot-sourcing the export script.
$env:IDF_PATH            = $idfPath
$env:IDF_TOOLS_PATH      = 'C:\Espressif\tools'
$env:IDF_COMPONENT_MANAGER = '1'
$env:ESP_IDF_VERSION     = '6.2.0'
$env:PYTHONUTF8          = '1'
# Installed riscv32-esp-elf is esp-15.2.0_20251204 but IDF master pins
# esp-16.1.0_20260609; the mismatch is benign (built fine before) so downgrade
# the version check to a warning.
$env:IDF_MAINTAINER      = '1'
$env:SDKCONFIG_DEFAULTS  = "$proj\sdkconfig.defaults;$proj\components\gen_bmgr_codes\board_manager.defaults"

$cmakeBin = 'C:\Espressif\tools\cmake\4.0.3\bin'
$ninjaBin = 'C:\Espressif\tools\ninja\1.12.1'
# Locate the riscv toolchain bin (glob in case the version dir differs).
$toolBin  = $null
foreach ($d in Get-ChildItem 'C:\Espressif\tools\riscv32-esp-elf\*\riscv32-esp-elf\bin' -ErrorAction SilentlyContinue) {
    if (Test-Path (Join-Path $d.FullName 'riscv32-esp-elf-gcc.exe')) { $toolBin = $d.FullName; break }
}
if (-not $toolBin) { $toolBin = 'C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin' }
$env:PATH = (@($toolBin, $cmakeBin, $ninjaBin, "$pyEnv\Scripts") + @($env:PATH)) -join ';'

Set-Location $proj

# --- Apply managed-component patches (idempotent) -------------------------
# av_processor: hook the decoded playback PCM through the ESP-Audio-Effects
# DSP chain (app_audio_dsp_playback_run) right before it is written to the
# codec. The symbol is provided by the app (main component) and linked at the
# final stage, so this is safe even before the app is compiled.
$avSrc = Join-Path $proj 'managed_components\jason-mao__av_processor\src\audio_processor.c'
if (Test-Path $avSrc) {
    $c = Get-Content -Path $avSrc -Raw -Encoding UTF8
    if ($c -notmatch 'app_audio_dsp_playback_run') {
        # 1) declare the hook
        $c = $c.Replace('#include "audio_processor.h"',
            "#include ""audio_processor.h""`n/* Patched by build_only.ps1: playback DSP hook */`nextern void app_audio_dsp_playback_run(uint8_t *data, int len);")
        # 2) call the hook in the feeder playback path (TTS -> codec)
        $c = $c.Replace('        esp_codec_dev_write(audio_manager.config.play_dev, blk->buf, blk->valid_size);',
            "        app_audio_dsp_playback_run((uint8_t *)blk->buf, blk->valid_size);`n        esp_codec_dev_write(audio_manager.config.play_dev, blk->buf, blk->valid_size);")
        Set-Content -Path $avSrc -Value $c -Encoding UTF8 -NoNewline
        Write-Output "[patch] av_processor: playback DSP hook applied" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] av_processor: playback DSP hook already present" | Tee-Object -FilePath $log -Append
    }
}
# --------------------------------------------------------------------------

# av_processor: sane AFE output (ASR upload) gains. AGC runs AFTER WakeNet in
# the 1MIC pipeline, so it only shapes the audio sent to the server, NOT the
# wake-word input. Previous hot values (linear 3.0 / target +3 dBFS / comp 40)
# made the upload pumped & distorted ("糊"). The playback-DSP hook above may
# have already rewritten this file; re-read it here.
$avSrc2 = Join-Path $proj 'managed_components\jason-mao__av_processor\src\audio_processor.c'
if (Test-Path $avSrc2) {
    $c2 = Get-Content -Path $avSrc2 -Raw -Encoding UTF8
    if ($c2 -notmatch 'VoCat-S31 AFE output tuning') {
        $c2 = $c2.Replace(
            "    audio_recorder.afe_cfg->wakenet_init = cfg ? cfg->ai_mode_wakeup : false;",
            "    audio_recorder.afe_cfg->wakenet_init = cfg ? cfg->ai_mode_wakeup : false;`n`n    /* VoCat-S31 AFE output tuning: AGC/linear gain run AFTER WakeNet, so they only shape ASR upload audio, not the wake-word input. Use sane ASR values. */`n    audio_recorder.afe_cfg->afe_linear_gain = 1.0f;`n    audio_recorder.afe_cfg->agc_target_level_dbfs = -12;`n    audio_recorder.afe_cfg->agc_compression_gain_db = 12;`n    ESP_LOGI(TAG, `"[AFE_GAIN] linear_gain=%.1f agc_target_dbfs=%d agc_compression_db=%d wakenet_init=%d afe_type=%d`",`n             audio_recorder.afe_cfg->afe_linear_gain,`n             audio_recorder.afe_cfg->agc_target_level_dbfs,`n             audio_recorder.afe_cfg->agc_compression_gain_db,`n             audio_recorder.afe_cfg->wakenet_init,`n             audio_recorder.afe_cfg->afe_type);")
        Set-Content -Path $avSrc2 -Value $c2 -Encoding UTF8 -NoNewline
        Write-Output "[patch] av_processor: AFE output tuning applied" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] av_processor: AFE output tuning already present" | Tee-Object -FilePath $log -Append
    }
}

# av_processor: force AEC OFF on VoCat-S31. This board has TWO real mics and NO
# hardware AEC reference loop; "MR" makes the AFE treat the 2nd (left-ear) mic
# as the speaker-echo reference, so the AEC cancels the talker's own voice
# (muffled wake word to the server + nearly impossible to wake at distance).
$avSrc3 = Join-Path $proj 'managed_components\jason-mao__av_processor\src\audio_processor.c'
if (Test-Path $avSrc3) {
    $c3 = Get-Content -Path $avSrc3 -Raw -Encoding UTF8
    if ($c3 -notmatch 'VoCat-S31 has no AEC reference loop') {
        $c3 = $c3.Replace(
            "    audio_recorder.afe_cfg->aec_init = (strchr(audio_manager.config.mic_layout, 'R') != NULL);",
            "    audio_recorder.afe_cfg->aec_init = false; /* VoCat-S31 has no AEC reference loop (ch1 is a 2nd real mic) */`n    if (!audio_recorder.afe_cfg->aec_init) {`n        ESP_LOGI(TAG, `"AEC disabled: VoCat-S31 has no AEC reference loop (ch1 is a 2nd real mic)`");`n    }")
        Set-Content -Path $avSrc3 -Value $c3 -Encoding UTF8 -NoNewline
        Write-Output "[patch] av_processor: AEC force-off applied" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] av_processor: AEC force-off already present" | Tee-Object -FilePath $log -Append
    }
}

# --------------------------------------------------------------------------

# es8389: power MICBIAS (0x62 bit7) for the VoCat-S31 electret mics. Without
# this the ADC only captures digital silence (wake word never fires). Component
# manager can overwrite managed_components on re-fetch, so re-apply from patches/.
$esSrc   = Join-Path $proj 'managed_components\espressif__esp_codec_dev\device\es8389\es8389.c'
$esPatch = Join-Path $proj 'patches\esp_codec_dev\device\es8389\es8389.c'
if ((Test-Path $esSrc) -and (Test-Path $esPatch)) {
    $cs = Get-Content -Path $esSrc -Raw -Encoding UTF8
    if ($cs -notmatch 'ES8389_ANALOG_CONTROL_REG0x62, 0x87') {
        Copy-Item -Path $esPatch -Destination $esSrc -Force
        Write-Output "[patch] es8389.c: mic bias 0x87 applied from patches/" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] es8389.c: mic bias fix already present" | Tee-Object -FilePath $log -Append
    }
}
$esRegSrc   = Join-Path $proj 'managed_components\espressif__esp_codec_dev\device\es8389\es8389_reg.h'
$esRegPatch = Join-Path $proj 'patches\es_codec_dev\device\es8389\es8389_reg.h'
if ((Test-Path $esRegSrc) -and (Test-Path $esRegPatch)) {
    $cs = Get-Content -Path $esRegSrc -Raw -Encoding UTF8
    if ($cs -notmatch 'ES8389_ADC_INSEL_L_SINGLE_MIC1P') {
        Copy-Item -Path $esRegPatch -Destination $esRegSrc -Force
        Write-Output "[patch] es8389_reg.h: single-ended MIC1P mux applied from patches/" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] es8389_reg.h: single-ended mux already present" | Tee-Object -FilePath $log -Append
    }
}

# emote_op: keep transient status overlays disabled while showing assistant
# speech in the EAF toast label. Also keep missing cosmetic icon lookups quiet.
# Re-apply from patches/ when the component manager refreshes managed_components.
$emSrc   = Join-Path $proj 'managed_components\espressif2022__esp_emote_expression\src\emote_op.c'
$emPatch = Join-Path $proj 'patches\espressif2022__esp_emote_expression\src\emote_op.c'
if ((Test-Path $emSrc) -and (Test-Path $emPatch)) {
    $srcHash = (Get-FileHash -Path $emSrc -Algorithm SHA256).Hash
    $patchHash = (Get-FileHash -Path $emPatch -Algorithm SHA256).Hash
    if ($srcHash -ne $patchHash) {
        Copy-Item -Path $emPatch -Destination $emSrc -Force
        Write-Output "[patch] emote_op: EAF speech bubble applied from patches/" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] emote_op: EAF speech bubble already present" | Tee-Object -FilePath $log -Append
    }
}

# emote_init: mark the handle initialized as soon as the gfx display exists so
# the first flush-complete notification (which can arrive before the default
# label is created) is not rejected, which used to wedge gfx_render forever.
$emInitSrc   = Join-Path $proj 'managed_components\espressif2022__esp_emote_expression\src\emote_init.c'
$emInitPatch = Join-Path $proj 'patches\espressif2022__esp_emote_expression\src\emote_init.c'
if ((Test-Path $emInitSrc) -and (Test-Path $emInitPatch)) {
    $srcHash = (Get-FileHash -Path $emInitSrc -Algorithm SHA256).Hash
    $patchHash = (Get-FileHash -Path $emInitPatch -Algorithm SHA256).Hash
    if ($srcHash -ne $patchHash) {
        Copy-Item -Path $emInitPatch -Destination $emInitSrc -Force
        Write-Output "[patch] emote_init: early-init flush race fix applied from patches/" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] emote_init: early-init flush race fix already present" | Tee-Object -FilePath $log -Append
    }
}

# esp_emote_gfx: keep the label shape/arrow renderer in sync with the managed
# component.  The component manager may refresh this directory during a build,
# so compare hashes instead of relying on a one-time marker.
$gfxRoot = Join-Path $proj 'managed_components\espressif2022__esp_emote_gfx'
$gfxPatchRoot = Join-Path $proj 'patches\espressif2022__esp_emote_gfx'
$gfxPatchFiles = @(
    'include\widget\gfx_label.h',
    'src\widget\label\gfx_draw_label_priv.h',
    'src\widget\label\gfx_label_obj.c',
    'src\widget\label\gfx_label.c',
    'src\widget\label\gfx_draw_label.c',
    'src\widget\anim\gfx_anim.c',
    'src\core\display\gfx_render.c'
)
foreach ($relativePath in $gfxPatchFiles) {
    $gfxSrc = Join-Path $gfxRoot $relativePath
    $gfxPatch = Join-Path $gfxPatchRoot $relativePath
    if ((Test-Path $gfxSrc) -and (Test-Path $gfxPatch)) {
        $srcHash = (Get-FileHash -Path $gfxSrc -Algorithm SHA256).Hash
        $patchHash = (Get-FileHash -Path $gfxPatch -Algorithm SHA256).Hash
        if ($srcHash -ne $patchHash) {
            Copy-Item -Path $gfxPatch -Destination $gfxSrc -Force
            Write-Output "[patch] esp_emote_gfx ${relativePath}: speech bubble renderer applied" | Tee-Object -FilePath $log -Append
        }
    }
}

# esp_gmf_ai_audio: add the "voice output enabled" gate so the recorder
# pipeline can be paused while the server TTS plays (mirrors the upstream
# client's kVoiceProcessingEnabled). Without it the AFE(FEED) ringbuffer
# overflows and the recorder task starves afe_fetch on the same core.
$afeSrcRoot = Join-Path $proj 'managed_components\espressif__gmf_ai_audio'
$afePatchRoot = Join-Path $proj 'patches\espressif__gmf_ai_audio'
$afePatchFiles = @(
    'src\esp_gmf_afe.c',
    'include\esp_gmf_afe.h'
)
foreach ($relativePath in $afePatchFiles) {
    $afeSrc = Join-Path $afeSrcRoot $relativePath
    $afePatch = Join-Path $afePatchRoot $relativePath
    if ((Test-Path $afeSrc) -and (Test-Path $afePatch)) {
        $srcHash = (Get-FileHash -Path $afeSrc -Algorithm SHA256).Hash
        $patchHash = (Get-FileHash -Path $afePatch -Algorithm SHA256).Hash
        if ($srcHash -ne $patchHash) {
            Copy-Item -Path $afePatch -Destination $afeSrc -Force
            Write-Output "[patch] gmf_ai_audio ${relativePath}: voice-output gate applied" | Tee-Object -FilePath $log -Append
        }
    }
}

# emote_setup: use the 14px 1bpp Puhui Basic font (from 78__xiaozhi-fonts) for
# the toast caption so Chinese glyphs render crisp on the 360x360 round screen.
# The bundled 20px 4bpp font is blurry at this resolution and too wide.
$emSetupSrc   = Join-Path $proj 'managed_components\espressif2022__esp_emote_expression\src\emote_setup.c'
$emSetupPatch = Join-Path $proj 'patches\espressif2022__esp_emote_expression\src\emote_setup.c'
if ((Test-Path $emSetupSrc) -and (Test-Path $emSetupPatch)) {
    $srcHash = (Get-FileHash -Path $emSetupSrc -Algorithm SHA256).Hash
    $patchHash = (Get-FileHash -Path $emSetupPatch -Algorithm SHA256).Hash
    if ($srcHash -ne $patchHash) {
        Copy-Item -Path $emSetupPatch -Destination $emSetupSrc -Force
        Write-Output "[patch] emote_setup: 14px 1bpp toast font applied from patches/" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] emote_setup: 14px 1bpp toast font already present" | Tee-Object -FilePath $log -Append
    }
}

# emote_expression idf_component.yml: declare the dependency on xiaozhi-fonts
# so emote_setup.c can reference font_puhui_14_1 without an undefined-symbol link error.
$emYmlSrc   = Join-Path $proj 'managed_components\espressif2022__esp_emote_expression\idf_component.yml'
$emYmlPatch = Join-Path $proj 'patches\espressif2022__esp_emote_expression\idf_component.yml'
if ((Test-Path $emYmlSrc) -and (Test-Path $emYmlPatch)) {
    $srcHash = (Get-FileHash -Path $emYmlSrc -Algorithm SHA256).Hash
    $patchHash = (Get-FileHash -Path $emYmlPatch -Algorithm SHA256).Hash
    if ($srcHash -ne $patchHash) {
        Copy-Item -Path $emYmlPatch -Destination $emYmlSrc -Force
        Write-Output "[patch] emote_expression yml: xiaozhi-fonts dep applied from patches/" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] emote_expression yml: xiaozhi-fonts dep already present" | Tee-Object -FilePath $log -Append
    }
}

# sc101iot: SCCB detect retry + XCLK settle so a slow-but-present sensor is not
# falsely reported missing. Component manager can overwrite managed_components
# on re-fetch, so re-apply from patches/.
$scSrc   = Join-Path $proj 'managed_components\espressif__esp_cam_sensor\sensors\sc101iot\sc101iot.c'
$scPatch = Join-Path $proj 'patches\espressif__esp_cam_sensor\sensors\sc101iot\sc101iot.c'
if ((Test-Path $scSrc) -and (Test-Path $scPatch)) {
    $cs = Get-Content -Path $scSrc -Raw -Encoding UTF8
    if ($cs -notmatch 'SCCB detect retry \+ XCLK settle') {
        Copy-Item -Path $scPatch -Destination $scSrc -Force
        Write-Output "[patch] sc101iot: SCCB retry+settle applied from patches/" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] sc101iot: SCCB retry+settle already present" | Tee-Object -FilePath $log -Append
    }
}

# esp_video_init: SCCB bus scan diagnostic on camera detect failure (so we can
# tell whether 0x68 is present on the shared I2C/SCCB bus). Re-apply on re-fetch.
$viSrc   = Join-Path $proj 'managed_components\espressif__esp_video\src\esp_video_init.c'
$viPatch = Join-Path $proj 'patches\espressif__esp_video\src\esp_video_init.c'
if ((Test-Path $viSrc) -and (Test-Path $viPatch)) {
    $cs = Get-Content -Path $viSrc -Raw -Encoding UTF8
    if ($cs -notmatch 'SCCB bus scan diagnostic') {
        Copy-Item -Path $viPatch -Destination $viSrc -Force
        Write-Output "[patch] esp_video_init: SCCB bus scan applied from patches/" | Tee-Object -FilePath $log -Append
    } else {
        Write-Output "[patch] esp_video_init: SCCB bus scan already present" | Tee-Object -FilePath $log -Append
    }
}

# esp_emote_assets: keep the manifest aligned with the nine unique EAF files.
# The component is managed and may be re-fetched, so restore all resolution
# manifests from the tracked patches before generating assets.bin.
$emoteAssetRoot = Join-Path $proj 'managed_components\espressif2022__esp_emote_assets'
$emotePatchRoot = Join-Path $proj 'patches\espressif2022__esp_emote_assets'
foreach ($resolution in @('1024_600', '320_240', '360_360')) {
    $manifestSrc = Join-Path $emoteAssetRoot "$resolution\emote.json"
    $manifestPatch = Join-Path $emotePatchRoot "$resolution\emote.json"
    if ((Test-Path $manifestSrc) -and (Test-Path $manifestPatch)) {
        $srcHash = (Get-FileHash -Path $manifestSrc -Algorithm SHA256).Hash
        $patchHash = (Get-FileHash -Path $manifestPatch -Algorithm SHA256).Hash
        if ($srcHash -ne $patchHash) {
            Copy-Item -Path $manifestPatch -Destination $manifestSrc -Force
            Write-Output "[patch] emote_assets ${resolution}: nine unique EAF manifest applied" | Tee-Object -FilePath $log -Append
        } else {
            Write-Output "[patch] emote_assets ${resolution}: manifest already current" | Tee-Object -FilePath $log -Append
        }
    }
}

Write-Output "===== [$(Get-Date)] BUILD ONLY =====" | Tee-Object -FilePath $log -Append
& "$pyEnv\Scripts\python.exe" "$idfPath\tools\idf.py" build 2>&1 | Tee-Object -FilePath $log -Append
Write-Output "BUILD EXIT: $LASTEXITCODE" | Tee-Object -FilePath $log -Append
