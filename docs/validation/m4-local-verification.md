# M4 local verification record

Date: 2026-08-30  
Source: `feature/m4-single-multi-disk` at the verification run (commit `d219900`).

This record covers the deterministic local checks only. It does not claim that
the real AIM/NTFS VMware acceptance has been performed.

## MSVC Release clean build and focused M4 run (T18)

The Release verification was performed in a fresh build directory after
explicitly loading the VS2022 x64 toolchain with `vcvars64.bat`:

```powershell
$bat = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$build = 'out/build/t18-release-clean2'
cmd.exe /d /s /c "call `"$bat`" && cmake -S . -B `"$build`" -G `"Ninja Multi-Config`" -DBUILD_TESTING=ON"
```

Exit code: **0** (clean configure; MSVC 19.44.35228.0, x64).

```powershell
cmd.exe /d /s /c "call `"$bat`" && cmake --build `"$build`" --config Release --parallel"
```

Exit code: **0**; all **465/465** project targets compiled and linked,
including `myrd.exe` and every M4 target/test executable.

The focused M4 Release test command was:

```powershell
cmd.exe /d /s /c "call `"$bat`" && ctest --test-dir `"$build`" -C Release --output-on-failure -R `"^(unit[.]m4[.](cli|windows_volume|disk_runtime|run_policy|persistence_adapter|provision_script)|integration[.]m4[.](run_loop_runtime|product_composition|multi_instance))$`""
```

Exit code: **0**; **9/9 passed, 0 failed**.

The Release product and focused-test artifact SHA-256 values are:

```text
src/Release/myrd.exe  080222F692536CA38A0D0B20033BC81BB81C9C6953151DCB25C3B78E181BAAA7
tests/Release/myrd_m4_cli_tests.exe 6569700FD74D1F618A4ADA46D4BF05B96769B41D14E8E93B7D9C178A8700D9EF
tests/Release/myrd_m4_windows_volume_tests.exe BC119C3D710539DFFDD0CFF3501EFEA0F721B3DB560F1546B14209A162E7E0DB
tests/Release/myrd_m4_disk_runtime_tests.exe F7F3CDF920A6A417F6819BF0F274CFF600096608CD72A73C6F4DF46D47038978
tests/Release/myrd_m4_run_policy_tests.exe 1EC5C917C53B7CF9A5C9D7D6CFC9A129A36B357988B65B94596E881DD9F6E311
tests/Release/myrd_m4_run_loop_runtime_tests.exe 05CF349AB9472CED4F4588BC405A8C67622F812A442340D1FAE56EED9FE9B785
tests/Release/myrd_m4_persistence_adapter_tests.exe DC8F39822625285AF41C2724CFD8C5F733392A43189DE4D4B5DE0ECC66DF5674
tests/Release/myrd_m4_provisioner_policy_tests.exe DC86B79A29BAD742B30166453F6A49A1EA0FABD9B0B0EDD575CBCA260BF7A14E
tests/Release/myrd_m4_product_composition_tests.exe 9E6060DF7A2A397CE62119058EB539B43787E61A75C29689577DB000BD1F5C73
tests/Release/myrd_m4_multi_instance_tests.exe 03F806152DF2C5F5543A2CC6DD8EE5BA62014C0C2C9B56F403C8E1B691A4FE70
```

`unit.m4.provision_script` is a script-contract test implemented by the
`myrd_m4_provisioner_policy_tests.exe` target; the test name is the artifact
reported by CTest, not a standalone binary.

This verifies compilation and deterministic test seams only. It does not
claim real AIM/NTFS VMware qualification.

## MSVC Debug focused M4 run

The following command was run from an MSVC-enabled PowerShell session:

```powershell
ctest --test-dir out/build/msvc -C Debug --output-on-failure `
  -R '^(unit\.m4\.(cli|windows_volume|disk_runtime|run_policy|persistence_adapter|provision_script)|integration\.m4\.(run_loop_runtime|product_composition|multi_instance))$'
```

Result: **9/9 passed**.

Passed tests:

- `unit.m4.cli`
- `unit.m4.windows_volume`
- `unit.m4.disk_runtime`
- `unit.m4.run_policy`
- `integration.m4.run_loop_runtime`
- `unit.m4.persistence_adapter`
- `unit.m4.provision_script`
- `integration.m4.product_composition`
- `integration.m4.multi_instance`

The product-composition tests use test-only seams and do not create a real AIM
device or Windows volume. They cover executable help/error output, preflight
ordering, driver-unavailable zero-write, and create/recover entry dispatch.
The multi-instance tests use fake lifecycle operations and cover independent
bundles, drive letters, AIM identities, generations, lock rejection, recovery
without provisioning, and failure isolation.

## Release build

