cd "C:\Users\Ash\Documents\Unity 2 PS2"

cmake --build --preset ps2-release 2>&1 |
    Select-String -CaseSensitive "error|FAILED" |
    Select-Object -First 3

$env:EMU_EXTRA_FILES = "build/fogscene.p2b"
$env:EMU_TIMEOUT     = "60"
bash ./tools/ci/run-emu-test.sh build/ps2-release/samples/16-fog/16-fog.elf PS2UR_TOKEN_FOG_OK 2>&1 |
    Select-String -CaseSensitive "PASS|FAIL"

Select-String -CaseSensitive "16-fog\] cube|FOG_FAIL|FOG_OK" "$env:TEMP\ps2-emu-stage\emulog.txt" |
    Select-Object -First 8 |
    ForEach-Object Line