An MSVC-enabled PowerShell session also completed:

```powershell
cmake --build out/build/msvc --config Release --target myrd_m4_product_composition_tests
```

The Release product executable was produced at:

```text
out/build/msvc/src/Release/myrd.exe
```

The Release build was not used as evidence of real-device qualification.

## Repository hygiene

The verification run included:

```powershell
git diff --check
```

Result: passed with no whitespace errors. The verification record itself is
documentation only; no legacy Task23 profile/operation suite results are
combined with the new M4 focused result. Any failures in those historical
tests must be tracked separately and cannot invalidate or inflate this focused
M4 count.

## Remaining qualification boundary

The following still require a controlled VM run and are intentionally outside
this local record:

- real single-disk create and recovery;
- manual VMware Reset or power-cycle recovery;
- two real disks running concurrently;
- real cross-instance cleanup and residual-device inspection;
- approved-driver/AIM/NTFS environment evidence.

## Post-integration Release refresh (2026-08-31)

This refresh supersedes the earlier T18 artifact hashes above. It was run from
the current feature HEAD `2df871e8e07436a3d9e8515c7aac2dbeda8a9205` after the
VM runbook/report integration. No VM, driver, AIM device, or Windows volume
operation was performed.

The build used a new, clean directory and an explicitly loaded VS2022 x64
toolchain:

```powershell
$bat = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$build = 'out/build/t18-release-refresh-20260831'
cmd.exe /d /s /c "call `"$bat`" && cmake -S . -B `"$build`" -G `"Ninja Multi-Config`" -DBUILD_TESTING=ON"
```

Exit code: **0** (clean configure; MSVC 19.44.35228.0, x64).

```powershell
cmd.exe /d /s /c "call `"$bat`" && cmake --build `"$build`" --config Release --parallel 4"
```

Exit code: **0**; all **465/465** project targets compiled and linked.

The focused Release test command was:

```powershell
ctest --test-dir out/build/t18-release-refresh-20260831 -C Release --output-on-failure -R '^(unit[.]m4[.](cli|windows_volume|disk_runtime|run_policy|persistence_adapter|provision_script)|integration[.]m4[.](run_loop_runtime|product_composition|multi_instance))$'
```

Exit code: **0**; **9/9 passed, 0 failed** (total test time 1.08 seconds).

The refreshed Release artifact SHA-256 values are:

```text
out/build/t18-release-refresh-20260831/src/Release/myrd.exe  3C538833270BCE3E63F65051F14EE3C353314373541723E3819FF81ECF2BD6E7
out/build/t18-release-refresh-20260831/tests/Release/myrd_m4_cli_tests.exe  6F94893419B0BC4531F8E8742357C4852D9F5BC1ADF666840728CC6240828A2D
out/build/t18-release-refresh-20260831/tests/Release/myrd_m4_windows_volume_tests.exe  2BDE2CE3FC57D76C5A9208ED0115478B77B77164708B66400226235206DC985B
out/build/t18-release-refresh-20260831/tests/Release/myrd_m4_disk_runtime_tests.exe  F72E138712D678A1AA9298BA775A5B8F47C23FB13B5F0B83A4E11EBD67301288
out/build/t18-release-refresh-20260831/tests/Release/myrd_m4_run_policy_tests.exe  460D1DE399E4FBFB9EFA37E5B01C2395F1A9AA0F2905DE49DEB30764E09F7ED3
out/build/t18-release-refresh-20260831/tests/Release/myrd_m4_run_loop_runtime_tests.exe  B1E47D66393D39C25E04EC2C590ACDB37FD92B56FB1CE92782DF188650492065
out/build/t18-release-refresh-20260831/tests/Release/myrd_m4_persistence_adapter_tests.exe  446A9039985EE991253EDB94D4B07A2DC6DFB7E585F7F7C43FA6D6EE627CC355
out/build/t18-release-refresh-20260831/tests/Release/myrd_m4_provisioner_policy_tests.exe  E46BDF5B6C3FB9E06487ECD9682DF67E9244AB024C4BB6389B79D9687F7F332A
out/build/t18-release-refresh-20260831/tests/Release/myrd_m4_product_composition_tests.exe  DC1C326FDAB43B65A8BBA92D739427AC2E507D11DFB0642C63729069A95E9ED0
out/build/t18-release-refresh-20260831/tests/Release/myrd_m4_multi_instance_tests.exe  13ECCB347DC0EAAE02C4590B755265E4597EE79E146DD182CAD9F523EBD05B83
```

The prior T18 Release hash set is retained for historical traceability only;
the list in this section remains the authoritative artifact hash set for the
unchanged production sources. Later documentation-only commits do not alter
the executable bytes; the current source commit for a qualification kit must
still be recorded separately in that kit's manifest.
